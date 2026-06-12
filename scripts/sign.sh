#!/bin/bash
# Optional: re-sign the doomstare binary with your Apple Development identity.
#
# On Apple Silicon the linker ad-hoc signs binaries automatically, which is
# enough for the SmartSpectra API-key flow — you normally don't need this.
# Note: do NOT sign with keychain-access-groups entitlements unless you embed a
# provisioning profile; macOS AMFI SIGKILLs restricted-entitlement binaries
# that lack one.
set -euo pipefail

BINARY="${1:-build/doomstare}"

IDENTITY=$(security find-identity -v -p codesigning | grep "Apple Development" | head -1 | sed 's/.*"\(.*\)"/\1/')
if [[ -z "$IDENTITY" ]]; then
  echo "No Apple Development signing identity found. Open Xcode > Settings > Accounts to add one." >&2
  exit 1
fi

codesign --force --sign "$IDENTITY" "$BINARY"
echo "Signed $BINARY with '$IDENTITY'"
