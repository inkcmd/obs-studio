# XINKX Studio — engine integration plan

One native Windows application, one OBS scene graph and video pipeline (`libobs`),
one source of truth for each provider account. Engine boundaries are C++/Qt
components inside the process, **not** separately launched web services.

Each phase must ship as one reviewable Git patch, with its own tests and a
Windows build/runtime checklist. `Implemented` is not the same as `compiled`
or `verified against live platforms`.

| Phase | Scope | Status (2026-11-11) |
| --- | --- | --- |
| **1. Event Core** | Unified incoming real events from chat, follows, memberships, raids, donations; stable IDs, dedup, bounded ordered fan-out, local journal, health counters | **Source implementation complete; Windows compile and live verification pending** |
| **2. Chat / Alerts / Automation** | Separate provider capability adapters, full free/paid event taxonomy, all permitted Twitch/YouTube/Kick subscriptions, moderation, rule-based event/action workflow and alerts editor | **Partial native implementation:** AlertEngine, paid/subtype metadata and local rules UI added; provider expansion and moderation pending; Windows build pending |
| **3. Scene Elements** | One OBS source/item model for 16:9 and 9:16; native shapes/text/gradients/masks/path drawing, keyframe animation, alignment, grouping, presets, property inspector, undo/redo | Planned; existing libobs sources, HTML-backed widgets and dual canvas editor remain functional |
| **4. LLM / Speech** | Optional local model adapters (Ollama), explicit safe tool permissions, contextual memory, TTS/STT, voice routing and in-studio commands | Planned; existing `StudioAutomation` allowlisted command executor is the starting point, not an LLM runtime |
| **5. Unified Studio / SDK** | Consolidate account/session models and workspace UI, extensible engines, diagnostics and plugin interfaces, installer and update/OBS upstream merge routine | Planned; existing OBS fork is operational source baseline |

## Phase 1 — delivery contract

- All **real** `ChatGateway` and `DonationGateway` events enter `EventCore`.
  Other subscribers receive normalized events after ordered core delivery,
  rather than sending independent notifications to the OBS overlay. The
  legacy `ChatLine` signal is now emitted by a core subscriber, so real
  DonationAlerts/custom-feed events also appear in the existing event feed.
- `externalId + sourceId + accountId` identifies an event when the provider
  provides a stable ID. YouTube linked outputs share a canonical chat account.
  Feed UUIDs are stable across renaming a donation source. Unidentified events
  receive random local IDs; content is not used for dangerous false dedup.
- Real events reach the overlay through one bounded Qt queue in acceptance
  order, at most 64 per GUI tick. At 2048 pending events chat is discarded
  before follows, subscriptions or donations. Counters expose duplicates and
  backpressure drops without logging private event content.
- `events.jsonl` is a local atomic, capped journal (latest 1000 events) next to
  portable `studio.ini`. It contains an explicitly allowlisted event schema:
  **chat text, user display names and donation amounts**, but never account
  secrets, stream keys, HTTP headers or raw provider JSON. The journal is a
  local data file; protect it when sharing a portable profile or logs.
- Previously saved events seed dedup on restart but are **never automatically
  played back as live alerts**. Existing overlay test buttons are intentionally
  synthetic and do not enter the real-event journal.
- Existing native OBS event display and reaction handlers remain the consumer.
  Phase 1 does not claim that unsupported Kick follow/sub events become
  available or that alerts work without an alert source present in the scene.

## Phase 1 — Windows acceptance checklist

1. Apply the patch to `xinkx/main` in `C:\codex-homelab\XINKX-Studio` and run
   `cmake --build --preset windows-x64 --parallel 12` after verifying CMake
   configuration. Source-contract tests alone are not a native build.
2. Create a private test stream, receive a real Twitch chat/follow and paid
   subscription (if authorized), a YouTube chat/Super Chat, and a DonationAlerts
   test payment. Confirm exactly one item in the chat/event timeline and exactly
   one visible overlay, provided a visible alert source is present.
3. Restart the app, inspect `config/xinkx-studio/events.jsonl` under the app's
   actual portable config directory. Old donations/alerts must not fire again.
   Confirm newly arrived events append while old entries are retained to 1000.
4. Verify the EventCore metrics label on the **Эффекты** page: accepted,
   duplicates, queued and dropped counts. Try temporarily hiding the alert
   source and confirm the existing delivery-health warning still appears.
5. Check the standalone streamer chat overlay, bot replies, independent RTMP
   outputs and 9:16 preview for regressions. Never test against a public stream
   without explicit intent.

## Phase 1 — known follow-ups

- Provider capability/permission negotiation and complete free/paid event
  taxonomy are Phase 2. Kick's undocumented websocket currently covers chat,
  not official follow/subscription events.
- `EventCore` persistently stores a bounded recent-event journal, not an
  unbounded audit/financial ledger. A future configurable SQLite event store
  and export/retention UI can be introduced if needed.
- Disk failures currently produce a visible local warning. Journal durability
  depends on filesystem write success; OS crash between buffered writes may
  lose the most recent 1.5 seconds of history.

## Phase 2 — implemented slice and verification

- A new `AlertEngine` consumes the accepted EventCore stream and routes live
  chat versus real alerts to the original OBS overlays. Free/paid counters
  appear beside EventCore delivery health; explicit YouTube/Twitch subtypes
  and purchase flags survive the journal. A gifted membership recipient is
  not counted as a new purchase by that recipient.
- `EventAutomation` stores up to 100 opt-in rules atomically in
  `event-rules.json` (portable XINKX settings folder). A **Правила** tab creates
  or removes rules, selecting event type, provider, text condition, cooldown
  and an allowed OBS action. Rules never run on replayed journal history.
- Allowed commands: activate an existing scene, show an existing source,
  trigger a known reaction or control the timer. Only `StudioAutomation` can
  perform actions. No arbitrary shell, scripts, public listener, hidden stream
  start/stop or unsupervised network API write is introduced.
- **Windows checks:** build, add donation → `effect.trigger` rule; trigger two
  identical events in quick succession and confirm cooldown; activate a scene
  using a manually generated chat event; restart and verify rule persistence.
- **Not claimed:** full Kick follow/sub webhooks, platform moderation,
  configurable native alert visuals, paid/free emote tiers or editable rule
  condition graphs. Those are still work within the broad Stage 2 scope.
