#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
for test in status-refresh setup-recovery separate-pair waiting-primary; do
 clang -std=c11 -O1 -Wall -Wextra -Werror -Wno-unused-parameter "tests/$test.test.c" -o "build-local/$test-test" \
  clipboard-macos.m -framework AppKit \
 -I /Users/yousufsulaimaan/st-obs-reliability/obs-headers/libobs -I /Users/yousufsulaimaan/st-obs-reliability/genhdr -I /opt/homebrew/include \
 $(pkg-config --cflags --libs libwebsockets) -F /Applications/OBS.app/Contents/Frameworks -framework libobs -Wl,-rpath,/Applications/OBS.app/Contents/Frameworks
done
build-local/status-refresh-test > build-local/status-refresh.log 2>&1
WS_MODULE=/Users/yousufsulaimaan/st-shared-native-staging-20261023/node_modules/ws node tests/setup-recovery-server.cjs > build-local/setup-server.log 2>&1 &
server_pid=$!
trap 'kill "$server_pid" 2>/dev/null || true' EXIT
build-local/setup-recovery-test > build-local/setup-recovery.log 2>&1
wait "$server_pid"
tail -1 build-local/status-refresh.log
tail -1 build-local/setup-server.log
tail -1 build-local/setup-recovery.log
WS_MODULE=/Users/yousufsulaimaan/st-shared-native-staging-20261023/node_modules/ws node tests/separate-pair-server.cjs > build-local/separate-pair-server.log 2>&1 &
server_pid=$!
build-local/separate-pair-test > build-local/separate-pair.log 2>&1
wait "$server_pid"
tail -1 build-local/separate-pair-server.log
tail -1 build-local/separate-pair.log

WS_MODULE=/Users/yousufsulaimaan/st-shared-native-staging-20261023/node_modules/ws node tests/waiting-primary-server.cjs > build-local/waiting-primary-server.log 2>&1 &
server_pid=$!
build-local/waiting-primary-test > build-local/waiting-primary.log 2>&1
wait "$server_pid"
tail -1 build-local/waiting-primary-server.log
tail -1 build-local/waiting-primary.log
