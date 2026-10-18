# 05 — Settings

Windows stores settings in `HKCU\Software\NeatDM` (E6). SwiftDM stores the same
keys (same names, lowercase INI sections) in
`~/.config/swiftdm/swiftdm.ini`, e.g.:

```ini
[general]
MaxConnections = 8
ConnectionsAtOnce = 0          ; 0 = add connections all at once, 1 = one by one
BandWidthLimit = 0             ; KB/s, 0 = unlimited (default for new downloads)
UseUAgent = 0                  ; 1 = force DefaultAgent for browser-sent downloads
DefaultAgent = Mozilla/5.0 (...)
DownloadDirectory = ~/Downloads
CategoryFolders = 1
TempDirectory = ~/.local/share/swiftdm/tmp
ServerCreationDate = 0
CompletionDialog = 1
AppAutoStart = 0

[proxy]
SocksVersion = 0               ; 0 = none, 4, 5, or "http"
ProxyAddress =
ProxyPort = 0
ProxyUser =
ProxyPassword =

[browser]
ChromePanel = 1
FoxPanel = 1
EdgePanel = 1
```

## Key reference (from E8)

| Key | Type | Original default observed | Meaning |
|---|---|---|---|
| `MaxConnections` | DWORD | 8 ("8 recommended") | max connections per download |
| `ConnectionsAtOnce` | DWORD | 0 | 0 = create additional connections all at once; else one by one |
| `BandWidthLimit` | DWORD | 0 | default per-download limit (KB/s) |
| `UseUAgent` | DWORD | 2 | use configured UA for browser-sent downloads too |
| `DefaultAgent` | STRING | Chrome-like modern UA | default User-Agent |
| `DownloadDirectory` | STRING | `%USERPROFILE%\Downloads` | destination root |
| `CategoryFolders` | DWORD | 2 | create per-category subfolders |
| `TempDirectory` | STRING | `%APPDATA%\Roaming\NeatDM` | temp/state root |
| `ServerCreationDate` | DWORD | 2 | derive file creation date from server header |
| `CompletionDialog` | DWORD | 0 | show completion dialog |
| `AppAutoStart` | DWORD | 0 | start at login (SwiftDM: XDG autostart entry) |
| `LastDownloadID` | DWORD | — | last allocated download id (also DB rowid) |
| `SocksVersion` | DWORD | — | proxy type selector |
| `ChromePanel`/`FoxPanel`/`EdgePanel`/`IEUser` | DWORD | 1 | browser panel show flags (E9) |

Observed defaults for the tri-state checkboxes (`2`) map to "unchecked" in the
original UI; our INI normalizes to `0/1` booleans and records the original raw
values here for reference. Where the original semantic is ambiguous (E23) we
store booleans and document the mapping.

## Per-download overrides

`downloads.connections` and `downloads.bandwidthlimit` override the globals per
task (E4). Proxy/auth are global; saved credentials live in the `auths` table
and are matched per host/target (E4, E18 Passwords tab).

## Startup behavior

- Load settings, open DB, mark stale "downloading" rows as `Paused` (resume
  manually), start tray icon and main window.
- `AppAutoStart`: writes `~/.config/autostart/swiftdm.desktop` when enabled.
