# ShyBoti Connect

OBS plugin that connects OBS to ShyBoti.

## How it works

- Talks to OBS through obs-websocket's in-process C API (`call_request`), so the
  websocket server in OBS can stay disabled.
- Dials out to `wss://shyboti.de/api/agent` with the pairing token
- Bitrate rules run locally and are cached in the plugin config folder.

## Build

Built by GitHub Actions on push (Windows, macOS, Ubuntu), set up from obs-plugintemplate.
Downloads land under the run's Artifacts.

Dependencies are fetched at configure time: nlohmann/json, IXWebSocket, mbedTLS (Windows).

## Known gaps

- No in-place auto update. `CheckForUpdate` answers `updated: false`.
- `AgentQuit` only disconnects, it never closes OBS.
