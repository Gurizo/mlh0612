// eyefight: a staring-contest game refereed by computer vision.
// Built on the Presage SmartSpectra C++ SDK.
//
// Any browser (phone/laptop) captures its own camera via getUserMedia and
// streams JPEG frames to this server, which pushes them into SmartSpectra via
// CustomInput. The SDK's per-frame blink detection ends the round. Scores
// persist to a JSON leaderboard with eye-crop avatars.
//
//   GET  /            — game UI (web/index.html)
//   GET  /events      — SSE: game state + leaderboard (10 Hz). No video.
//   GET  /avatar?id=  — a consented eye-crop PNG (eyes only, never the face)
//   POST /api/join    — {"name":"..."} -> {"token":"..."} or 409 when busy
//   POST /api/frame   — JPEG body + X-TS header; used only for on-device
//                       detection + the eye crop. Frames are never re-served.
//   POST /api/leave   — ?token=... forfeits / ends the session
//
// Usage:
//   ./eyefight --api_key=YOUR_KEY [--port=8428] [--web_root=web]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <string>
#include <vector>

#include <absl/flags/flag.h>
#include <absl/flags/parse.h>

#include <smartspectra/smartspectra.h>
#include <smartspectra/smartspectra_config.h>
#include <smartspectra/smartspectra_types.h>

#include "httplib.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_FAILURE_STRINGS
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace spectra = presage::smartspectra;
using presage::smartspectra::MetricType;

ABSL_FLAG(std::string, api_key, "", "Presage API key (falls back to $SMARTSPECTRA_API_KEY).");
ABSL_FLAG(int, port, 8428, "HTTP port.");
ABSL_FLAG(std::string, host, "0.0.0.0", "Bind address (0.0.0.0 allows LAN play).");
ABSL_FLAG(std::string, web_root, "web", "Directory with the game static files.");
ABSL_FLAG(std::string, leaderboard, "leaderboard.json", "Leaderboard persistence file.");
ABSL_FLAG(bool, fake, false,
          "Dev mode: skip the SmartSpectra SDK, synthesize a pulse, and accept "
          "POST /api/debug_blink to end rounds. For UI work without an API key.");

namespace {

constexpr double kCountdownSeconds = 3.0;
constexpr double kBlinkGraceSeconds = 0.5;   // ignore blink edges right at round start
constexpr double kSessionTimeoutSeconds = 6; // no frames -> player vanished
constexpr double kDoneLingerSeconds = 12;    // result shown before lobby reopens
constexpr int kFrameDim = 480;               // every frame must be exactly this square;
                                             // the SDK's optical flow spans the whole stream
constexpr double kFaceFreshSeconds = 1.0;    // face required this recently to start staring
constexpr double kFaceLostSeconds = 1.2;     // face gone this long mid-stare -> round over
constexpr double kCountdownMaxSeconds = 25;  // never showed a face -> back to lobby

double NowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string JsonEscape(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (static_cast<unsigned char>(c) >= 0x20) out += c;
    }
    return out;
}

struct BoardEntry {
    std::string name;
    double score = 0;     // best staring time, seconds
    std::string when;     // ISO date of last play
    std::string avatar;   // id into g.avatars ("" = eyeless default profile)
};

enum class Phase { kIdle, kCountdown, kStaring, kDone };

const char* PhaseName(Phase p) {
    switch (p) {
        case Phase::kIdle: return "idle";
        case Phase::kCountdown: return "countdown";
        case Phase::kStaring: return "staring";
        case Phase::kDone: return "done";
    }
    return "idle";
}

struct GameState {
    std::mutex mu;

    // from the SDK
    std::string hint;
    bool prev_blink_detected = false;
    double last_blink_t = -1;

    // spectator preview (the player's own JPEGs, re-broadcast)
    std::string jpeg;
    uint64_t jpeg_seq = 0;

    // session
    Phase phase = Phase::kIdle;
    std::string token;
    std::string player;
    bool want_eyes = true;
    double phase_start = 0;
    double stare_start = 0;
    double last_frame_t = 0;
    double final_score = -1;
    int final_rank = -1;
    double final_best = -1;
    std::string final_avatar;
    std::string end_reason;     // "blink" or "lost"
    double face_t = 0;          // last time the SDK saw a face

    // per-eye boxes from the latest face landmarks (raw landmark coords;
    // whether they are normalized or pixel-space is decided at crop time)
    struct EyeBox { float cx = 0, cy = 0, w = 0, h = 0; };
    bool eye_valid = false;
    double eye_t = 0;
    EyeBox eyes[2];        // [0] = image-left eye, [1] = image-right eye
    std::string eye_frame; // jpeg snapshot taken when eyes were last detected,
                           // so the avatar crop matches the box even if the
                           // newest frame is stale/post-blink

    // avatar id -> jpeg bytes (eye strips)
    std::map<std::string, std::string> avatars;

    // virtual-clock mapping for CustomInput (compresses idle gaps; the SDK
    // rejects forward timestamp gaps > 2 s and non-monotonic values). Frames
    // may arrive in parallel/out-of-order over the tunnel, so these are guarded
    // by g_feed_mu (not g.mu) and a stale frame is dropped to keep Send() monotonic.
    std::string feed_token;        // session the clock below is calibrated to
    int64_t client_first_us = 0;
    int64_t last_client_us = 0;    // newest client timestamp fed to the SDK
    int64_t virtual_base_us = 0;
    int64_t last_sent_us = 0;
    int feed_w = 0, feed_h = 0;    // frame size locked per session — the SDK's
                                   // optical-flow tracker crashes if it changes

    std::vector<BoardEntry> board;

    // spectator "distract" reactions: emojis flung at the active player. Stored
    // by index (client maps index -> emoji); ephemeral, only the recent ones
    // are streamed to the player.
    struct React { uint64_t id; int idx; double t; };
    std::deque<React> reacts;
    uint64_t react_seq = 0;
};

constexpr int kEmojiCount = 8;  // must match the EMOJIS array in web/index.html

GameState g;
std::mutex g_feed_mu;  // serializes frame decode-order -> SDK Send (monotonic)
spectra::SmartSpectra* g_smart_spectra = nullptr;
httplib::Server* g_server = nullptr;

void HandleSignal(int) {
    if (g_smart_spectra) (void)g_smart_spectra->Stop();
    if (g_server) g_server->stop();
}

std::string RandomToken() {
    static std::mt19937_64 rng(std::random_device{}());
    char buf[33];
    snprintf(buf, sizeof(buf), "%016llx%016llx",
             static_cast<unsigned long long>(rng()),
             static_cast<unsigned long long>(rng()));
    return buf;
}

std::string TodayIso() {
    char buf[16];
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

// --- base64 (for persisting avatar JPEGs inside leaderboard.json) -----------

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const std::string& in) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        uint32_t v = static_cast<uint8_t>(in[i]) << 16;
        if (i + 1 < in.size()) v |= static_cast<uint8_t>(in[i + 1]) << 8;
        if (i + 2 < in.size()) v |= static_cast<uint8_t>(in[i + 2]);
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += i + 1 < in.size() ? kB64[(v >> 6) & 63] : '=';
        out += i + 2 < in.size() ? kB64[v & 63] : '=';
    }
    return out;
}

std::string Base64Decode(const std::string& in) {
    static int8_t lut[256];
    static bool init = false;
    if (!init) {
        memset(lut, -1, sizeof(lut));
        for (int i = 0; i < 64; ++i) lut[static_cast<uint8_t>(kB64[i])] = static_cast<int8_t>(i);
        init = true;
    }
    std::string out;
    uint32_t v = 0;
    int bits = 0;
    for (char c : in) {
        const int8_t d = lut[static_cast<uint8_t>(c)];
        if (d < 0) continue;
        v = (v << 6) | d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((v >> bits) & 0xFF);
        }
    }
    return out;
}

// --- leaderboard persistence (single small JSON file) -----------------------

void SaveBoardLocked() {
    std::ofstream f(absl::GetFlag(FLAGS_leaderboard), std::ios::trunc);
    f << "[\n";
    for (size_t i = 0; i < g.board.size(); ++i) {
        const auto& e = g.board[i];
        f << "  {\"name\":\"" << JsonEscape(e.name) << "\",\"score\":" << e.score
          << ",\"when\":\"" << e.when << "\"";
        const auto it = g.avatars.find(e.avatar);
        if (it != g.avatars.end()) {
            f << ",\"avatar_b64\":\"" << Base64Encode(it->second) << "\"";
        }
        f << "}" << (i + 1 < g.board.size() ? ",\n" : "\n");
    }
    f << "]\n";
}

// Minimal parser for the file this process writes (and hand-edited variants).
void LoadBoard() {
    std::ifstream f(absl::GetFlag(FLAGS_leaderboard));
    if (!f) return;
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t pos = 0;
    while ((pos = text.find("{\"name\":\"", pos)) != std::string::npos) {
        pos += 9;
        const size_t name_end = text.find('"', pos);
        if (name_end == std::string::npos) break;
        size_t entry_end = text.find('}', name_end);
        if (entry_end == std::string::npos) entry_end = text.size();
        const auto field = [&](const char* key) -> size_t {
            const size_t p = text.find(key, name_end);
            return p < entry_end ? p : std::string::npos;
        };
        BoardEntry e;
        e.name = text.substr(pos, name_end - pos);
        const size_t s = field("\"score\":");
        const size_t w = field("\"when\":\"");
        const size_t a = field("\"avatar_b64\":\"");
        if (s == std::string::npos) break;
        e.score = atof(text.c_str() + s + 8);
        if (w != std::string::npos) {
            const size_t w_end = text.find('"', w + 8);
            e.when = text.substr(w + 8, w_end - w - 8);
        }
        if (a != std::string::npos) {
            const size_t a_start = a + 14;
            const size_t a_end = text.find('"', a_start);
            if (a_end != std::string::npos) {
                e.avatar = RandomToken().substr(0, 16);
                g.avatars[e.avatar] = Base64Decode(text.substr(a_start, a_end - a_start));
            }
        }
        g.board.push_back(std::move(e));
        pos = entry_end;
    }
    std::sort(g.board.begin(), g.board.end(),
              [](const BoardEntry& a, const BoardEntry& b) { return a.score > b.score; });
}

// --- floating-eyes avatars ---------------------------------------------------

// Crop each eye separately from the player's latest frame, mask them into
// soft ovals, and composite them side by side on a transparent PNG: two eyes,
// no face. Returns PNG bytes, or "" on failure.
std::string CropEyesLocked() {
    if (!g.eye_valid || g.eye_frame.empty()) {
        fprintf(stderr, "[avatar] skipped: eye_valid=%d frame_bytes=%zu\n",
                g.eye_valid, g.eye_frame.size());
        return "";
    }

    int w = 0, h = 0, channels = 0;
    unsigned char* rgb = stbi_load_from_memory(
        reinterpret_cast<const unsigned char*>(g.eye_frame.data()),
        static_cast<int>(g.eye_frame.size()), &w, &h, &channels, 3);
    if (!rgb) { fprintf(stderr, "[avatar] decode failed\n"); return ""; }

    // Landmarks may be normalized [0,1] or pixel-space depending on pipeline.
    GameState::EyeBox e[2] = {g.eyes[0], g.eyes[1]};
    if (e[0].cx <= 2.0f && e[1].cx <= 2.0f && e[0].cy <= 2.0f && e[1].cy <= 2.0f) {
        for (auto& b : e) { b.cx *= w; b.cy *= h; b.w *= w; b.h *= h; }
    }

    // one common box size so both eyes come out identical
    const int bw = static_cast<int>(std::max(e[0].w, e[1].w) * 1.7);
    const int bh = static_cast<int>(std::max(
        {e[0].h * 2.2, e[1].h * 2.2, bw * 0.55}));  // blink-proof minimum height
    if (bw < 12 || bh < 8 || bw > w || bh > h) {
        fprintf(stderr, "[avatar] degenerate box: bw=%d bh=%d frame=%dx%d "
                "eyesL=(%.3f,%.3f,%.3f,%.3f) eyesR=(%.3f,%.3f,%.3f,%.3f)\n",
                bw, bh, w, h, e[0].cx, e[0].cy, e[0].w, e[0].h,
                e[1].cx, e[1].cy, e[1].w, e[1].h);
        stbi_image_free(rgb);
        return "";
    }

    const int gap = bw / 3;
    const int out_w = bw * 2 + gap, out_h = bh;
    std::vector<uint8_t> out(static_cast<size_t>(out_w) * out_h * 4, 0);  // transparent

    for (int k = 0; k < 2; ++k) {
        int x0 = static_cast<int>(e[k].cx) - bw / 2;
        int y0 = static_cast<int>(e[k].cy) - bh / 2;
        x0 = std::max(0, std::min(x0, w - bw));
        y0 = std::max(0, std::min(y0, h - bh));
        const int ox = k * (bw + gap);
        for (int y = 0; y < bh; ++y) {
            for (int x = 0; x < bw; ++x) {
                // almond/eye-lens mask: the lid height tapers to a point at the
                // left/right corners, so the crop reads as an eye, not a blob.
                const double nx = (x + 0.5) / bw * 2 - 1, ny = (y + 0.5) / bh * 2 - 1;
                const double lid = 1.0 - nx * nx;          // 1 at center -> 0 at corners
                if (lid <= 0.0) continue;
                const double edge = std::fabs(ny) / lid;   // 0 at the eye-line, 1 at the lid
                if (edge >= 1.0) continue;
                // feather the lids AND the pointed corners for a soft edge
                const double a = std::min({1.0, (1.0 - edge) * 2.5, lid * 4.0});
                const uint8_t* src = rgb + (static_cast<size_t>(y0 + y) * w + x0 + x) * 3;
                uint8_t* dst = out.data() + (static_cast<size_t>(y) * out_w + ox + x) * 4;
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                dst[3] = static_cast<uint8_t>(a * 255);
            }
        }
    }
    stbi_image_free(rgb);

    std::string png;
    auto sink = [](void* ctx, void* data, int size) {
        static_cast<std::string*>(ctx)->append(static_cast<char*>(data), size);
    };
    if (!stbi_write_png_to_func(sink, &png, out_w, out_h, 4, out.data(), out_w * 4)) return "";
    return png;
}

// MediaPipe-facemesh indices: eye corners and lids, per eye. Used when the
// SDK delivers a dense (>=468 point) mesh; sparser meshes fall back to two
// boxes placed heuristically inside the whole-landmark bounding box.
//
// `open` gates the capture: we only snapshot the avatar frame when the eyes are
// open, so the profile shows open eyes (the round ends on a blink, so the very
// last frame is always shut). On a blink we keep the previous open-eyed frame.
void UpdateEyeBoxFromLandmarks(const spectra::Metrics& m, double now, bool open) {
    if (!open) return;
    if (!m.has_face() || m.face().landmarks_size() == 0) return;
    const auto& lm = m.face().landmarks(m.face().landmarks_size() - 1);
    const int n = lm.value_size();
    if (n == 0) return;

    GameState::EyeBox boxes[2];
    if (n >= 468) {
        // {outer corner, inner corner, top lid, bottom lid}
        constexpr int kRight[4] = {33, 133, 159, 145};
        constexpr int kLeft[4] = {263, 362, 386, 374};
        const int* idx[2] = {kRight, kLeft};
        for (int k = 0; k < 2; ++k) {
            const auto& a = lm.value(idx[k][0]);
            const auto& b = lm.value(idx[k][1]);
            const auto& t = lm.value(idx[k][2]);
            const auto& u = lm.value(idx[k][3]);
            boxes[k].cx = (a.x() + b.x()) / 2;
            boxes[k].cy = (t.y() + u.y()) / 2;
            boxes[k].w = std::abs(b.x() - a.x());
            boxes[k].h = std::abs(u.y() - t.y());
        }
    } else {
        float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
        for (int i = 0; i < n; ++i) {
            const auto& p = lm.value(i);
            min_x = std::min(min_x, p.x()); max_x = std::max(max_x, p.x());
            min_y = std::min(min_y, p.y()); max_y = std::max(max_y, p.y());
        }
        // eyes sit roughly 40% down the face box, ~30/70% across
        const float fw = max_x - min_x, fh = max_y - min_y;
        for (int k = 0; k < 2; ++k) {
            boxes[k].cx = min_x + fw * (k == 0 ? 0.30f : 0.70f);
            boxes[k].cy = min_y + fh * 0.40f;
            boxes[k].w = fw * 0.24f;
            boxes[k].h = fh * 0.10f;
        }
    }
    if (boxes[0].cx > boxes[1].cx) std::swap(boxes[0], boxes[1]);
    g.eyes[0] = boxes[0];
    g.eyes[1] = boxes[1];
    g.eye_valid = true;
    g.eye_t = now;
    if (!g.jpeg.empty()) g.eye_frame = g.jpeg;  // freeze a matching frame now
}

// --- game state machine ------------------------------------------------------

void ResetToIdleLocked() {
    g.phase = Phase::kIdle;
    g.token.clear();
    g.player.clear();
    g.jpeg.clear();
    g.final_score = -1;
    g.final_rank = -1;
    g.final_best = -1;
    g.final_avatar.clear();
    g.end_reason.clear();
    g.eye_valid = false;
    g.eye_frame.clear();
    g.reacts.clear();
}

bool SameName(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (tolower(static_cast<unsigned char>(a[i])) !=
            tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

void EndRoundLocked(double now, const char* reason) {
    g.end_reason = reason;
    g.final_score = std::max(0.0, now - g.stare_start);

    // eyes left the camera: disqualified — no leaderboard entry, no profile
    if (g.end_reason == "lost") {
        g.final_rank = -1;
        g.final_best = -1;
        g.final_avatar.clear();
        g.phase = Phase::kDone;
        g.phase_start = now;
        return;
    }

    // Use the eyes captured during the round (any moment the SDK saw a face),
    // not just the final instant — phone detection can drop for a beat right
    // before the blink, and we don't want to lose the avatar over that.
    std::string avatar_id;
    fprintf(stderr, "[avatar] round end: want_eyes=%d eye_valid=%d eye_age=%.2fs frame_bytes=%zu\n",
            g.want_eyes, g.eye_valid, now - g.eye_t, g.eye_frame.size());
    if (g.want_eyes && g.eye_valid) {
        const std::string png = CropEyesLocked();
        if (!png.empty()) {
            avatar_id = RandomToken().substr(0, 16);
            g.avatars[avatar_id] = png;
            fprintf(stderr, "[avatar] created (%zu bytes)\n", png.size());
        }
    }
    g.final_avatar = avatar_id;

    // one profile per name: keep the best score, always refresh eyes and date
    BoardEntry* mine = nullptr;
    for (auto& e : g.board) {
        if (SameName(e.name, g.player)) { mine = &e; break; }
    }
    if (mine) {
        g.final_best = std::max(mine->score, g.final_score);
        if (g.final_score > mine->score) mine->score = g.final_score;
        if (!avatar_id.empty()) {
            g.avatars.erase(mine->avatar);
            mine->avatar = avatar_id;
        } else if (!g.want_eyes && !mine->avatar.empty()) {
            // player revoked consent this round — take their eyes off the board
            g.avatars.erase(mine->avatar);
            mine->avatar.clear();
        }
        mine->when = TodayIso();
    } else {
        g.final_best = g.final_score;
        g.board.push_back({g.player, g.final_score, TodayIso(), avatar_id});
    }
    std::sort(g.board.begin(), g.board.end(),
              [](const BoardEntry& a, const BoardEntry& b) { return a.score > b.score; });
    while (g.board.size() > 100) {
        g.avatars.erase(g.board.back().avatar);
        g.board.pop_back();
    }
    g.final_rank = 1;
    for (size_t i = 0; i < g.board.size(); ++i) {
        if (SameName(g.board[i].name, g.player)) {
            g.final_rank = static_cast<int>(i) + 1;
            break;
        }
    }
    SaveBoardLocked();

    g.phase = Phase::kDone;
    g.phase_start = now;
}

void TickLocked(double now) {
    switch (g.phase) {
        case Phase::kIdle:
            break;
        case Phase::kCountdown:
            // staring only begins once the SDK actually sees a face
            if (now - g.phase_start >= kCountdownSeconds &&
                now - g.face_t < kFaceFreshSeconds) {
                g.phase = Phase::kStaring;
                g.phase_start = now;
                g.stare_start = now;
            }
            if (now - g.last_frame_t > kSessionTimeoutSeconds ||
                now - g.phase_start > kCountdownMaxSeconds) {
                ResetToIdleLocked();
            }
            break;
        case Phase::kStaring:
            // eyes must stay in frame: face gone too long ends the round at
            // the moment it was last seen
            if (now - g.face_t > kFaceLostSeconds) {
                EndRoundLocked(std::max(g.face_t, g.stare_start), "lost");
                break;
            }
            if (now - g.last_frame_t > kSessionTimeoutSeconds) ResetToIdleLocked();
            break;
        case Phase::kDone:
            if (now - g.phase_start > kDoneLingerSeconds) ResetToIdleLocked();
            break;
    }
}

void RegisterBlinkEdgeLocked(double now) {
    g.last_blink_t = now;
    if (g.phase == Phase::kStaring && now - g.stare_start > kBlinkGraceSeconds) {
        EndRoundLocked(now, "blink");
    }
}

// --- SDK callbacks -----------------------------------------------------------

void OnMetrics(const spectra::Metrics& m, int64_t /*timestamp_us*/) {
    const double now = NowSeconds();
    std::lock_guard<std::mutex> lock(g.mu);
    TickLocked(now);

    // face presence: landmarks and blink samples are frame-driven, so their
    // arrival means the SDK currently sees a face
    if (m.has_face() && (m.face().landmarks_size() > 0 || m.face().blinking_size() > 0)) {
        g.face_t = now;
    }

    // Are the eyes open right now? Default to the last known state, override
    // with this callback's newest blink sample. Used to avoid capturing the
    // avatar mid-blink.
    bool open = !g.prev_blink_detected;
    if (m.has_face() && m.face().blinking_size() > 0) {
        open = !m.face().blinking(m.face().blinking_size() - 1).detected();
    }

    // eye region for the avatar crop (only captured while eyes are open)
    if (g.phase == Phase::kCountdown || g.phase == Phase::kStaring) {
        UpdateEyeBoxFromLandmarks(m, now, open);
    }

    // blinks: rising edges of the per-frame detection flag end the round
    if (m.has_face() && m.face().blinking_size() > 0) {
        for (int i = 0; i < m.face().blinking_size(); ++i) {
            const bool detected = m.face().blinking(i).detected();
            if (detected && !g.prev_blink_detected) RegisterBlinkEdgeLocked(now);
            g.prev_blink_detected = detected;
        }
    }
}

// --- JSON payloads -----------------------------------------------------------

std::string BoardJsonLocked(int top_n) {
    std::string json = "[";
    const int n = std::min<int>(top_n, static_cast<int>(g.board.size()));
    for (int i = 0; i < n; ++i) {
        const auto& e = g.board[i];
        char buf[320];
        snprintf(buf, sizeof(buf),
                 "%s{\"name\":\"%s\",\"score\":%.2f,\"when\":\"%s\",\"avatar\":\"%s\"}",
                 i ? "," : "", JsonEscape(e.name).c_str(), e.score, e.when.c_str(),
                 e.avatar.c_str());
        json += buf;
    }
    return json + "]";
}

std::string GameJsonLocked(double now) {
    char buf[768];
    const double countdown_left =
        g.phase == Phase::kCountdown
            ? std::max(0.0, kCountdownSeconds - (now - g.phase_start))
            : 0;
    const double stare_t = g.phase == Phase::kStaring ? now - g.stare_start
                           : g.phase == Phase::kDone  ? g.final_score
                                                      : 0;
    snprintf(buf, sizeof(buf),
             "\"phase\":\"%s\",\"player\":\"%s\",\"countdown\":%.1f,\"stare_t\":%.2f,"
             "\"score\":%.2f,\"rank\":%d,\"best\":%.2f,\"players\":%zu,\"avatar\":\"%s\","
             "\"reason\":\"%s\",\"face\":%s,"
             "\"blink_ago\":%.2f,\"hint\":\"%s\"",
             PhaseName(g.phase), JsonEscape(g.player).c_str(), countdown_left, stare_t,
             g.final_score, g.final_rank, g.final_best, g.board.size(), g.final_avatar.c_str(),
             g.end_reason.c_str(), now - g.face_t < kFaceFreshSeconds ? "true" : "false",
             g.last_blink_t < 0 ? -1.0 : now - g.last_blink_t, JsonEscape(g.hint).c_str());
    return buf;
}

std::string BuildEventJson() {
    const double now = NowSeconds();
    std::lock_guard<std::mutex> lock(g.mu);
    TickLocked(now);

    // recent reactions (last ~3.5s); prune anything older
    while (!g.reacts.empty() && g.reacts.front().t < now - 4.0) g.reacts.pop_front();
    std::string reacts = "[";
    bool first = true;
    for (const auto& r : g.reacts) {
        if (r.t < now - 3.5) continue;
        char buf[48];
        snprintf(buf, sizeof(buf), "%s[%llu,%d]", first ? "" : ",",
                 static_cast<unsigned long long>(r.id), r.idx);
        reacts += buf;
        first = false;
    }
    reacts += "]";

    return "{" + GameJsonLocked(now) + ",\"board\":" + BoardJsonLocked(10) +
           ",\"reacts\":" + reacts + "}";
}

}  // namespace

int main(int argc, char** argv) {
    absl::ParseCommandLine(argc, argv);

    const bool fake = absl::GetFlag(FLAGS_fake);
    std::string api_key = absl::GetFlag(FLAGS_api_key);
    if (api_key.empty()) {
        if (const char* env = std::getenv("SMARTSPECTRA_API_KEY")) api_key = env;
    }
    if (api_key.empty() && !fake) {
        fprintf(stderr, "No API key. Pass --api_key=... or set SMARTSPECTRA_API_KEY.\n");
        return EXIT_FAILURE;
    }

    LoadBoard();

    std::unique_ptr<spectra::SmartSpectra> smart_spectra;
    std::shared_ptr<spectra::CustomInput> input;
    std::thread fake_pulse;
    if (fake) {
        fprintf(stderr, "FAKE MODE: no SDK; POST /api/debug_blink ends rounds.\n");
        fake_pulse = std::thread([] {
            while (true) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                const double now = NowSeconds();
                std::lock_guard<std::mutex> lock(g.mu);
                TickLocked(now);
                if (g.phase == Phase::kCountdown || g.phase == Phase::kStaring) {
                    // pretend two eyes sit either side of the frame center
                    g.eyes[0] = {0.40f, 0.43f, 0.10f, 0.06f};
                    g.eyes[1] = {0.60f, 0.43f, 0.10f, 0.06f};
                    g.eye_valid = true;
                    g.eye_t = now;
                    if (!g.jpeg.empty()) g.eye_frame = g.jpeg;
                    // a face is "seen" as long as frames keep arriving
                    g.face_t = g.last_frame_t;
                }
            }
        });
        fake_pulse.detach();
    } else {
        spectra::SmartSpectraConfig config;
        config.api_key = api_key;
        config.requested_metrics = spectra::SmartSpectraConfig::BreathingMetrics();
        config.AddMetrics({
            MetricType::BLINKING,
            MetricType::FACE_LANDMARKS,
        });

        smart_spectra = std::make_unique<spectra::SmartSpectra>(std::move(config));
        g_smart_spectra = smart_spectra.get();

        smart_spectra->SetOnMetrics(OnMetrics);
        smart_spectra->SetOnValidationStatusChanged(
            [](const spectra::ValidationStatus& vs, int64_t) {
                std::lock_guard<std::mutex> lock(g.mu);
                g.hint = vs.hint;
            });
        smart_spectra->SetOnError([](const spectra::SmartSpectraError& error) {
            fprintf(stderr, "[smartspectra] %s\n", error.FullMessage().c_str());
        });

        if (const auto err = smart_spectra->UseCustomInput().Build(input); !err.ok()) {
            fprintf(stderr, "UseCustomInput failed: %s\n", err.message.c_str());
            return EXIT_FAILURE;
        }
        if (const auto err = smart_spectra->Start(); !err.ok()) {
            fprintf(stderr, "Start failed: %s\n", err.FullMessage().c_str());
            return EXIT_FAILURE;
        }
    }

    httplib::Server server;
    g_server = &server;
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    server.set_mount_point("/", absl::GetFlag(FLAGS_web_root));

    server.Post("/api/join", [](const httplib::Request& req, httplib::Response& res) {
        std::string name = "anonymous";
        const size_t k = req.body.find("\"name\":\"");
        if (k != std::string::npos) {
            const size_t end = req.body.find('"', k + 8);
            if (end != std::string::npos) name = req.body.substr(k + 8, end - k - 8);
        }
        if (name.size() > 16) name.resize(16);
        name.erase(std::remove_if(name.begin(), name.end(),
                                  [](char c) { return c == '{' || c == '}'; }),
                   name.end());
        if (name.empty()) name = "anonymous";
        const bool want_eyes = req.body.find("\"eyes\":false") == std::string::npos;

        const double now = NowSeconds();
        std::lock_guard<std::mutex> lock(g.mu);
        TickLocked(now);
        // Only an active round (countdown/staring) blocks a new join. A finished
        // round (kDone) is just lingering on the result screen — let the next
        // player (or "go again") start immediately instead of waiting it out.
        if (g.phase == Phase::kCountdown || g.phase == Phase::kStaring) {
            res.status = 409;
            res.set_content("{\"error\":\"busy\",\"player\":\"" + JsonEscape(g.player) + "\"}",
                            "application/json");
            return;
        }
        ResetToIdleLocked();
        g.phase = Phase::kCountdown;
        g.phase_start = now;
        g.last_frame_t = now;
        g.token = RandomToken();
        g.player = name;
        g.want_eyes = want_eyes;
        g.prev_blink_detected = false;
        res.set_content("{\"token\":\"" + g.token + "\",\"countdown\":" +
                            std::to_string(static_cast<int>(kCountdownSeconds)) + "}",
                        "application/json");
    });

    server.Post("/api/frame", [&input](const httplib::Request& req, httplib::Response& res) {
        const std::string token = req.get_param_value("token");
        const int64_t client_us = strtoll(req.get_header_value("X-TS").c_str(), nullptr, 10);
        const double now = NowSeconds();

        {
            std::lock_guard<std::mutex> lock(g.mu);
            TickLocked(now);
            if (g.phase == Phase::kIdle || token != g.token) {
                res.status = 410;
                res.set_content("{\"error\":\"no session\"}", "application/json");
                return;
            }
            g.last_frame_t = now;
            g.jpeg = req.body;          // newest frame wins for the spectator preview
            ++g.jpeg_seq;
        }

        // Decode outside any lock so frames can decode in parallel.
        int w = 0, h = 0, channels = 0;
        unsigned char* rgb = stbi_load_from_memory(
            reinterpret_cast<const unsigned char*>(req.body.data()),
            static_cast<int>(req.body.size()), &w, &h, &channels, 3);
        if (!rgb) {
            res.status = 400;
            res.set_content("{\"error\":\"bad jpeg\"}", "application/json");
            return;
        }

        // Every frame fed to the SDK must be exactly kFrameDim square. The
        // optical-flow tracker runs continuously across the whole stream and
        // never resets between players, so a differently-sized frame (a stale
        // client, or just the next player's phone) would crash it. Drop anything
        // that isn't the expected size — globally, not per-session.
        if (w != kFrameDim || h != kFrameDim) {
            static int logged = 0;
            if (logged++ < 5) fprintf(stderr, "[frame] dropped wrong size %dx%d (need %dx%d)\n",
                                      w, h, kFrameDim, kFrameDim);
            stbi_image_free(rgb);
            res.set_content("{\"ok\":1}", "application/json");
            return;
        }

        // Feed the SDK under g_feed_mu so concurrent frames are serialized and
        // stale (out-of-order) frames are dropped — the graph needs strictly
        // increasing timestamps and rejects gaps > 2 s.
        if (input) {
            std::lock_guard<std::mutex> feed(g_feed_mu);
            if (token != g.feed_token) {            // (re)calibrate the clock for a new session
                g.feed_token = token;
                g.client_first_us = client_us;
                g.last_client_us = client_us - 1;
                g.virtual_base_us = g.last_sent_us + 33'000;
            }
            if (client_us > g.last_client_us) {     // in order — feed it
                g.last_client_us = client_us;
                int64_t ts = g.virtual_base_us + (client_us - g.client_first_us);
                if (ts - g.last_sent_us > 1'500'000) {  // stall: compress the gap
                    g.virtual_base_us -= ts - g.last_sent_us - 33'000;
                    ts = g.last_sent_us + 33'000;
                }
                if (ts <= g.last_sent_us) ts = g.last_sent_us + 1000;
                g.last_sent_us = ts;
                spectra::FrameBuffer frame;
                frame.data = rgb;
                frame.width = w;
                frame.height = h;
                frame.stride_bytes = w * 3;
                frame.format = spectra::PixelFormat::kRGB;
                if (const auto err = input->Send(frame, ts); !err.ok()) {
                    static int logged = 0;
                    if (logged++ < 10) fprintf(stderr, "Send failed: %s\n", err.message.c_str());
                }
            }
            // else: late/out-of-order or wrong size — drop it
        }
        stbi_image_free(rgb);

        res.set_content("{\"ok\":1}", "application/json");  // UI state comes via SSE
    });

    if (fake) {
        server.Post("/api/debug_blink", [](const httplib::Request&, httplib::Response& res) {
            const double now = NowSeconds();
            std::lock_guard<std::mutex> lock(g.mu);
            TickLocked(now);
            RegisterBlinkEdgeLocked(now);
            res.set_content("{\"ok\":true}", "application/json");
        });
    }

    // Spectators fling an emoji (by index) at the active player to distract them.
    server.Post("/api/react", [](const httplib::Request& req, httplib::Response& res) {
        const int idx = atoi(req.get_param_value("e").c_str());
        const double now = NowSeconds();
        std::lock_guard<std::mutex> lock(g.mu);
        TickLocked(now);
        if ((g.phase == Phase::kCountdown || g.phase == Phase::kStaring) &&
            idx >= 0 && idx < kEmojiCount) {
            g.reacts.push_back({++g.react_seq, idx, now});
            while (g.reacts.size() > 60) g.reacts.pop_front();  // safety cap
        }
        res.set_content("{\"ok\":1}", "application/json");
    });

    server.Post("/api/leave", [](const httplib::Request& req, httplib::Response& res) {
        const std::string token = req.get_param_value("token");
        std::lock_guard<std::mutex> lock(g.mu);
        if (!g.token.empty() && token == g.token && g.phase != Phase::kDone) {
            ResetToIdleLocked();
        }
        res.set_content("{\"ok\":true}", "application/json");
    });

    server.Get("/avatar", [](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.get_param_value("id");
        std::lock_guard<std::mutex> lock(g.mu);
        const auto it = g.avatars.find(id);
        if (it == g.avatars.end()) {
            res.status = 404;
            return;
        }
        res.set_header("Cache-Control", "public, max-age=31536000, immutable");
        const bool is_png = it->second.size() > 4 && it->second.compare(1, 3, "PNG") == 0;
        res.set_content(it->second, is_png ? "image/png" : "image/jpeg");
    });

    // NOTE: there is intentionally no live camera broadcast. A player's frames
    // are used only for on-device detection and the consented eye-crop avatar —
    // they are never re-served to spectators or anyone else.

    server.Get("/events", [](const httplib::Request&, httplib::Response& res) {
        res.set_chunked_content_provider(
            "text/event-stream",
            [](size_t, httplib::DataSink& sink) {
                const std::string payload = "data: " + BuildEventJson() + "\n\n";
                if (!sink.write(payload.data(), payload.size())) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                return true;
            });
    });

    const int port = absl::GetFlag(FLAGS_port);
    printf("EYE FIGHT arena open — http://localhost:%d (Ctrl+C to stop)\n", port);
    printf("Phones need HTTPS for camera access; see README for the tunnel one-liner.\n");
    server.listen(absl::GetFlag(FLAGS_host), port);

    if (smart_spectra) (void)smart_spectra->Stop();
    printf("Done.\n");
    return 0;
}
