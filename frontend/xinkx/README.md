# XINKX Studio — Stage 3 (0.15-dev)

This directory contains code linked directly into the Qt frontend of the OBS Studio fork.
There are **no additional runtime services or separately installed plugins**.

## Implemented

- `StudioController`: state of the active OBS scene, selected scene item,
  streaming and recording status; native transform editing with OBS undo/redo.
- `StudioWorkspace`: native Qt toolbar and docked source inspector: X/Y, size,
  aspect-ratio lock, rotation, four-sided cropping, fit and alignment. Alignment
  uses OBS's own scene-transform actions, retaining undo/redo.
- `MultiStreamManager` and `MultiStreamWorkspace`: three direct independent
  RTMP(S) destinations with separate H.264 encoders, AAC audio encoders,
  bitrates and output resolutions (Twitch, YouTube and Kick). Includes
  per-platform start/stop, start all/stop all, status and transferred bytes.
- The inspector and multistream panels are native tabbed docks on the same
  right sidebar; OBS remains the sole authority for scenes and source data.
- Settings for this UI are stored in `config/xinkx-studio/studio.ini` during local development.
- **RTMP stream keys are memory-only** in this stage; they are never saved to
  `studio.ini` and must be reentered after an application restart. Streaming
  endpoints and encoder settings are persisted. OAuth is not integrated yet.
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

## Not yet implemented

1. Automatic fallback on NVENC hardware-init failure, secure persistent token
   storage via platform credential store, OAuth/stream-event provisioning.
2. Port platform chat and local overlay assets from XINKX Studio 0.14.5.
3. Multi-selection transforms, nested source groups, source-specific property UI.
4. Rework the permanent config namespace and installer; rename the executable,
   update virtual camera integration, and implement XINKX-specific updates.

## Manual verification on Windows

1. Start the `RelWithDebInfo` build. There must be two tabs on the **right**:
   `Источник` and `Мультистрим`. No undocked windows should cover preview.
2. Select a camera/image. Change size (with ratio lock on/off), rotation,
   cropping and all four align/fit actions; verify preview and Ctrl+Z/ Ctrl+Y.
3. Lock a source; the controls must be disabled. Test another source and Studio Mode.
4. In `Мультистрим`, enter an RTMP(S) URL and stream key for a test destination,
   choose resolution/bitrate and press `Запустить`.
5. Start another configured destination with different output settings. Check
   individual status and that both platforms receive different resolutions.
6. Stop one, then stop all. Restart the app: URL, bitrate, resolution and
   encoder choice should remain; secret keys must be blank.

**Do not use production stream keys for initial verification.** Output launch
and GPU encoder initialization can fail depending on the installed graphics
drivers; failed outputs should display an OBS error rather than affect other
destinations or normal OBS recording.
