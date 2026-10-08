#!/bin/bash
# Sign nested libraries first; never use --deep as a signing shortcut.
set -euo pipefail
: "${MACOS_APPLICATION_IDENTITY:?Developer ID Application identity required}"
: "${MACOS_TEAM_ID:?Apple team ID required}"
[[ "$MACOS_APPLICATION_IDENTITY" == "Developer ID Application: "* ]] || { echo 'Not a Developer ID Application identity' >&2; exit 1; }
bundle=${1:?Usage: macos-sign.sh path/to/plugin.bundle}
[[ -d "$bundle/Contents/MacOS" ]] || { echo 'Plugin bundle missing' >&2; exit 1; }
# Finder metadata from expanded installer payloads is not part of the code.
# Do not remove quarantine or change Gatekeeper settings.
xattr -dr com.apple.FinderInfo "$bundle" 2>/dev/null || true
xattr -dr com.apple.ResourceFork "$bundle" 2>/dev/null || true
keychain_args=()
if [[ -n "${MACOS_KEYCHAIN_PATH:-}" ]]; then keychain_args=(--keychain "$MACOS_KEYCHAIN_PATH"); fi
for lib in "$bundle"/Contents/Frameworks/*.dylib; do
  [[ -f "$lib" ]] || continue
  codesign --force --timestamp --options runtime --sign "$MACOS_APPLICATION_IDENTITY" ${keychain_args[@]+"${keychain_args[@]}"} "$lib"
done
xattr -d com.apple.FinderInfo "$bundle" 2>/dev/null || true
xattr -d com.apple.ResourceFork "$bundle" 2>/dev/null || true
codesign --force --timestamp --options runtime --sign "$MACOS_APPLICATION_IDENTITY" ${keychain_args[@]+"${keychain_args[@]}"} "$bundle"
xattr -d com.apple.FinderInfo "$bundle" 2>/dev/null || true
codesign --verify --deep --strict --verbose=2 "$bundle"
metadata=$(codesign --display --verbose=4 "$bundle" 2>&1)
grep -Fqx "TeamIdentifier=$MACOS_TEAM_ID" <<< "$metadata"
grep -Fq 'Authority=Developer ID Application:' <<< "$metadata"
grep -Fq 'Timestamp=' <<< "$metadata"
echo 'Developer ID bundle signature verified'
