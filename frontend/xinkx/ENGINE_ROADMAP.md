# XINKX Studio — engine integration plan

One native Windows application, one OBS scene graph and video pipeline (`libobs`),
one source of truth for each provider account. Engine boundaries are C++/Qt
components inside the process, **not** separately launched web services.

Each phase must ship as one reviewable Git patch, with its own tests and a
Windows build/runtime checklist. `Implemented` is not the same as `compiled`
or `verified against live platforms`.

| Phase | Scope | Status (2026-11-12) |
| --- | --- | --- |
| **1. Event Core** | Unified incoming real events from chat, follows, memberships, raids, donations; stable IDs, dedup, bounded ordered fan-out, local journal, health counters | **Source implementation complete; Windows compile and live verification pending** |
| **2. Chat / Alerts / Automation** | Separate provider capability adapters, full free/paid event taxonomy, all permitted Twitch/YouTube/Kick subscriptions, moderation, rule-based event/action workflow and alerts editor | **Partial native implementation:** AlertEngine, paid/subtype metadata and local rules UI added; provider expansion and remote moderation pending; local opt-in chat filter added |
| **3. Scene Elements** | One OBS source/item model for 16:9 and 9:16; native shapes/text/gradients/masks/path drawing, keyframe animation, alignment, grouping, presets, property inspector, undo/redo | **Partial native implementation:** GPU-backed custom figures and gradients, normal OBS scene item editing, properties and undo; path tool and animation timeline pending; full Windows feature verification pending |
| **4. LLM / Speech** | Optional local model adapters (Ollama), explicit safe tool permissions, contextual memory, TTS/STT, voice routing and in-studio commands | **Partial native implementation:** Ollama client, session memory, explicit manual OBS action, Windows SAPI TTS; STT and rich tools pending; full Windows feature verification pending |
| **5. Unified Studio / SDK** | Consolidate account/session models and workspace UI, extensible engines, diagnostics and plugin interfaces, installer and update/OBS upstream merge routine | **Partial native implementation:** live engine overview, in-process capability registry, safe upstream integration script; plugin ABI/installer still pending; full Windows feature verification pending |

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

## Phase 2 — local moderation follow-up (next patch)

- `ModerationEngine` is a **local, opt-in filter** for incoming `ChatGateway` chat
  before `EventCore::publish`. Hidden chat therefore cannot trigger the bot,
  local overlays, automatic rules, the event feed or the local journal.
- Independent checks: up to 100 custom stop-phrases, links, repeats within
  15 seconds, per-viewer message rate (2–30 messages over 3–120 seconds).
  Only bounded per-viewer timestamps and last-message text are held in memory;
  no rejected message content is stored in diagnostics.
- Settings live in atomic portable `moderation.json`. The **Чат → Модерация**
  button edits the configuration and shows a session-level suppressed counter.
  Defaults to **off** to preserve existing broadcast behavior.
- This is **not remote moderation**: no YouTube/Twitch/Kick deletions, timeouts
  or bans are issued. OAuth scopes, permissions and per-platform enforcement
  remain an explicit follow-up. Paid notifications and follows are unaffected.
- **Windows checks:** build the patch, enable stop-phrase/link/repeat/flood
  filtering in a private test stream, ensure blocked chat is absent from the
  OBS overlay, chat window and bot triggers; ensure a paid alert still appears.
  Restart and check persisted toggles; test default-disabled configuration.

## Phase 3 — implemented slice and verification

- `xinkx_shape` is a real OBS video input source rendered through a libobs GPU
  texture, without CEF or any browser source. Its bitmap is drawn with Qt
  primitives and uploaded only after a property change, not every frame.
- **Эффекты → Нарисовать элемент** opens a local Qt figure creator with size,
  shape, name and color. New sources are added to the real current OBS scene
  and to OBS's native undo history. Right-click source → **Свойства** edits a
  rectangle, ellipse, line, triangle or progress indicator, two colors,
  optional linear gradient, transparent fill, outline and rounded corners.
- Every shape is an ordinary OBS source: use the native OBS source list for
  grouping, filters, ordering, capture, transforms and copy/paste. Portrait
  editor can reuse that same source or create another independent instance.
- **Windows checks:** create each shape, resize/rotate/crop through OBS and
  9:16 composition, edit properties while streaming, restart and reopen
  scenes. Verify background transparency, antialiasing, GPU usage and source
  destruction on a portable build. Qt/QPainter drawing itself is synchronous;
  the actual OBS video source compiles and renders only when validated on GPU.
- **Not claimed:** freehand pen/Bezier path editor, compositing masks, live
  keyframe timeline, audio-reactive expressions or complete vector animation.

## Phase 4 — implemented slice and verification

- Native **AI и голос** tab sends an explicit question to local Ollama at the
  fixed loopback endpoint `127.0.0.1:11434/api/chat`; no cloud provider,
  browser request, API key or outside listener is involved. Conversation
  history contains up to six user/assistant turns **in memory only**.
- The default model is the user's installed `qwen3-vl:8b-instruct` but is
  editable. Requests are disabled while OBS/multistream outputs are live,
  unless the user explicitly opts in for that request. This avoids silently
  loading a large model onto the same RTX GPU during a broadcast.
- The LLM is read-only. Suggested commands are shown in a separate editable
  JSON box and execute only after the user clicks **Выполнить проверенную
  команду OBS**; the existing `StudioAutomation` validates allowed operations.
- Optional paid-event speech uses Windows SAPI in a serialized background
  worker pool. It is disabled by default, capped to five queued utterances
  and persists its opt-in preference. **Проверить голос** speaks one sample.
- **Windows checks:** Ollama offline/online, invalid model, timeout, reply
  and reset, running stream block, explicit command approval, standalone
  OBS scene execution, voice playback and app shutdown during speech.
- **Not claimed:** voice input/STT, model fine-tuning, vision analysis of live
  video, voice chat bots, third-party cloud models, automatic LLM-initiated
  actions or mixing TTS into a dedicated OBS output track.

## Phase 5 — implemented slice and verification

- **Обзор** is now the default studio page. It shows one compact status view
  for Event Core, alerts, streaming outputs, provider chat, native scene
  elements and local assistants, with navigation to the original editors.
  It does not duplicate OBS's primary stream Start/Stop or recording controls.
- `EngineRegistry` exposes a QObject/JSON inventory of installed engine
  capabilities for future trusted integrations. It is not a dynamic C++ plugin
  loader or a promised cross-version binary ABI.
- `frontend/xinkx/tools/sync-obs-upstream.ps1` fetches official OBS changes
  and reports branch divergence. With explicit `-Integrate` it opens a new
  integration branch and starts `git merge --no-commit`, leaving the user's
  `xinkx/main` untouched. No force push, auto-commit or blind overwrites.
- **Windows checks:** registry lists all active components, Overview reflects
  RTMP changes and live event health, keyboard/source navigation unchanged,
  first startup after portable profile migration preserves source collection.
  Review upstream script in dry-run mode only before enabling integration.
- **Not claimed:** final unified UX for every OBS setting, public third-party
  plugin SDK, auto-updater, Windows installer/release signing and successful
  upstream OBS 33.x merge. These remain separate release engineering work.
