# obs-streamtranslate

An OBS Studio audio filter that sends your audio to
[StreamTranslate](https://streamtranslate.live) for live translated captions —
built for IRL streamers and cloud OBS setups (IRLToolkit and similar) where
there's no browser tab available to capture a microphone.

**Full setup guide: https://streamtranslate.live/obs-plugin.html**

## How it works

Two pieces, and they do different jobs:

- **This plugin** is an audio filter. You attach it to your mic or stream audio
  source and it quietly sends a copy of your audio to StreamTranslate. It does
  not alter your audio and nothing appears on your stream.
- **Your captions overlay** is a normal Browser Source using the overlay URL from
  your StreamTranslate account. That's what actually displays the captions.

Filter = the microphone. Browser source = the screen.

Public releases use one combined audio/caption stream. Two-speaker mode is not
available yet. Version 0.1.11 removes its unfinished checkbox and ignores an
older saved stereo-split setting. Separate development builds are for staging
tests only; they are not part of these downloads.

## Install

Grab the latest build from [Releases](https://github.com/StreamTranslate/obs-streamtranslate/releases/latest).

### macOS

Quit OBS, open the signed and Apple-notarized `.pkg`, finish installation,
then reopen OBS. A normal administrator-password prompt may appear.
If macOS reports that the installer cannot be verified, contact support.

Restart OBS after installing.

### Windows

Close OBS before copying the files.

**Standard Windows installation (OBS 32 or OBS 33):** download
`StreamTranslate-OBS-Plugin-Windows.zip` and extract its `obs-streamtranslate`
folder into `%ProgramData%\obs-studio\plugins` (normally
`C:\ProgramData\obs-studio\plugins`). Create the `plugins` folder if needed.
Allow the Windows administrator prompt if it appears.

The resulting plugin folder contains `obs-streamtranslate.dll` and `cacert.pem`.
Keep both files together: the certificate file is needed to connect securely.
The same folder also contains a `bin\64bit` copy for OBS 32 compatibility.
OBS 33 loads the flat copy first and skips the legacy copy, so the filter loads
only once. Keep both copies from the same ZIP when updating.

**Portable OBS 33:** extract that same plugin folder into the portable OBS
installation's `plugins` folder. Portable OBS ignores the ProgramData location.

**Portable OBS 32:** use `StreamTranslate-OBS-Plugin-Windows-OBS32-Portable.zip`
and extract it into the portable OBS root, placing the files in
`obs-plugins\64bit`. This is a legacy layout; use the main ZIP when upgrading
to portable OBS 33.

Restart OBS and check **Audio source → Filters → + → StreamTranslate**. Existing
filter settings and your Plugin Key stay in the OBS configuration.

When updating an existing OBS 32 installation that still has the plugin in its
OBS installation folder, use the OBS32 ZIP in that same folder instead of
installing an additional ProgramData copy. The OBS32 ZIP also retains the old
standard-install layout. When upgrading to OBS 33, use the main ZIP.

OBS's [plugin-location documentation](https://obsproject.com/kb/legacy-plugin-locations)
explains the new layout. These packages do not require deleting old plugin files;
OBS 33 skips an older duplicate after loading the new copy successfully.

### Linux / cloud OBS

Use the `.so` from Releases, or build from source (below).

Cloud OBS providers don't let you put files on their servers, so open a support
ticket asking them to install this plugin and link this repository. It's open
source and Linux-compilable, which is what providers such as IRLToolkit require.
Everything after that you do yourself from your OBS remote.

## Setup

1. Right-click your audio source → **Filters** → **+** → **StreamTranslate**
2. Paste your **Plugin Key** (generate it on your StreamTranslate control page
   under *Cloud OBS / IRL Mode*)
3. Add your overlay URL as a **Browser Source** to display the captions

The filter shows a live status line — it counts audio as you speak when
connected, and names the problem when it isn't.

## Using it

- **Change languages:** from your control page, in any browser including your
  phone. Applies live, never asks for microphone access.
- **Pause:** mute the audio source in OBS, or disable the filter (the eye icon).
  Captions stop immediately.
- **Stop:** the control page shows a *Stop stream* button while the plugin is
  connected.

## Building from source (Linux)

```bash
sudo apt install build-essential cmake pkg-config libobs-dev libssl-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release -DST_STATIC_LIBWEBSOCKETS=ON
cmake --build build
```

The module is `build/obs-streamtranslate.so`. This source build embeds a
checksum-pinned libwebsockets, so cloud servers do not need a separate
libwebsockets package. OBS and system TLS libraries are still required.

Verify with `bash scripts/check-static-lws.sh build/obs-streamtranslate.so`.
The first configure downloads the pinned upstream source. For offline builds,
set `FETCHCONTENT_SOURCE_DIR_LWS` to an already verified source checkout.
To deliberately use a system shared library instead, set
`ST_STATIC_LIBWEBSOCKETS=OFF` and install `libwebsockets-dev`.

This changes packaging only, not the currently published release. It does not
provide an automatic updater: providers must install new plugin builds and
restart OBS. Website/server updates do not require reinstalling the plugin.

## Privacy

Audio is only sent while the filter is enabled and the source is unmuted.
Muting in OBS stops transmission immediately.

## Preparing a signed macOS release

CI builds the package input. Before publishing a release, sign the plugin with
`scripts/macos-sign.sh`, build the installer from that signed payload, and run
`scripts/macos-notarize.sh` on the installer. The latter requires Apple to return
Accepted, staples the ticket, and checks Gatekeeper before producing a release
package. Keep Developer ID keys and notarization credentials in the signing
keychain; raw CI Mac installers are not release downloads.
