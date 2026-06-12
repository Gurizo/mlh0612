// doomstare: a camera vitals dashboard built on the Presage SmartSpectra SDK.
//
// Starts the SDK on the default camera, then serves a local web dashboard:
//   GET /        — dashboard UI (web/index.html)
//   GET /stream  — MJPEG camera preview
//   GET /events  — server-sent events with live metrics JSON
//
// Usage:
//   ./doomstare --api_key=YOUR_KEY [--port=8428] [--web_root=web]

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <absl/flags/flag.h>
#include <absl/flags/parse.h>

#include <smartspectra/smartspectra.h>
#include <smartspectra/smartspectra_config.h>
#include <smartspectra/smartspectra_types.h>

#include "httplib.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace spectra = presage::smartspectra;
using presage::smartspectra::MetricType;

ABSL_FLAG(std::string, api_key, "", "Presage API key (falls back to $SMARTSPECTRA_API_KEY).");
ABSL_FLAG(int, port, 8428, "Dashboard HTTP port.");
ABSL_FLAG(std::string, web_root, "web", "Directory with the dashboard static files.");

namespace {

double NowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct TracePoint {
    double t;  // seconds, steady clock
    double v;
};

struct AppState {
    std::mutex mu;

    // camera preview
    std::string jpeg;
    uint64_t jpeg_seq = 0;

    // vitals
    double bpm = 0;
    bool bpm_stable = false;
    double br = 0;
    std::string hint;

    // waveform (prefer cardiac pressure trace, fall back to breathing trace)
    std::deque<TracePoint> trace;
    bool have_cardio_trace = false;
    std::string trace_kind = "breathing";

    // blinking
    bool prev_blink_detected = false;
    uint64_t blink_total = 0;
    std::deque<double> blink_times;  // rising-edge timestamps, seconds
    double last_blink_t = -1;
    double start_t = 0;
};

AppState g_state;
spectra::SmartSpectra* g_smart_spectra = nullptr;
httplib::Server* g_server = nullptr;

void HandleSignal(int) {
    if (g_smart_spectra) (void)g_smart_spectra->Stop();
    if (g_server) g_server->stop();
}

// Repack a FrameBuffer into tightly-packed RGB for stb's JPEG writer.
bool FrameToRgb(const spectra::FrameBuffer& frame, std::vector<uint8_t>& rgb) {
    using PF = spectra::PixelFormat;
    const int w = frame.width, h = frame.height;
    int channels;
    int r_off, g_off, b_off;
    switch (frame.format) {
        case PF::kRGB:  channels = 3; r_off = 0; g_off = 1; b_off = 2; break;
        case PF::kBGR:  channels = 3; r_off = 2; g_off = 1; b_off = 0; break;
        case PF::kRGBA: channels = 4; r_off = 0; g_off = 1; b_off = 2; break;
        case PF::kBGRA: channels = 4; r_off = 2; g_off = 1; b_off = 0; break;
        default: return false;  // NV12/NV21 not expected from the macOS camera path
    }
    rgb.resize(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = frame.data + static_cast<size_t>(y) * frame.stride_bytes;
        uint8_t* dst = rgb.data() + static_cast<size_t>(y) * w * 3;
        for (int x = 0; x < w; ++x) {
            dst[x * 3 + 0] = src[x * channels + r_off];
            dst[x * 3 + 1] = src[x * channels + g_off];
            dst[x * 3 + 2] = src[x * channels + b_off];
        }
    }
    return true;
}

void OnVideoFrame(const spectra::FrameBuffer& frame, int64_t /*timestamp_us*/) {
    // Rate-limit JPEG encoding to ~12 fps; the dashboard preview doesn't need more.
    static double last_encode = 0;
    const double now = NowSeconds();
    if (now - last_encode < 0.08) return;
    last_encode = now;

    static std::vector<uint8_t> rgb;
    if (!FrameToRgb(frame, rgb)) return;

    std::string jpeg;
    jpeg.reserve(128 * 1024);
    auto sink = [](void* ctx, void* data, int size) {
        static_cast<std::string*>(ctx)->append(static_cast<char*>(data), size);
    };
    if (!stbi_write_jpg_to_func(sink, &jpeg, frame.width, frame.height, 3, rgb.data(), 70)) return;

    std::lock_guard<std::mutex> lock(g_state.mu);
    g_state.jpeg = std::move(jpeg);
    ++g_state.jpeg_seq;
}

void OnMetrics(const spectra::Metrics& m, int64_t /*timestamp_us*/) {
    const double now = NowSeconds();
    std::lock_guard<std::mutex> lock(g_state.mu);

    if (m.has_cardio() && m.cardio().pulse_rate_size() > 0) {
        const auto& pulse = m.cardio().pulse_rate(m.cardio().pulse_rate_size() - 1);
        if (pulse.value() > 0) {
            g_state.bpm = pulse.value();
            g_state.bpm_stable = pulse.stable();
        }
    }
    if (m.has_breathing() && m.breathing().rate_size() > 0) {
        const auto& rate = m.breathing().rate(m.breathing().rate_size() - 1);
        if (rate.value() > 0) g_state.br = rate.value();
    }

    // Waveform: prefer the cardiac pressure trace once it produces samples.
    if (m.has_cardio() && m.cardio().arterial_pressure_trace_size() > 0) {
        if (!g_state.have_cardio_trace) {
            g_state.have_cardio_trace = true;
            g_state.trace_kind = "pulse";
            g_state.trace.clear();
        }
        for (int i = 0; i < m.cardio().arterial_pressure_trace_size(); ++i) {
            const auto& s = m.cardio().arterial_pressure_trace(i);
            g_state.trace.push_back({s.timestamp() / 1e6, s.value()});
        }
    } else if (!g_state.have_cardio_trace && m.has_breathing() &&
               m.breathing().upper_trace_size() > 0) {
        for (int i = 0; i < m.breathing().upper_trace_size(); ++i) {
            const auto& s = m.breathing().upper_trace(i);
            g_state.trace.push_back({s.timestamp() / 1e6, s.value()});
        }
    }
    while (g_state.trace.size() > 1500) g_state.trace.pop_front();

    // Blinks: count rising edges of the per-frame detection flag.
    if (m.has_face() && m.face().blinking_size() > 0) {
        for (int i = 0; i < m.face().blinking_size(); ++i) {
            const bool detected = m.face().blinking(i).detected();
            if (detected && !g_state.prev_blink_detected) {
                ++g_state.blink_total;
                g_state.blink_times.push_back(now);
                g_state.last_blink_t = now;
            }
            g_state.prev_blink_detected = detected;
        }
        while (!g_state.blink_times.empty() && g_state.blink_times.front() < now - 90) {
            g_state.blink_times.pop_front();
        }
    }
}

std::string BuildEventJson(double& last_trace_t) {
    const double now = NowSeconds();
    std::lock_guard<std::mutex> lock(g_state.mu);

    const double elapsed = now - g_state.start_t;
    int recent = 0;
    for (double t : g_state.blink_times) {
        if (t >= now - 60) ++recent;
    }
    // Before a full minute has elapsed, extrapolate from the session so far.
    double blinks_per_min = -1;
    if (elapsed > 5) {
        blinks_per_min = elapsed >= 60 ? recent : recent * 60.0 / elapsed;
    }
    const double sec_since_blink = g_state.last_blink_t < 0 ? -1 : now - g_state.last_blink_t;
    const bool blink_now = g_state.last_blink_t > 0 && now - g_state.last_blink_t < 0.3;

    char head[512];
    snprintf(head, sizeof(head),
             "{\"bpm\":%.1f,\"bpm_stable\":%s,\"br\":%.1f,"
             "\"blink_total\":%llu,\"blinks_per_min\":%.2f,\"sec_since_blink\":%.2f,"
             "\"blink_now\":%s,\"calibrated\":%s,\"trace_kind\":\"%s\","
             "\"hint\":\"%s\",\"trace\":[",
             g_state.bpm, g_state.bpm_stable ? "true" : "false", g_state.br,
             static_cast<unsigned long long>(g_state.blink_total), blinks_per_min,
             sec_since_blink, blink_now ? "true" : "false",
             elapsed > 15 ? "true" : "false", g_state.trace_kind.c_str(),
             g_state.hint.c_str());

    std::string json = head;
    bool first = true;
    for (const auto& p : g_state.trace) {
        if (p.t <= last_trace_t) continue;
        char buf[64];
        snprintf(buf, sizeof(buf), "%s[%.3f,%.4f]", first ? "" : ",", p.t, p.v);
        json += buf;
        first = false;
        last_trace_t = p.t;
    }
    json += "]}";
    return json;
}

}  // namespace

int main(int argc, char** argv) {
    absl::ParseCommandLine(argc, argv);

    std::string api_key = absl::GetFlag(FLAGS_api_key);
    if (api_key.empty()) {
        if (const char* env = std::getenv("SMARTSPECTRA_API_KEY")) api_key = env;
    }
    if (api_key.empty()) {
        fprintf(stderr, "No API key. Pass --api_key=... or set SMARTSPECTRA_API_KEY.\n");
        return EXIT_FAILURE;
    }

    spectra::SmartSpectraConfig config;
    config.api_key = api_key;
    config.requested_metrics = spectra::SmartSpectraConfig::BreathingMetrics();
    config.AddMetrics({
        MetricType::PULSE_RATE,
        MetricType::ARTERIAL_PRESSURE_TRACE,
        MetricType::BLINKING,
    });

    spectra::SmartSpectra smart_spectra(std::move(config));
    g_smart_spectra = &smart_spectra;

    smart_spectra.SetOnMetrics(OnMetrics);
    smart_spectra.SetOnVideoOutput(OnVideoFrame);
    smart_spectra.SetOnValidationStatusChanged(
        [](const spectra::ValidationStatus& vs, int64_t) {
            std::lock_guard<std::mutex> lock(g_state.mu);
            g_state.hint = vs.hint;
        });
    smart_spectra.SetOnError([](const spectra::SmartSpectraError& error) {
        fprintf(stderr, "[smartspectra] %s\n", error.FullMessage().c_str());
    });

    g_state.start_t = NowSeconds();

    // macOS shows the camera-permission prompt the first time this opens the camera.
    if (const auto err = smart_spectra.UseCamera().Build(); !err.ok()) {
        fprintf(stderr, "UseCamera failed: %s\n", err.message.c_str());
        return EXIT_FAILURE;
    }
    if (const auto err = smart_spectra.Start(); !err.ok()) {
        fprintf(stderr, "Start failed: %s\n", err.FullMessage().c_str());
        return EXIT_FAILURE;
    }

    httplib::Server server;
    g_server = &server;
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    server.set_mount_point("/", absl::GetFlag(FLAGS_web_root));

    server.Get("/stream", [](const httplib::Request&, httplib::Response& res) {
        res.set_chunked_content_provider(
            "multipart/x-mixed-replace; boundary=frame",
            [last_seq = uint64_t{0}](size_t, httplib::DataSink& sink) mutable {
                std::string jpeg;
                {
                    std::lock_guard<std::mutex> lock(g_state.mu);
                    if (g_state.jpeg_seq != last_seq) {
                        last_seq = g_state.jpeg_seq;
                        jpeg = g_state.jpeg;
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
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
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
    printf("doomstare running — open http://localhost:%d (Ctrl+C to stop)\n", port);
    server.listen("127.0.0.1", port);

    (void)smart_spectra.Stop();
    printf("Done.\n");
    return 0;
}
