# XINKX Studio — Stage 6 (0.15-dev)

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
3. Select an account and click **Сделать основным** to use it with the native OBS output. **Вернуть службу OBS** restores the OBS profile's stream service. Other enabled accounts run as independent extra libobs outputs. With a managed primary selected, the saved stock OBS settings are collapsed, not deleted; use **Показать сохранённые настройки OBS** to inspect them. The main OBS output is a real stream, not a dummy local relay.
4. Use **Apply** or **OK** to save edits. **Cancel** discards draft edits. OAuth actions require a saved account and persist their result immediately. A revoked grant cannot be restored by Cancel.
5. The account menu next to **Start Streaming** (**Ctrl+Alt+S**) changes which accounts participate without editing their saved defaults. During a session it lists additional outputs and lets you stop one individually. The main OBS button stops the session. The status bar shows totals and its additional-stream counter has a per-output tooltip.

The selected primary service and video encoder are attached only to the native stream output. The account's RTMP URL and key are kept out of the OBS profile's `service.json`. Recording continues to use the OBS profile's recording settings. Existing Stage 5 account records are compatible; named OBS services add `serviceName` and `serviceServer` fields. Account records are read from `config/xinkx-studio/studio.ini`. The stream key, manually entered ingest URL, client secret and OAuth tokens are protected using Windows DPAPI under the current user account. Named catalog services and their public ingest server addresses are stored in plain text.

## Provider OAuth

Enter the credentials for your developer app in the account editor and apply the settings. Use **Подключить OAuth** to authorize in the system browser. The loopback callback is `http://127.0.0.1:45678/`. **Обновить данные** loads account identity and ingest information and refreshes an expired or rejected token when a refresh token is available. **Отключить OAuth** requests provider revocation and clears the local session and OAuth-derived ingest data after success. A manual RTMP key stays available.

- **Twitch** uses a developer app with the loopback redirect URI, Client ID and Client Secret. The authorization-code flow requests access to the channel's stream key. [Twitch authorization code flow](https://dev.twitch.tv/docs/authentication/getting-tokens-oauth/)
- **YouTube** uses a Google OAuth client of type **Desktop app** with YouTube Data API v3 enabled. Use a YouTube channel enabled for Live and add the Google account as a test user if the consent screen is in testing mode. If the API returns multiple ingestion streams, specify the desired stream ID in the account editor or supply RTMP parameters manually. Set up and bind the event in YouTube Studio. [Google desktop OAuth](https://developers.google.com/identity/protocols/oauth2/native-app), [YouTube liveStreams.list](https://developers.google.com/youtube/v3/live/docs/liveStreams/list)
- **Kick** uses a developer app with the loopback redirect URI, Client ID and Client Secret. The token needs `user:read`, `channel:read` and `streamkey:read` to retrieve the channel and ingest information. Use a manual key if access is unavailable. [Kick OAuth 2.1](https://docs.kick.com/getting-started/generating-tokens-oauth2-flow), [Kick scopes](https://docs.kick.com/scopes/scopes)

### Distribution and legacy credentials

- A public OAuth **Client ID** may ship with a desktop application, but a confidential **Client Secret** must not be hardcoded in a distributable EXE (DPAPI does not protect embedded constants). A turnkey release requires public-client flows such as PKCE/device authorization where each provider supports them, or a hosted OAuth broker that retains confidential secrets server-side. Until that work is done, the current Twitch/Kick flows still require developer credentials from each operator; Google uses a Desktop OAuth client. Do not copy legacy credentials into source or a Git patch.
- The legacy web application uses `localhost:8765/api/oauth/{provider}/callback` and the native application currently uses `http://127.0.0.1:45678/`. These are distinct redirect URIs; native OAuth clients must have the native URI registered where required. DonationAlerts is an **alerts integration**, not an RTMP broadcast destination: it should be integrated through the native account/alerts feature rather than added to the stream-output selector.

## Verification

**Stage 5 baseline (verified on Windows):** The Windows MSVC build and portable launch passed. Local RTMP testing covered a managed OBS primary and two extra H.264/AAC outputs at 2560×1440, 1920×1080 and 1280×720 at 60 fps, including NVENC. Recording remained active after the stream stopped. Both additional receivers continued receiving video after the primary connection was refused. The Qt smoke test covered two accounts from one provider, primary selection, Apply/Cancel, persistence, removal, DPAPI and expired/invalid OAuth handling against a local HTTP mock.

**Stage 6 changes (Windows verification still required):** OBS RTMP/RTMPS service catalog and its ingest server selector are now available as additional and primary output destinations, preserving existing accounts. Native OBS settings are explicitly inspectable even when collapsed behind a managed primary. Verify `windows-x64` build, add/reload/edit/save of named services, primary handoff, parallel output, and the returned service's actual protocol before a live platform test. Linux builds and live broadcasts have not been tested for Stage 6.

Use private test channels for the remaining provider sign-in, refresh, revocation and ingest checks. Multichat and YouTube event provisioning are separate work.
