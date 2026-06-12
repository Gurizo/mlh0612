# DOOMSTARE 👁👁

**A camera vitals dashboard that catches you doom-staring at your screen.**

Built for the MLH "Build A Custom Camera Vitals Screen" challenge using the
[Presage SmartSpectra C++ SDK](https://github.com/Presage-Security/SmartSpectra).

Point your Mac's camera at your face and DOOMSTARE shows, live, in a custom web dashboard:

- ❤️ **Pulse rate (BPM)** with a heart that beats in sync and a scrolling waveform
- 👁 **Blink fatigue meter** — blinks/min from the SDK's per-frame blink detection.
  Healthy spontaneous blinking is ~12–20/min; when you stare at a screen it
  crashes. Stop blinking for 15 seconds and the dashboard calls you out.
- 🫁 **Breathing rate** as a bonus vital

Everything runs on-device via the SmartSpectra SDK; a small C++ app encodes the
camera preview as MJPEG and streams metrics over server-sent events to a local
dashboard — no frameworks, two single-header libraries
([cpp-httplib](https://github.com/yhirose/cpp-httplib),
[stb_image_write](https://github.com/nothings/stb)).

## Architecture

```
camera ──▶ SmartSpectra SDK ──▶ metrics callback (pulse, breathing, blinks, waveform)
                   │                        │
                   ▼                        ▼
            video frames ──▶ JPEG ──▶ C++ HTTP server (cpp-httplib)
                                            │
                          /stream (MJPEG)   │   /events (SSE, 10 Hz JSON)
                                            ▼
                              web dashboard (web/index.html)
```

## Requirements

- macOS 14+ on Apple Silicon
- Xcode Command Line Tools, CMake ≥ 3.22, Homebrew
- A free API key from [physiology.presagetech.com](https://physiology.presagetech.com)

## Build & run

```bash
# 1. Install the SmartSpectra SDK
brew tap presage/smartspectra https://github.com/Presage-Security/homebrew-smartspectra
brew install presage/smartspectra/smartspectra

# 2. Build (the linker ad-hoc signs the binary, which is all the SDK needs)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 3. Run — macOS will prompt for camera permission on first launch
./build/doomstare --api_key=YOUR_PRESAGE_API_KEY
```

Then open **http://localhost:8428**, allow a few seconds for the signal to
stabilize (sit still, decent lighting), and try not to blink. You will lose.
