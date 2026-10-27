# Native capture regression harness

## Update-notice regressions

`update-policy.test.c` tests stable numeric version ordering and rejects malformed,
prerelease, markup and oversized versions. It is standalone C11.
`native-update-harness.c` uses real libobs properties and the actual curl child.
Start `python3 tests/update-server.py` on localhost:8797, then run the harness.
It covers newer/equal/older releases, invalid or draft/prerelease metadata,
HTTP errors, redirects, overlarge/NUL/malformed payloads, timeout, fixed download
URL, no audio state changes, shared request deduplication and worker shutdown.
The fixture asserts that no Authorization or Cookie header is sent. The
production endpoint and HTTPS-only default are asserted before a test-only local
URL override. No account, speech provider or customer audio is used.
Set `ST_STUCK_HELPER` to the absolute path of a local executable that sleeps for
120 seconds (ignoring its arguments). This proves the parent kills a stuck
DNS/network process at its deadline and promptly on shutdown. The executable and
HTTP fixture overrides exist only with `ST_UPDATE_TEST_MODE`, not plugin builds.
Build like the native audio harness below. Passing any argument instead makes
one read-only HTTPS check against the public GitHub release API.

Neither the native harness nor a successful compile proves a signed/notarized
installer, physical-device streaming, or Windows/Linux runtime acceptance.

`native-audio-harness.c` uses real libobs and the plugin implementation with a generated 48kHz stereo source (speech-like signal only on the right). Its compile-time test wrapper redirects the transport to localhost:8796, without changing the production TLS code. No hardware, account, provider, or user OBS profile is used.

Run the StreamTranslate branch's `scripts/test-native-audio.cjs` first, then this harness. It injects a second filter with no callbacks, mute/unmute, a 28-second callback interruption and a forced server-side TCP disconnect. Results go to `/tmp/st-native-audio-results.json`; OBS log goes to stdout. Wait for the server's 115-second completion before analyzing.

Build on macOS with official OBS headers matching the installed OBS app, generated obsconfig.h, and Homebrew libwebsockets/pkg-config/simde:

```sh
clang -O1 -Wall -Werror -o dist/native-audio-test tests/native-audio-harness.c \
  -I obs-headers/libobs -I genhdr -I /opt/homebrew/include \
  $(pkg-config --cflags --libs libwebsockets) \
  -F /Applications/OBS.app/Contents/Frameworks -framework libobs \
  -Wl,-rpath,/Applications/OBS.app/Contents/Frameworks
```

The headless libobs harness can emit graphics-context cleanup warnings at shutdown; it has no video context. This does not validate a physical HDMI/USB card, OS enumeration, exclusive device access, sleep, sample-rate changes during streaming, YoloLiv firmware, or successful live-provider captions. Browser device-discovery simulations are separate tests in StreamTranslate.

## Deployment dependency

**Deploy the StreamTranslate server's PCM/text separation and admission protection before distributing this plugin.** Old servers may treat diagnostic text as audio. This is a candidate build, not an installed/released plugin.

The plugin reports numerical channel peaks, callback/conversion/delivery counters, filter instance ID and source states. It sends no raw audio beyond the existing caption stream, no source names, credentials or captions in diagnostics. Offline diagnostic snapshots are bounded to 16 entries and are also written to the ordinary OBS log; capture callbacks never wait for diagnostic uploads. Logs cannot establish the final cause of an OS/app kill.

## Retry classification regression

`retry-policy.test.c` checks terminal, missing-input and transient close codes.
`native-retry-harness.c` plus `retry-server.cjs` runs the actual libobs/resampler/WebSocket path. It verifies 4008 and 4403 stay paused until Reconnect, 4010 does not retry silent callbacks, real PCM wakes the no-input pause, and 1012 reconnects with delivered PCM. Build like the native audio harness; run the server then the harness (about 30 seconds). The status after an idle shutdown requires an explicit Reconnect, matching Control's deliberate idle stop.

`native-speech-harness.c` loops a generated 16kHz mono PCM fixture through the actual plugin's right-channel 48kHz input; a test proxy can inject a TCP break and observe translated captions from a QA room. This is synthetic input, not physical capture-card validation.

## Explicit native Stop intent

`native-stop-harness.c` drives real libobs audio/resampling and the candidate
WebSocket client against a loopback-only server. It proves that Stop sends the
bounded `capture_stop` v1 control before a graceful close, clears queued PCM,
stays paused, and resumes only after Reconnect. Two Stop clicks produce one
message. The server accepts this only on the admitted, current plugin socket;
a retired filter cannot stop a replacement. Missing delivery stays unknown,
not inferred owner intent. The change is not installed or released. Normal OBS
quit/filter removal is deliberately not labeled a physical user action by this
protocol.

Run the integration fixture with
`ST_SERVER_ROOT=/absolute/path/to/server node tests/stop-server.cjs`, then the
compiled `native-stop-harness` (same build flags as the retry harness). It uses
the actual server control parser and Defender module, not a mock Stop handler.
Both processes exit after a bounded test and never contact production.

## Reconnect opening-syllable regression

The retry fixture now delays the fourth transport handshake by900ms and sends a
quiet200ms marker at the start of resumed voice. The server asserts that this
opening marker arrives, rather than only the louder later audio. The prior
connected-only capture path fails this check. Same-capture audio is now queued
for at most3seconds and256chunks while connecting; idle silence, terminal and
conflict states, and explicit Stop do not accumulate a pre-roll. Expired queue
entries are discarded and counted, so a long outage does not replay old speech.
The harness also checks expiry, capacity and all pause gates. This is a local
synthetic PCM regression, not proof of live-provider transcription accuracy.

Reliability release keeps public single-speaker mode. Teardown no longer calls unbounded lws_service after cancellation.

## Pause / Start and independent connection test

`native-controls-harness.c` uses real libobs properties plus a loopback-only
HTTP health fixture (`python3 tests/connection-server.py`, port8798). Build with
`ST_CONNECTION_TEST_MODE` supplied by the harness only. It checks morphing
Pause/Start labels and intent, one-second counter snapshots, probes while active
and paused without changing capture identity/audio state, malformed health JSON,
redirect/error/oversize rejection, host validation, and bounded cancellation.
The fixture rejects credentials. A successful health probe is **not** proof of
plugin-key authentication or end-to-end captions. Production builds require
HTTPS, never follow redirects, and have no test URL override.

The update-server fixture's new/equal/older tags are0.1.14/0.1.13/0.1.12 to match
current release0.1.13; these are test metadata, not a published version bump.
