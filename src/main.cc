// doomstare: a staring-contest game where your vitals snitch on you.
// Built on the Presage SmartSpectra C++ SDK.
//
// Any browser (phone/laptop) captures its own camera via getUserMedia and
// streams JPEG frames to this server, which pushes them into SmartSpectra via
// CustomInput. The SDK's per-frame blink detection ends the round; pulse rate
// is shown live. Scores persist to a JSON leaderboard.
//
//   GET  /            — game UI (web/index.html)
//   GET  /stream      — MJPEG re-broadcast of the current player (spectators)
//   GET  /events      — SSE: game state, vitals, leaderboard (10 Hz)
//   POST /api/join    — {"name":"..."} -> {"token":"..."} or 409 when busy
//   POST /api/frame   — JPEG body + X-TS header (client µs); acks game state
//   POST /api/leave   — ?token=... forfeits / ends the session
//
// Usage:
//   ./doomstare --api_key=YOUR_KEY [--port=8428] [--web_root=web]

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

constexpr double kCountdownSeconds = 5.0;
constexpr double kBlinkGraceSeconds = 0.5;   // ignore blink edges right at round start
constexpr double kSessionTimeoutSeconds = 6; // no frames -> player vanished
constexpr double kDoneLingerSeconds = 12;    // result shown before lobby reopens

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

struct TracePoint { double t, v; };

struct BoardEntry {
    std::string name;
    double score = 0;     // best staring time, seconds
    double bpm = 0;       // average pulse during that stare (0 = no signal)
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

    // vitals from the SDK
    double bpm = 0;
    bool bpm_stable = false;
    double br = 0;
    std::string hint;
    std::deque<TracePoint> trace;
    bool have_cardio_trace = false;
    std::string trace_kind = "breathing";
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
    double bpm_sum = 0;
    int bpm_n = 0;
    double final_score = -1;
    double final_bpm = 0;
    int final_rank = -1;
    double final_best = -1;
    std::string final_avatar;

    // eye region from the latest face landmarks (raw landmark coords; whether
    // they are normalized or pixel-space is decided at crop time)
    bool eye_valid = false;
    double eye_t = 0;
    float eye_min_x = 0, eye_max_x = 0, eye_min_y = 0, eye_max_y = 0;
    int frame_w = 0, frame_h = 0;  // dims of the last decoded frame

    // avatar id -> jpeg bytes (eye strips)
    std::map<std::string, std::string> avatars;

    // virtual-clock mapping for CustomInput (compresses idle gaps; the SDK
    // rejects forward timestamp gaps > 2 s and non-monotonic values)
    bool ts_init = false;
    int64_t client_first_us = 0;
    int64_t virtual_base_us = 0;
    int64_t last_sent_us = 0;

    std::vector<BoardEntry> board;
};

GameState g;
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
          << ",\"bpm\":" << e.bpm << ",\"when\":\"" << e.when << "\"";
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
        const size_t b = field("\"bpm\":");
        const size_t w = field("\"when\":\"");
        const size_t a = field("\"avatar_b64\":\"");
        if (s == std::string::npos) break;
        e.score = atof(text.c_str() + s + 8);
        if (b != std::string::npos) e.bpm = atof(text.c_str() + b + 6);
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

// --- eye-strip avatars -------------------------------------------------------

// Crop a both-eyes strip from the player's latest frame using the face
// landmarks captured during the round. Returns JPEG bytes, or "" on failure.
std::string CropEyeStripLocked() {
    if (!g.eye_valid || g.jpeg.empty()) return "";

    int w = 0, h = 0, channels = 0;
    unsigned char* rgb = stbi_load_from_memory(
        reinterpret_cast<const unsigned char*>(g.jpeg.data()),
        static_cast<int>(g.jpeg.size()), &w, &h, &channels, 3);
    if (!rgb) return "";

    // Landmarks may be normalized [0,1] or pixel-space depending on pipeline.
    double min_x = g.eye_min_x, max_x = g.eye_max_x;
    double min_y = g.eye_min_y, max_y = g.eye_max_y;
    if (max_x <= 2.0 && max_y <= 2.0) {
        min_x *= w; max_x *= w;
        min_y *= h; max_y *= h;
    }
    // pad: a little sideways, generously up/down (brows and bags are character)
    const double pad_x = (max_x - min_x) * 0.15;
    const double pad_y = std::max((max_y - min_y) * 0.9, (max_x - min_x) * 0.12);
    int x0 = std::max(0, static_cast<int>(min_x - pad_x));
    int x1 = std::min(w, static_cast<int>(max_x + pad_x));
    int y0 = std::max(0, static_cast<int>(min_y - pad_y));
    int y1 = std::min(h, static_cast<int>(max_y + pad_y));
    const int cw = x1 - x0, ch = y1 - y0;
    if (cw < 24 || ch < 8) {  // degenerate box — refuse to make a 3-pixel soul
        stbi_image_free(rgb);
        return "";
    }

    std::vector<uint8_t> crop(static_cast<size_t>(cw) * ch * 3);
    for (int y = 0; y < ch; ++y) {
        memcpy(crop.data() + static_cast<size_t>(y) * cw * 3,
               rgb + (static_cast<size_t>(y0 + y) * w + x0) * 3,
               static_cast<size_t>(cw) * 3);
    }
    stbi_image_free(rgb);

    std::string jpeg;
    auto sink = [](void* ctx, void* data, int size) {
        static_cast<std::string*>(ctx)->append(static_cast<char*>(data), size);
    };
    if (!stbi_write_jpg_to_func(sink, &jpeg, cw, ch, 3, crop.data(), 80)) return "";
    return jpeg;
}

// MediaPipe-facemesh indices for eye corners/lids and brows. Used when the
// SDK delivers a dense (>=468 point) mesh; otherwise we fall back to a strip
// across the upper-middle of the whole-landmark bounding box.
constexpr int kEyeIdx[] = {33, 133, 159, 145, 263, 362, 386, 374,
                           70, 63, 105, 66, 107, 300, 293, 334, 296, 336};

void UpdateEyeBoxFromLandmarks(const spectra::Metrics& m, double now) {
    if (!m.has_face() || m.face().landmarks_size() == 0) return;
    const auto& lm = m.face().landmarks(m.face().landmarks_size() - 1);
    const int n = lm.value_size();
    if (n == 0) return;

    float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
    if (n >= 468) {
        for (int idx : kEyeIdx) {
            const auto& p = lm.value(idx);
            min_x = std::min(min_x, p.x()); max_x = std::max(max_x, p.x());
            min_y = std::min(min_y, p.y()); max_y = std::max(max_y, p.y());
        }
    } else {
        for (int i = 0; i < n; ++i) {
            const auto& p = lm.value(i);
            min_x = std::min(min_x, p.x()); max_x = std::max(max_x, p.x());
            min_y = std::min(min_y, p.y()); max_y = std::max(max_y, p.y());
        }
        // eyes live roughly 30-50% down the face box, middle ~76% across
        const float fw = max_x - min_x, fh = max_y - min_y;
        min_x += fw * 0.12f; max_x -= fw * 0.12f;
        const float top = min_y + fh * 0.30f, bot = min_y + fh * 0.50f;
        min_y = top; max_y = bot;
    }
    g.eye_min_x = min_x; g.eye_max_x = max_x;
    g.eye_min_y = min_y; g.eye_max_y = max_y;
    g.eye_valid = true;
    g.eye_t = now;
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
    g.eye_valid = false;
}

bool SameName(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (tolower(static_cast<unsigned char>(a[i])) !=
            tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

void EndRoundLocked(double now) {
    g.final_score = now - g.stare_start;
    g.final_bpm = g.bpm_n > 0 ? g.bpm_sum / g.bpm_n : 0;

    // freeze the player's eyes at the moment of the fatal blink
    std::string avatar_id;
    if (g.want_eyes && now - g.eye_t < 2.0) {
        const std::string jpeg = CropEyeStripLocked();
        if (!jpeg.empty()) {
            avatar_id = RandomToken().substr(0, 16);
            g.avatars[avatar_id] = jpeg;
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
        if (g.final_score > mine->score) {
            mine->score = g.final_score;
            mine->bpm = g.final_bpm;
        }
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
        g.board.push_back({g.player, g.final_score, g.final_bpm, TodayIso(), avatar_id});
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
            if (now - g.phase_start >= kCountdownSeconds) {
                g.phase = Phase::kStaring;
                g.phase_start = now;
                g.stare_start = now;
                g.bpm_sum = 0;
                g.bpm_n = 0;
            }
            if (now - g.last_frame_t > kSessionTimeoutSeconds) ResetToIdleLocked();
            break;
        case Phase::kStaring:
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
        EndRoundLocked(now);
    }
}

// --- SDK callbacks -----------------------------------------------------------

void OnMetrics(const spectra::Metrics& m, int64_t /*timestamp_us*/) {
    const double now = NowSeconds();
    std::lock_guard<std::mutex> lock(g.mu);
    TickLocked(now);

    if (m.has_cardio() && m.cardio().pulse_rate_size() > 0) {
        const auto& pulse = m.cardio().pulse_rate(m.cardio().pulse_rate_size() - 1);
        if (pulse.value() > 0) {
            g.bpm = pulse.value();
            g.bpm_stable = pulse.stable();
            if (g.phase == Phase::kStaring) {
                g.bpm_sum += g.bpm;
                ++g.bpm_n;
            }
        }
    }
    if (m.has_breathing() && m.breathing().rate_size() > 0) {
        const auto& rate = m.breathing().rate(m.breathing().rate_size() - 1);
        if (rate.value() > 0) g.br = rate.value();
    }

    // waveform: prefer the cardiac pressure trace once it produces samples
    if (m.has_cardio() && m.cardio().arterial_pressure_trace_size() > 0) {
        if (!g.have_cardio_trace) {
            g.have_cardio_trace = true;
            g.trace_kind = "pulse";
            g.trace.clear();
        }
        for (int i = 0; i < m.cardio().arterial_pressure_trace_size(); ++i) {
            const auto& s = m.cardio().arterial_pressure_trace(i);
            g.trace.push_back({s.timestamp() / 1e6, s.value()});
        }
    } else if (!g.have_cardio_trace && m.has_breathing() && m.breathing().upper_trace_size() > 0) {
        for (int i = 0; i < m.breathing().upper_trace_size(); ++i) {
            const auto& s = m.breathing().upper_trace(i);
            g.trace.push_back({s.timestamp() / 1e6, s.value()});
        }
    }
    while (g.trace.size() > 1500) g.trace.pop_front();

    // eye region for the avatar crop (only worth tracking mid-session)
    if (g.phase == Phase::kCountdown || g.phase == Phase::kStaring) {
        UpdateEyeBoxFromLandmarks(m, now);
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
                 "%s{\"name\":\"%s\",\"score\":%.2f,\"bpm\":%.0f,\"when\":\"%s\",\"avatar\":\"%s\"}",
                 i ? "," : "", JsonEscape(e.name).c_str(), e.score, e.bpm, e.when.c_str(),
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
             "\"score\":%.2f,\"rank\":%d,\"avg_bpm\":%.0f,\"best\":%.2f,\"avatar\":\"%s\","
             "\"bpm\":%.1f,\"bpm_stable\":%s,\"br\":%.1f,"
             "\"blink_ago\":%.2f,\"hint\":\"%s\"",
             PhaseName(g.phase), JsonEscape(g.player).c_str(), countdown_left, stare_t,
             g.final_score, g.final_rank, g.final_bpm, g.final_best, g.final_avatar.c_str(),
             g.bpm, g.bpm_stable ? "true" : "false", g.br,
             g.last_blink_t < 0 ? -1.0 : now - g.last_blink_t, JsonEscape(g.hint).c_str());
    return buf;
}

std::string BuildEventJson(double& last_trace_t) {
    const double now = NowSeconds();
    std::lock_guard<std::mutex> lock(g.mu);
    TickLocked(now);

    std::string json = "{" + GameJsonLocked(now) + ",\"board\":" + BoardJsonLocked(10) + ",\"trace\":[";
    bool first = true;
    for (const auto& p : g.trace) {
        if (p.t <= last_trace_t) continue;
        char buf[64];
        snprintf(buf, sizeof(buf), "%s[%.3f,%.4f]", first ? "" : ",", p.t, p.v);
        json += buf;
        first = false;
        last_trace_t = p.t;
    }
    json += "],\"trace_kind\":\"" + g.trace_kind + "\"}";
    return json;
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
                g.bpm = 70 + 8 * std::sin(now / 5);
                g.bpm_stable = true;
                if (g.phase == Phase::kStaring) { g.bpm_sum += g.bpm; ++g.bpm_n; }
                if (g.phase == Phase::kCountdown || g.phase == Phase::kStaring) {
                    // pretend the eyes sit in a centered strip of the frame
                    g.eye_min_x = 0.30f; g.eye_max_x = 0.70f;
                    g.eye_min_y = 0.38f; g.eye_max_y = 0.48f;
                    g.eye_valid = true;
                    g.eye_t = now;
                }
                g.trace.push_back({now, std::sin(now * 7) + 0.3 * std::sin(now * 23)});
                while (g.trace.size() > 1500) g.trace.pop_front();
            }
        });
        fake_pulse.detach();
    } else {
        spectra::SmartSpectraConfig config;
        config.api_key = api_key;
        config.requested_metrics = spectra::SmartSpectraConfig::BreathingMetrics();
        config.AddMetrics({
            MetricType::PULSE_RATE,
            MetricType::ARTERIAL_PRESSURE_TRACE,
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
        if (g.phase != Phase::kIdle) {
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
        g.ts_init = false;
        g.prev_blink_detected = false;
        res.set_content("{\"token\":\"" + g.token + "\",\"countdown\":5}", "application/json");
    });

    server.Post("/api/frame", [&input](const httplib::Request& req, httplib::Response& res) {
        const std::string token = req.get_param_value("token");
        const int64_t client_us = strtoll(req.get_header_value("X-TS").c_str(), nullptr, 10);
        const double now = NowSeconds();

        int64_t send_us = -1;
        {
            std::lock_guard<std::mutex> lock(g.mu);
            TickLocked(now);
            if (g.phase == Phase::kIdle || token != g.token) {
                res.status = 410;
                res.set_content("{\"error\":\"no session\"}", "application/json");
                return;
            }
            g.last_frame_t = now;
            g.jpeg = req.body;
            ++g.jpeg_seq;

            // map the client's monotonic clock onto our virtual SDK clock
            if (!g.ts_init) {
                g.ts_init = true;
                g.client_first_us = client_us;
                g.virtual_base_us = g.last_sent_us + 33'000;
            }
            int64_t ts = g.virtual_base_us + (client_us - g.client_first_us);
            if (ts - g.last_sent_us > 1'500'000) {  // stall: compress the gap
                g.virtual_base_us -= ts - g.last_sent_us - 33'000;
                ts = g.last_sent_us + 33'000;
            }
            if (ts <= g.last_sent_us) ts = g.last_sent_us + 1000;
            g.last_sent_us = ts;
            send_us = ts;
        }

        int w = 0, h = 0, channels = 0;
        unsigned char* rgb = stbi_load_from_memory(
            reinterpret_cast<const unsigned char*>(req.body.data()),
            static_cast<int>(req.body.size()), &w, &h, &channels, 3);
        if (!rgb) {
            res.status = 400;
            res.set_content("{\"error\":\"bad jpeg\"}", "application/json");
            return;
        }
        if (input) {
            spectra::FrameBuffer frame;
            frame.data = rgb;
            frame.width = w;
            frame.height = h;
            frame.stride_bytes = w * 3;
            frame.format = spectra::PixelFormat::kRGB;
            if (const auto err = input->Send(frame, send_us); !err.ok()) {
                static int logged = 0;
                if (logged++ < 10) fprintf(stderr, "Send failed: %s\n", err.message.c_str());
            }
        }
        stbi_image_free(rgb);

        std::lock_guard<std::mutex> lock(g.mu);
        res.set_content("{" + GameJsonLocked(NowSeconds()) + "}", "application/json");
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
        res.set_content(it->second, "image/jpeg");
    });

    server.Get("/stream", [](const httplib::Request&, httplib::Response& res) {
        res.set_chunked_content_provider(
            "multipart/x-mixed-replace; boundary=frame",
            [last_seq = uint64_t{0}](size_t, httplib::DataSink& sink) mutable {
                std::string jpeg;
                {
                    std::lock_guard<std::mutex> lock(g.mu);
                    if (g.jpeg_seq != last_seq && !g.jpeg.empty()) {
                        last_seq = g.jpeg_seq;
                        jpeg = g.jpeg;
                    }
                }
                if (!jpeg.empty()) {
                    char header[128];
                    snprintf(header, sizeof(header),
                             "--frame\r\nContent-Type: image/jpeg\r\n"
                             "Content-Length: %zu\r\n\r\n",
                             jpeg.size());
                    if (!sink.write(header, strlen(header))) return false;
                    if (!sink.write(jpeg.data(), jpeg.size())) return false;
                    if (!sink.write("\r\n", 2)) return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(66));
                return true;
            });
    });

    server.Get("/events", [](const httplib::Request&, httplib::Response& res) {
        res.set_chunked_content_provider(
            "text/event-stream",
            [last_trace_t = 0.0](size_t, httplib::DataSink& sink) mutable {
                const std::string payload = "data: " + BuildEventJson(last_trace_t) + "\n\n";
                if (!sink.write(payload.data(), payload.size())) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                return true;
            });
    });

    const int port = absl::GetFlag(FLAGS_port);
    printf("DOOMSTARE arena open — http://localhost:%d (Ctrl+C to stop)\n", port);
    printf("Phones need HTTPS for camera access; see README for the tunnel one-liner.\n");
    server.listen(absl::GetFlag(FLAGS_host), port);

    if (smart_spectra) (void)smart_spectra->Stop();
    printf("Done.\n");
    return 0;
}
