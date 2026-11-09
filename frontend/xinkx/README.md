# XINKX Studio — Stage 8 (0.15-dev)

The Qt frontend uses the OBS scene graph, video pipeline and output handlers. The source inspector edits OBS scene items and uses the native undo/redo stack.

## Build on Windows

```powershell
Set-Location C:\codex-homelab\XINKX-Studio
cmake --preset windows-x64
cmake --build --preset windows-x64 --parallel 12
```

The portable runtime is in `build_x64/rundir/RelWithDebInfo`. Start `bin/64bit/obs64.exe` with `bin/64bit` as its working directory. Its profiles and scenes are under the runtime's `config/` directory. The OBS executable name is retained for virtual camera compatibility.

## Stream accounts

1. In **Settings → Stream**, add Twitch, YouTube or Kick OAuth accounts; **Каталог OBS · RTMP-сервисы** opens the OBS service catalog (including less-common platforms) and their available ingest servers; **RTMP(S)** accepts an arbitrary server URL. There is no limit on the number of accounts, including accounts from one provider. The catalog is limited to services that work with RTMP or RTMPS: WHIP, HLS and other protocols need separate output implementations.
2. For a catalog service, select both the service and its server and enter the stream key. For a custom RTMP(S) destination, supply the URL and key. For the three integrated providers, supply credentials manually or use OAuth to obtain an ingest URL and key. Each destination has its own encoder, bitrate and output resolution. For native 1440p set both the base and output OBS resolution to 2560×1440.
3. **Every enabled XINKX destination is equal**: the OBS Start/Stop Streaming button starts/stops all configured accounts through their own `libobs` outputs, with no primary or dummy stream and no single-server dependency. Stock OBS Stream settings remain stored, but are used only when the XINKX list is empty. They can be inspected via the checkbox on the accounts panel. The old `primary` flag in Stage 5/6 profiles is ignored for compatibility.
4. Use **Apply** or **OK** to save edits. **Cancel** discards draft edits. OAuth actions require a saved account and persist their result immediately. A revoked grant cannot be restored by Cancel.
5. The account menu next to **Start Streaming** (**Ctrl+Alt+S**) changes which accounts participate without editing their saved defaults. During a session, each output can be stopped individually. The main OBS button stops all outputs. The OBS bottom status bar now shows the session timer, combined bitrate and combined dropped frames even without an active stock OBS output. **Выходы: N/M** shows the current number of transmitting accounts, the aggregate bitrate and per-output details in its tooltip. These are *local* OBS output measurements, not platform playback confirmation.

Each destination has its own `obs_service_t`, `obs_output_t` and encoders. Existing Stage 5/6 account records are compatible. Their ingest URL and key are kept out of the OBS profile's `service.json`; recording and virtual-camera settings still belong to OBS. Account records are read from `config/xinkx-studio/studio.ini`. The stream key, manually entered ingest URL, client secret and OAuth tokens are protected using Windows DPAPI under the current user account. Named catalog services and their public ingest server addresses are stored in plain text.

## Provider OAuth

Enter the credentials for your developer app in the account editor and apply the settings. Use **Подключить OAuth** to authorize in the default browser. The loopback callback is now `http://localhost:8765/api/oauth/{platform}/callback` (`platform` = twitch, youtube, kick). If the default browser has the wrong signed-in account, click **Скопировать ссылку OAuth** and open it in the right browser. **Отменить ожидание** releases the local listener without waiting three minutes. These callback URLs match the legacy web app *if registered exactly as shown*; the old Windows portproxy listener on 127.0.0.1:8765 must be removed or OAuth binding will fail. **Обновить данные** loads account identity and ingest information and refreshes an expired or rejected token when a refresh token is available. **Отключить OAuth** requests provider revocation and clears the local session and OAuth-derived ingest data after success. A manual RTMP key stays available.

- **Twitch** uses a developer app with the loopback redirect URI, Client ID and Client Secret. The authorization-code flow requests access to the channel's stream key. [Twitch authorization code flow](https://dev.twitch.tv/docs/authentication/getting-tokens-oauth/)
- **YouTube** uses a Google OAuth client of type **Desktop app** with YouTube Data API v3 enabled. Use a YouTube channel enabled for Live and add the Google account as a test user if the consent screen is in testing mode. If the API returns multiple ingestion streams, specify the desired stream ID in the account editor or supply RTMP parameters manually. Set up and bind the event in YouTube Studio. [Google desktop OAuth](https://developers.google.com/identity/protocols/oauth2/native-app), [YouTube liveStreams.list](https://developers.google.com/youtube/v3/live/docs/liveStreams/list)
- **Kick** uses a developer app with the loopback redirect URI, Client ID and Client Secret. The token needs `user:read`, `channel:read` and `streamkey:read` to retrieve the channel and ingest information. Use a manual key if access is unavailable. [Kick OAuth 2.1](https://docs.kick.com/getting-started/generating-tokens-oauth2-flow), [Kick scopes](https://docs.kick.com/scopes/scopes)

### Distribution and legacy credentials

- A public OAuth **Client ID** may ship with a desktop application, but a confidential **Client Secret** must not be hardcoded in a distributable EXE (DPAPI does not protect embedded constants). A turnkey release requires public-client flows such as PKCE/device authorization where each provider supports them, or a hosted OAuth broker that retains confidential secrets server-side. Until that work is done, the current Twitch/Kick flows still require developer credentials from each operator; Google uses a Desktop OAuth client. Do not copy legacy credentials into source or a Git patch.
- The native OAuth callback reuses the legacy callback path on port 8765. A single process must own this port. DonationAlerts is an **alerts integration**, not an RTMP broadcast destination: it should be integrated through the native account/alerts feature rather than added to the stream-output selector.

## Verification

**Stage 5 baseline (verified on Windows):** The Windows MSVC build and portable launch passed. Local RTMP testing covered a managed OBS primary and two extra H.264/AAC outputs at 2560×1440, 1920×1080 and 1280×720 at 60 fps, including NVENC. Recording remained active after the stream stopped. Both additional receivers continued receiving video after the primary connection was refused. The Qt smoke test covered two accounts from one provider, primary selection, Apply/Cancel, persistence, removal, DPAPI and expired/invalid OAuth handling against a local HTTP mock.

**Stage 6 history:** OBS RTMP/RTMPS service catalog and its ingest server selector were added for named destinations. Stage 7 supersedes the Stage 6 primary-output selection; every saved catalog destination now operates independently. Verify `windows-x64` build, add/reload/edit/save of named services, parallel outputs, and the returned service's actual protocol before a live platform test.

**Stage 7 changes (Windows verification still required):** Independent broadcast outputs replace the managed-primary model. The OBS frontend streaming-active flag and aggregate status-bar counters work without a native stock stream output. OAuth moves to localhost:8765 with exact provider paths, a copyable authorization link and cancellation. The XINKX source inspector gains a switch for OBS's existing native magnetic snapping plus **Копировать размещение / Вставить размещение**, which copies position, dimensions, rotation and crop between real OBS scene items through the system clipboard and OBS undo stack. It does not introduce a second scene implementation. Test start/stop and failure of one destination while others remain active, background recording, all output counters, hotkeys, OAuth cancellation/timeout/retry, chosen-browser callback, scene transform paste and undo. An MSVC build and live platform check are pending.

**Stage 8 changes (Windows verification still required):** Provider OAuth REST requests use OBS's existing libcurl HTTP/TLS stack in background workers instead of relying on the optional QtNetwork TLS backend. The settings panel now reports the actual transport error or OAuth HTTP error; the browser callback only confirms receipt of the authorization code, not successful token issuance. The scene source inspector is scrollable and adds native composition presets: four camera picture-in-picture corners and left/right half-canvas placement, with adjustable margins. These presets edit OBS scene items directly and retain the standard OBS undo stack. Check Twitch token exchange, refresh/revoke, multiple provider accounts, scene layout presets, all stream outputs and recording in the Windows build.

Use private test channels for the remaining provider sign-in, refresh, revocation and ingest checks. Multichat and YouTube event provisioning are separate work.
