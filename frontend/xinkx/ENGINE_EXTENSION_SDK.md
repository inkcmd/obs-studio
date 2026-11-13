# XINKX Studio — Engine Extension SDK (in-process v1)

## Status and scope

This contract is intended for **new C++ engines compiled into the same OBS
frontend process**. It does not load third-party DLLs, JavaScript, arbitrary
scripts, or background processes. The OBS scene graph and OAuth store remain
owned by their existing engines.

Before adding a new capability, inspect `EngineRegistry::inventory()` and
`EngineRegistry::actionCatalog()` as well as the source implementation. An
existing module must not be installed a second time.

## Existing building blocks from the uploaded frontend snapshot

| Component | Existing responsibility | Do not duplicate |
| --- | --- | --- |
| EventCore | Dedup, ordered bounded delivery, persisted history | Provider ingress and replay |
| ChatGateway / DonationGateway | Provider adapters | New transport for each feature |
| AlertEngine / OverlayRuntime | Alerts, presentation, local SSE | Second event-to-overlay path |
| EventAutomation / StudioAutomation | Rule evaluation and safe OBS actions | Second command executor |
| MultiStreamManager / PortraitCanvas | Output destinations, 9:16 mixer | Separate OBS process |
| RecordingEngine | Named independent capture jobs | Source Record plugin dependency |
| EngineRegistry | Existing inventory and UI overview | Second registry |

## Engine lifecycle

1. Derive from `EngineExtension` and implement `moduleId`, `moduleTitle`,
   `capabilities`, `actions`, and `execute`. Optionally implement `onEvent` and
   `health`. The contract is in `EngineExtension.hpp`.
2. Add the `.cpp/.hpp` to `frontend/cmake/feature-xinkx-studio.cmake`.
3. Register exactly one instance with `EngineRegistry::registerExtension`
   while setting up the frontend, **after EventCore exists, before rules load**.
   The extension's QObject parent owns it; the registry only keeps a QPointer.
4. Expose any controls in the existing XINKX Studio tabs. The registry inventory
   and action catalog update automatically; no new network service is needed.
5. Add checks for module/action ID collisions, rule permissions, persistence,
   shutdown and existing regression tests.

## Action contract

Action IDs are globally unique and use `module.action` namespacing. The
`obs` adapter retains legacy commands like `scene.activate` and
`source.visible`. Each `EngineAction` declares whether it is `automatable`.

`EngineRegistry::executeAction({"command":"markers.add","label":"Boss fight"})`
executes on the UI thread. `executeAction(command, true)` is the **event-rule
path**; it rejects manual-only actions before calling the extension. Arguments
are capped at 4096 JSON bytes. No arbitrary process execution or unaudited
network writes are permitted through this contract.

`EventAutomation` reads its allowed action IDs and simple argument schemas
(`argumentKey`, `argumentType`, `defaults`) from the registry instead of a
hardcoded list. Existing rules keep their command IDs and JSON format.
More complex arguments will require a future schema/widget extension.

## Events

A registered extension receives `onEvent(const StudioEvent&)` only for
**live accepted events** matching the explicitly declared `eventTypes()` list,
downstream of `EventCore::delivered`. Empty subscriptions mean no event payload
is shared with the extension. EventCore remains the
single ingress point, and past journal entries are not replayed to extensions.
Extensions must never retain raw OAuth tokens or raw provider payloads.

## Example engine: Markers

`MarkerEngine` shows how to add functionality without editing ChatGateway,
EventCore or OverlayRuntime: a tab for manual timeline bookmarks, optional
follow/sub/raid/donation markers, and CSV export. Auto markers are disabled by
default. It records only local timestamps, type/platform, and user-entered
labels; it never stores viewer names or messages. Records are capped at 1000
and persisted atomically in `xinkx-studio/markers.json`.

**Timing limit:** CSV `elapsed_since_xinkx_launch_ms` is relative to the
XINKX process startup, not a media file timecode. A later link to the current
RecordingEngine job file/PTS would be required for frame-accurate editing.

## Windows verification

- Compile: `cmake --build --preset windows-x64 --parallel 12`.
- Verify the existing `OBS Actions` inventory plus the new `markers` module.
- Add a marker; restart; verify persisted data and CSV export.
- Create an EventAutomation rule (`donation` → `markers.add`); test against a
  private donation test event and confirm exactly one mark.
- Keep automatic event markers off; confirm no auto marks appear.
- Close XINKX normally and confirm no crash reports. The sandbox cannot
  validate native MSVC runtime behavior.
