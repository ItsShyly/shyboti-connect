# ShyBoti Connect

OBS plugin that connects OBS to ShyBoti.

> ShyBoti is in closed beta. The plugin only works with a ShyBoti account, which is invite-only for now.

## How it works

- Talks to OBS through obs-websocket's in-process C API (`call_request`), so the
  websocket server in OBS can stay disabled.
- Dials out to `wss://shyboti.de/api/agent` with the pairing token
- Bitrate rules run locally and are cached in the plugin config folder.

## Requirements

- OBS Studio 31.1 or newer (Help > About shows your version)

## Install

Download the file for your system from the [latest release](../../releases/latest).

### Windows

1. Close OBS.
2. Unpack the zip into `C:\ProgramData\obs-studio\plugins\`
   (Folder may be hidden. Also create \plugins if it's not existing).
3. It should look like this:
   
 ```
C:\ProgramData\obs-studio\plugins\shyboti-obs\bin\64bit\shyboti-obs.dll
```

### macOS

Close OBS and run the `.pkg`.

### Linux

Close OBS and install the `.deb`:
```
sudo apt install ./shyboti-obs-*.deb
```

## Connect

1. On the ShyBoti dashboard, open **OBS** and generate a pairing token.
2. Start OBS and open **Tools > ShyBoti > Settings...**
3. Paste the token and press **Connect**.

The status turns green with `Connected as #yourchannel`.

## Build

Built by GitHub Actions on push (Windows, macOS, Ubuntu), set up from obs-plugintemplate.
Downloads land under the run's Artifacts.

Dependencies are fetched at configure time: nlohmann/json, IXWebSocket, mbedTLS (Windows).

## Known gaps

- No auto update yet. New versions are installed by hand from the releases.
