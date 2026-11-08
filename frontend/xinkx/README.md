# XINKX Studio — Stage 4 (0.15-dev)

This directory contains code linked directly into the Qt frontend of the OBS Studio fork.
There are **no additional runtime services or separately installed plugins**.

## Implemented

- `StudioController`: state of the active OBS scene, selected scene item,
  streaming and recording status; native transform editing with OBS undo/redo.
- `StudioWorkspace`: native docked source inspector: X/Y, size,
  aspect-ratio lock, rotation, four-sided cropping, fit and alignment. Alignment
  uses OBS's own scene-transform actions, retaining undo/redo.
- `MultiStreamManager`: any number of additional RTMP(S) outputs using native
  OBS services, H.264 encoders, AAC audio encoders, independent bitrates and
  resolutions, all managed by OBS's main streaming lifecycle.
- `MultiStreamWorkspace`: dynamic account list directly in the standard OBS
  Settings → Stream page (no independent dock or extra Start/Stop button).
- `OAuthLogin`: browser authorization-code flow with local callback and PKCE
  for YouTube/Kick; optional Twitch OAuth, ingest key retrieval via provider APIs.
- Standard OBS status bar displays combined bandwidth and dropped frames;
  primary OBS controls continue to drive the live session and timer.
- Settings for this UI are stored in `config/xinkx-studio/studio.ini` during local development.
- On Windows, RTMP keys and OAuth tokens are encrypted by Windows DPAPI before
  being saved to `studio.ini`, bound to the current Windows user account.
- Title and Windows resource metadata identify the application as XINKX Studio.

## Development build (Windows)

From `C:\Homelab\XINKX-Studio`:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64 --parallel 12
```

**Important**: the executable is still `obs64.exe`. OBS currently uses this
filename for virtual camera registration and for the updater. We will rename
the executable only after adapting those integrations.

This development build *forces OBS portable mode* and disables the stock OBS
updater. Its OBS-format profiles, scenes, and log files live beneath its own
build/runtime `config/` directory; it does not read or update the installed OBS
profile under `%APPDATA%\obs-studio`. The user must **not install this build
into Program Files**: the final app will use an independent writable XINKX
application-data location instead of mandatory portable mode.

## OAuth setup

1. Configure one **primary stream** using the original OBS Settings → Stream
   controls; it is needed for OBS's own status/timer and Start Streaming button.
2. Add additional Twitch, YouTube or Kick accounts in the XINKX section below
   the native stream settings. Select enabled accounts with checkboxes.
3. For OAuth, register a developer application with the platform, set redirect
   URI to `http://127.0.0.1:45678/`, and enter its Client ID and (when required)
   Client Secret into the account editor. No OAuth client credentials are
   bundled. Click OAuth · Подключить, then Получить ключ. Manual keys also work.
4. Click the **existing OBS Начать трансляцию** button. Enabled additional
   outputs start automatically and stop with the same OBS button. The independent
   OBS **Начать запись** button still controls recording, not multistream.

YouTube requires a pre-existing stream created in YouTube Studio to retrieve
an ingest key. If several streams exist, this version selects the first one;
selection and event creation remain future work. Token refresh is supported
when requesting keys. Kick APIs require the appropriate application permissions.

For genuine 2560×1440 output, configure **both Base and Output Resolution** in
OBS Settings → Video as 2560×1440. Setting only an additional stream to 1440p
upscales the OBS 720p output if the global scaled output is 1280×720.

## Not yet implemented

1. Automatic fallback on NVENC hardware-init failure, YouTube broadcast/event
   selection and provisioning, platform keychain support outside Windows.
2. Port platform chat and local overlay assets from XINKX Studio 0.14.5.
3. Multi-selection transforms, nested source groups, source-specific property UI.
4. Rework the permanent config namespace and installer; rename the executable,
   update virtual camera integration, and implement XINKX-specific updates.

## Manual verification on Windows

1. Start the `RelWithDebInfo` build. There must be no XINKX top toolbar or
   independent multistream dock; `Источник` remains the optional right dock.
2. Select a camera/image. Change size (with ratio lock on/off), rotation,
   cropping and all four align/fit actions; verify preview and Ctrl+Z/ Ctrl+Y.
3. Lock a source; the controls must be disabled. Test another source and Studio Mode.
4. Configure one primary OBS stream and add two separate additional accounts,
   with OAuth or manual RTMP(S) keys, and differing resolution/bitrate.
5. Start **OBS's primary stream**. Both additional outputs must start. Verify
   native live timer, bandwidth, status indicator and platform-side reception.
6. Stop **OBS's primary stream**. All additional outputs must stop. Restart
   the app: additional accounts and encrypted stream keys must persist.
7. Check that OBS recording still starts and stops independently.

**This stage has not yet been compiled with MSVC in the authoring environment.**
Do not use production stream keys for initial verification. Output launch
and GPU encoder initialization can fail depending on the installed graphics
drivers; failed outputs should display an OBS error rather than affect other
destinations or normal OBS recording.
