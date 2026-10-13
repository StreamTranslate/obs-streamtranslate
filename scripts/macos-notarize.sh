#!/bin/bash
# A rejected or incomplete notarization must never yield a distributable artifact.
set -euo pipefail
: "${MACOS_INSTALLER_IDENTITY:?Developer ID Installer identity required}"
: "${MACOS_TEAM_ID:?Apple team ID required}"
: "${NOTARY_KEYCHAIN_PROFILE:?Validated notarytool keychain profile required}"
[[ "$MACOS_INSTALLER_IDENTITY" == "Developer ID Installer: "* ]] || { echo 'Not a Developer ID Installer identity' >&2; exit 1; }
pkg=${1:?Usage: macos-notarize.sh path/to/installer.pkg}
[[ -f "$pkg" && "$pkg" == *.pkg ]] || { echo 'Installer package missing' >&2; exit 1; }
keychain_args=(); notary_args=()
if [[ -n "${MACOS_KEYCHAIN_PATH:-}" ]]; then
  keychain_args=(--keychain "$MACOS_KEYCHAIN_PATH")
  notary_args=(--keychain "$MACOS_KEYCHAIN_PATH")
fi
signed="${pkg%.pkg}.signed.pkg"
productsign --timestamp --sign "$MACOS_INSTALLER_IDENTITY" ${keychain_args[@]+"${keychain_args[@]}"} "$pkg" "$signed"
signature=$(pkgutil --check-signature "$signed")
grep -Fq "$MACOS_INSTALLER_IDENTITY" <<< "$signature"
result="${pkg%.pkg}.notarization.json"
xcrun notarytool submit "$signed" --keychain-profile "$NOTARY_KEYCHAIN_PROFILE" ${notary_args[@]+"${notary_args[@]}"} --wait --timeout 30m --output-format json > "$result"
# --wait alone is not enough: notarytool can return a rejected status.
python3 - "$result" <<'PY'
import json, sys
with open(sys.argv[1]) as f: result = json.load(f)
if result.get('status') != 'Accepted':
    sys.exit('Notarization not accepted; inspect the Apple submission log')
print('Apple notarization accepted: ' + result['id'])
PY
xcrun stapler staple "$signed"
xcrun stapler validate "$signed"
spctl --assess --type install --verbose=2 "$signed"
mv "$signed" "$pkg"
echo 'Signed, notarized, stapled installer verified'
