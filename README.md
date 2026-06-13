# EYE FIGHT 👁

**The staring contest with a computer-vision referee.**

A camera-vitals *game* built for the MLH "Build A Custom Camera Vitals Screen"
challenge using the
[Presage SmartSpectra C++ SDK](https://github.com/Presage-Security/SmartSpectra).

Open it in any browser — phone, laptop, whatever — enter your name, and stare.
A 3-second countdown, then the clock runs until the SDK's per-frame **blink
detection** catches you. Your time goes on the persistent **hall of dry eyes**
leaderboard. Spectators in the lobby watch the current player's camera and
timer live.

- 👁 blink detection ends the round — the SDK is the referee, no honor system
- 🙈 eyes must stay in frame: staring won't start until the SDK sees a face,
  and leaving the camera mid-stare ends the round at the moment your eyes left
- 👀 **eye-strip profiles**: with your consent, the SDK's face landmarks crop
  your two eyes — masked into floating ovals, no face — from the frame at the
  moment of your fatal blink; that becomes your leaderboard avatar. One
  profile per name: replays keep your best score and refresh your eyes. Opt
  out and you get the eyeless ✕ ✕ default; opting out later removes stored eyes.
- 📤 **share your record** — Web Share sheet on phones (with your eye crop
  attached), X/Twitter post fallback on desktop
- 🏆 persistent leaderboard (JSON file), live spectator view, busy lobby states

## How it works

Presage has no web SDK — so the browser captures its **own** camera with
`getUserMedia` (camera permission + preview in our UI) and streams ~15 fps
JPEG frames to a C++ server, which pushes them into SmartSpectra through its
**CustomInput** frame-push API. Blink/face metrics flow back over
server-sent events.

```
phone/laptop browser                      C++ server (this repo)
┌─────────────────────┐   POST /api/frame   ┌──────────────────────────┐
│ getUserMedia preview│ ───── JPEGs ──────▶ │ stb_image → CustomInput  │
│ countdown / timer UI│                     │   SmartSpectra SDK       │
│ lobby + leaderboard │ ◀──── SSE ───────── │ blinks·landmarks → game  │
└─────────────────────┘      /events        │ leaderboard.json         │
        spectators ◀── MJPEG /stream ────── └──────────────────────────┘
```

One subtlety: SmartSpectra requires strictly monotonic frame timestamps with
no >2 s gaps, so the server maps each player's `performance.now()` clock onto
a virtual SDK clock that compresses the idle time between rounds.

Eye avatars use the SDK's `FACE_LANDMARKS` metric: on a dense (468-point)
mesh, each eye's corner/lid landmarks bound its own crop; the two crops are
masked into feathered ovals and composited onto a transparent PNG — two eyes,
no face. Only that strip is ever stored (base64 inside `leaderboard.json`) —
full frames are never persisted.

No frameworks — two single-header libraries
([cpp-httplib](https://github.com/yhirose/cpp-httplib),
[stb](https://github.com/nothings/stb)) and one HTML file.

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
./build/eyefight --api_key=YOUR_PRESAGE_API_KEY
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
./build/eyefight --fake          # SDK skipped, face presence simulated
curl -X POST localhost:8428/api/debug_blink   # simulate the fatal blink
```

## Tips for a good signal

Face the camera, fill the frame, decent lighting. The HUD shows the SDK's own
hints when the signal is bad — and if your eyes leave the frame, the round is
over.
