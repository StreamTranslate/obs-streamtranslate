#!/usr/bin/env bash
set -euo pipefail
binary=${1:?Usage: check-static-lws.sh path/to/plugin}
magic=$(od -An -tx1 -N4 "$binary" | tr -d ' \n')
case "$(uname -s)" in
  Linux)
    [[ "$magic" == 7f454c46 ]] || { echo 'FAIL: expected an ELF binary on Linux' >&2; exit 1; }
    dynamic=$(readelf -d "$binary")
    dependencies=$(grep NEEDED <<< "$dynamic" || true)
    ;;
  Darwin)
    case "$magic" in
      cffaedfe|cefaedfe|feedfacf|feedface|cafebabe|bebafeca|cafebabf|bfbafeca) ;;
      *) echo 'FAIL: expected a Mach-O binary on macOS; inspect Linux binaries inside Linux' >&2; exit 1 ;;
    esac
    dependencies=$(otool -L "$binary")
    if grep -qi 'is not an object file\|truncated or malformed object' <<< "$dependencies"; then
      echo 'FAIL: cannot inspect this binary' >&2; exit 1
    fi
    ;;
  *) echo 'Dependency inspection requires Linux or macOS' >&2; exit 2 ;;
esac
printf '%s\n' "$dependencies"
if grep -qi 'libwebsockets\|libwebsocket\.' <<< "$dependencies"; then
  echo 'FAIL: plugin still depends on a shared libwebsockets' >&2
  exit 1
fi
echo 'PASS: no shared libwebsockets dependency'
