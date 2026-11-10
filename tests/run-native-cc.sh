#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
: "${ST_OBS_HEADERS:=/Users/yousufsulaimaan/st-obs-reliability/obs-headers/libobs}"
: "${ST_OBS_GENERATED:=/Users/yousufsulaimaan/st-obs-reliability/genhdr}"
: "${ST_OBS_FRAMEWORKS:=/Applications/OBS.app/Contents/Frameworks}"
mkdir -p build-local
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g tests/native-cc-protocol.test.c -o build-local/native-cc-protocol-test
build-local/native-cc-protocol-test
for test in json output properties; do
 clang -std=c11 -O1 -Wall -Wextra -Werror -Wno-unused-parameter -Wl,-export_dynamic -o "build-local/native-cc-${test}-test" "tests/native-cc-${test}.test.c" \
  clipboard-macos.m -framework AppKit \
  -I "$ST_OBS_HEADERS" -I "$ST_OBS_GENERATED" -I /opt/homebrew/include \
  $(pkg-config --cflags --libs libwebsockets) -F "$ST_OBS_FRAMEWORKS" -framework libobs -Wl,-rpath,"$ST_OBS_FRAMEWORKS"
 "build-local/native-cc-${test}-test" > "build-local/native-cc-${test}-test.log" 2>&1
 tail -1 "build-local/native-cc-${test}-test.log"
done
clang -std=c11 -Wall -Wextra -Werror tests/clipboard-macos.test.m -framework AppKit -o build-local/clipboard-macos-test
build-local/clipboard-macos-test
clang -std=c11 -O1 -Wall -Wextra -Werror -Wno-unused-parameter -fPIC -bundle -o build-local/obs-streamtranslate-native-cc plugin.c \
 clipboard-macos.m -framework AppKit \
 -I "$ST_OBS_HEADERS" -I "$ST_OBS_GENERATED" -I /opt/homebrew/include \
 $(pkg-config --cflags --libs libwebsockets) -F "$ST_OBS_FRAMEWORKS" -framework libobs -Wl,-rpath,"$ST_OBS_FRAMEWORKS"
echo 'Actual macOS plugin binary compiled; no OBS profile or installed plugin changed.'
