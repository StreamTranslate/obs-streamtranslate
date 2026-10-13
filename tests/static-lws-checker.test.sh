#!/usr/bin/env bash
# Invalid or wrong-format binaries must never be reported as dependency-free.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
printf 'MZinvalid' > "$scratch/windows.dll"
printf '\177ELF' > "$scratch/truncated-elf.so"
printf '\317\372\355\376' > "$scratch/truncated-macho.so"
for binary in "$scratch"/*; do
  if bash "$root/scripts/check-static-lws.sh" "$binary" > "$scratch/result.log" 2>&1; then
    echo "FAIL: checker accepted invalid input: $binary" >&2
    exit 1
  fi
done
echo 'PASS: invalid and wrong-format dependency inputs rejected'
