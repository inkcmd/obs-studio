# XINKX Studio — Stage 1 (0.15-dev)

This directory contains code linked directly into the Qt frontend of the OBS Studio fork.
There are **no additional runtime services or separately installed plugins**.

## Implemented

- `StudioController`: read-only model of the active OBS scene, selected scene item,
  streaming status, and recording status. The model reads directly from libobs.
- `StudioWorkspace`: native Qt toolbar and source inspector. The inspector uses
  the existing OBS source-properties command; there is no second source database.
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

1. Replace the informational inspector with native editing of OBS scene-item
   transform and properties; preserve OBS undo/redo and collection serialization.
2. Implement direct independent RTMP/RTMPS outputs in `OutputManager`.
3. Port OAuth/platform chat and local overlay assets from XINKX Studio 0.14.5.
4. Rework the permanent config namespace and installer; rename the executable,
   update virtual camera integration, and implement XINKX-specific updates.
