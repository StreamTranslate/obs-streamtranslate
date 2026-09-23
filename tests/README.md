# Native capture regression harness

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
