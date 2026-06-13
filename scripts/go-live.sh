#!/bin/bash
# Launch EYE FIGHT live at https://eyefight.xyz
#
# Starts the game server (Presage SDK) and the Cloudflare Tunnel together,
# and stops both cleanly on Ctrl+C.
#
# Usage:
#   SMARTSPECTRA_API_KEY=your_key ./scripts/go-live.sh
#   # or pass it inline:
#   ./scripts/go-live.sh --api_key=your_key
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/eyefight"
KEY="${SMARTSPECTRA_API_KEY:-}"

# allow --api_key=... passthrough
for arg in "$@"; do
  case "$arg" in
    --api_key=*) KEY="${arg#--api_key=}" ;;
  esac
done

if [[ ! -x "$BIN" ]]; then
  echo "Build first:  cmake --build build -j" >&2
  exit 1
fi
if [[ -z "$KEY" ]]; then
  echo "Set your Presage key:  SMARTSPECTRA_API_KEY=... ./scripts/go-live.sh" >&2
  exit 1
fi

cleanup() {
  echo "\nshutting down…"
  [[ -n "${GAME_PID:-}" ]] && kill "$GAME_PID" 2>/dev/null || true
  [[ -n "${TUN_PID:-}"  ]] && kill "$TUN_PID"  2>/dev/null || true
}
trap cleanup INT TERM EXIT

echo "▶ starting game server on :8428"
"$BIN" --api_key="$KEY" --host=127.0.0.1 --port=8428 &
GAME_PID=$!

# wait for the server to answer before opening the tunnel
for i in $(seq 1 30); do
  if curl -s -m 1 -o /dev/null http://localhost:8428/; then break; fi
  sleep 1
done

echo "▶ starting Cloudflare Tunnel → https://eyefight.xyz"
cloudflared tunnel run eyefight &
TUN_PID=$!

echo ""
echo "  ✅ EYE FIGHT is live at https://eyefight.xyz"
echo "     (Ctrl+C to take it offline)"
echo ""
wait
