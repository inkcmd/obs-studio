# XINKX Studio — Stage 2 (0.15-dev)

This directory contains code linked directly into the Qt frontend of the OBS Studio fork.
There are **no additional runtime services or separately installed plugins**.

## Implemented

- `StudioController`: state of the active OBS scene, selected scene item,
  streaming status, and recording status; direct transform editing with OBS undo/redo.
- `StudioWorkspace`: native Qt toolbar and docked source inspector with X, Y,
  width, and height fields. The existing OBS source-properties command remains
  available; there is no second scene/source database.
- Settings for this UI are stored in `config/xinkx-studio/studio.ini` during local development.
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

## Next steps

1. Add native editing for OBS source-specific settings, cropping, alignment, and
   multiple-selection transforms; handle nested source groups.
2. Implement direct independent RTMP/RTMPS outputs in `OutputManager`.
3. Port OAuth/platform chat and local overlay assets from XINKX Studio 0.14.5.
4. Rework the permanent config namespace and installer; rename the executable,
   update virtual camera integration, and implement XINKX-specific updates.

## Manual verification on Windows

1. Start the `RelWithDebInfo` build. Confirm the inspector is docked to the **right**
   and the STREAM/REC labels are separated.
2. Add an image or camera source and select it. Change X/Y and width/height,
   pressing Enter to apply. OBS Preview must move/resize the *same* source.
3. Press Ctrl+Z and Ctrl+Y. The source's transform must undo/redo, and the
   inspector values must update.
4. Lock the OBS scene item: editing must be disabled. Test Studio Mode selection.
