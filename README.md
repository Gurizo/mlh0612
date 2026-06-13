# DOOMSTARE 👁👁

**The staring contest your vitals can't lie about.**

A camera-vitals *game* built for the MLH "Build A Custom Camera Vitals Screen"
challenge using the
[Presage SmartSpectra C++ SDK](https://github.com/Presage-Security/SmartSpectra).

Open it in any browser — phone, laptop, whatever — enter your name, and stare.
A 5-second countdown, then the clock runs until the SDK's per-frame **blink
detection** catches you. Your time goes on the persistent **hall of dry eyes**
leaderboard, along with your average **pulse rate** while staring (measured
from your face, no contact). Spectators in the lobby watch the current
player's camera and timer live.

- 👁 blink detection ends the round — the SDK is the referee, no honor system
- ❤️ live pulse rate (BPM) on the game HUD, average BPM on the leaderboard
- 🏆 persistent leaderboard (JSON file), live spectator view, busy lobby states
- 👀 **eye-strip profiles**: with your consent, the SDK's face landmarks crop a
  tiny strip of *just your eyes* from the frame at the moment of your fatal
  blink — that becomes your leaderboard avatar. One profile per name: replays
  keep your best score and refresh your eyes. Opt out and you get the eyeless
  ✕ ✕ default; opting out on a later round takes your eyes back off the board.

## How it works

Presage has no web SDK — so the browser captures its **own** camera with
`getUserMedia` (camera permission + preview in our UI) and streams ~15 fps
JPEG frames to a C++ server, which pushes them into SmartSpectra through its
**CustomInput** frame-push API. Metrics flow back over server-sent events.

```
phone/laptop browser                      C++ server (this repo)
┌─────────────────────┐   POST /api/frame   ┌──────────────────────────┐
│ getUserMedia preview│ ───── JPEGs ──────▶ │ stb_image → CustomInput  │
│ countdown / timer UI│                     │   SmartSpectra SDK       │
│ lobby + leaderboard │ ◀──── SSE ───────── │ blinks·pulse → game state│
└─────────────────────┘      /events        │ leaderboard.json         │
        spectators ◀── MJPEG /stream ────── └──────────────────────────┘
```

One subtlety: SmartSpectra requires strictly monotonic frame timestamps with
no >2 s gaps, so the server maps each player's `performance.now()` clock onto
a virtual SDK clock that compresses the idle time between rounds.

Eye avatars use the SDK's `FACE_LANDMARKS` metric: on a dense (468-point)
mesh the eye/brow landmark indices bound the crop; sparser meshes fall back
to the upper-middle band of the landmark bounding box. Only the cropped eye
strip is ever stored (base64 inside `leaderboard.json`) — full frames are
never persisted.

No frameworks — two single-header libraries
([cpp-httplib](https://github.com/yhirose/cpp-httplib),
[stb_image](https://github.com/nothings/stb)) and one HTML file.

## Requirements

- macOS 14+ on Apple Silicon (server; players connect from anything)
- Xcode Command Line Tools, CMake ≥ 3.22, Homebrew
- A free API key from [physiology.presagetech.com](https://physiology.presagetech.com)

## Build & run

```bash
# 1. Install the SmartSpectra SDK
brew tap presage/smartspectra https://github.com/Presage-Security/homebrew-smartspectra
brew install presage/smartspectra/smartspectra

# 2. Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# 3. Run
./build/doomstare --api_key=YOUR_PRESAGE_API_KEY
```

Open **http://localhost:8428** and try not to blink. You will lose.

### Playing from phones

Browsers only allow camera access on secure origins, so `http://<lan-ip>`
won't work from a phone. Easiest fix — a free HTTPS tunnel:

```bash
brew install cloudflared
cloudflared tunnel --url http://localhost:8428
```

Share the printed `https://….trycloudflare.com` URL; everyone at the table
can join from their own phone.

### Dev mode (no API key)

```bash
./build/doomstare --fake          # synthetic pulse, SDK skipped
curl -X POST localhost:8428/api/debug_blink   # simulate the fatal blink
```

## Tips for a good signal

Face the camera, fill the frame, decent lighting, hold still-ish. The HUD
shows the SDK's own hints when the signal is bad.
