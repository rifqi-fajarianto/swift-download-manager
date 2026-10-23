# 05 — Settings

Windows stores settings in `HKCU\Software\NeatDM` (E6). SwiftDM stores the same
keys (same names, lowercase INI sections) in
`~/.config/swiftdm/swiftdm.ini`, e.g.:

```ini
[general]
CompletionDialog = 1
AppAutoStart = 0
ShowTrayIcon = 1
CloseToTray = 1
OpenFolderAfterCompletion = 0

[download]
MaxConnections = 8
ConnectionsAtOnce = 0          ; 0 = add connections all at once, 1 = one by one
BandWidthLimit = 1024          ; KB/s value used when the limiter is enabled
BandWidthLimitEnabled = 0      ; 1 = speed limiter on, 0 = unlimited (value kept)
UseUAgent = 0                  ; 1 = force DefaultAgent for browser-sent downloads
DefaultAgent = Mozilla/5.0 (...)
DownloadDirectory = ~/Downloads
CategoryFolders = 1
TempDirectory = ~/.local/share/swiftdm/tmp
ServerCreationDate = 0

[interface]
Theme = dark                   ; system | light | dark
ToolbarSize = normal           ; normal | small
ShowToolbarText = 1
TrayIconStyle = colorful       ; colorful | simple

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

Each INI section matches one tab of the settings window: `general` (General),
`download` (Download), `interface` (Interface), `proxy` (Proxy/Socks). The
`browser` section is reserved for the browser panel flags.

## Key reference (from E8)

| Key | Section | Type | Original default observed | Meaning |
|---|---|---|---|---|
| `MaxConnections` | download | DWORD | 8 ("8 recommended") | max connections per download |
| `ConnectionsAtOnce` | download | DWORD | 0 | 0 = create additional connections all at once; else one by one |
| `BandWidthLimit` | download | DWORD | 1024 | per-download limit value in KB/s (kept while the toggle is off) |
| `BandWidthLimitEnabled` | download | DWORD | 0 | speed-limiter toggle: 1 = apply `BandWidthLimit`, 0 = unlimited |
| `UseUAgent` | download | DWORD | 2 | use configured UA for browser-sent downloads too |
| `DefaultAgent` | download | STRING | Chrome-like modern UA | default User-Agent |
| `DownloadDirectory` | download | STRING | `%USERPROFILE%\Downloads` | destination root |
| `CategoryFolders` | download | DWORD | 2 | create per-category subfolders |
| `TempDirectory` | download | STRING | `%APPDATA%\Roaming\NeatDM` | temp/state root |
| `ServerCreationDate` | download | DWORD | 2 | derive file creation date from server header |
| `CompletionDialog` | general | DWORD | 0 | show completion dialog |
| `AppAutoStart` | general | DWORD | 0 | start at login (SwiftDM: XDG autostart entry) |
| `ShowTrayIcon` | general | BOOL | 1 (SwiftDM) | show the system tray icon |
| `CloseToTray` | general | BOOL | 1 (SwiftDM) | hide to the tray when the main window is closed |
| `OpenFolderAfterCompletion` | general | BOOL | 0 (SwiftDM) | open the target folder after a download completes |
| `ShowDownloadConfirmationDialog` | general | BOOL | 1 (SwiftDM) | ask for confirmation before every transfer starts; off starts the download right away |
| `Theme` | interface | STRING | dark (SwiftDM) | app theme: `system`, `light`, `dark` |
| `ToolbarSize` | interface | STRING | normal (SwiftDM) | toolbar size: `normal`, `small` |
| `ShowToolbarText` | interface | BOOL | 1 (SwiftDM) | show the text under the toolbar icons |
| `TrayIconStyle` | interface | STRING | colorful (SwiftDM) | tray icon: `colorful`, `simple` |
| `LastDownloadID` | — | DWORD | — | last allocated download id (also DB rowid) |
| `SocksVersion` | proxy | DWORD | — | proxy type selector |
| `ChromePanel`/`FoxPanel`/`EdgePanel`/`IEUser` | browser | DWORD | 1 | browser panel show flags (E9) |

Observed defaults for the tri-state checkboxes (`2`) map to "unchecked" in the
original UI; our INI normalizes to `0/1` booleans and records the original raw
values here for reference. Where the original semantic is ambiguous (E23) we
store booleans and document the mapping.

## Compatibility with older configs (0.18.11)

Configs written before the tabs were split keep the download options in
`[general]`. The loader reads the file section by section and applies
`[general]` first, then `[download]`, so an old config still loads and a
`[download]` section overrides the legacy values regardless of the order of
the sections in the file. Unknown keys and unknown sections are ignored.

Unknown interface values (for example `Theme = neon`) keep the default instead
of being stored, so a typo cannot break the appearance of the app.

## Per-download overrides

`downloads.connections` and `downloads.bandwidthlimit` override the globals per
task (E4). The per-download row also stores the limiter toggle
(`bandwidthlimit_enabled`): off means unlimited for that task while the value
is kept for a later resume. Proxy/auth are global; saved credentials live in
the `auths` table and are matched per host/target (E4, E18 Passwords tab).

A download inside a queue is an exception while it stays there: the queue's
`connections` and shared speed budget control the transfer, and the stored
per-item values are ignored. Moving the item back to Downloads makes the stored
per-item values effective again. The Download window disables both per-item
editors for a queue item and shows a note naming the queue (0.19.0).

## Queue settings (0.19.0)

Queue settings are not INI keys. Each queue is a row in the `queues` table and
is edited through `Queue Properties`; the dialog writes the record through
`HistoryStore::update_queue()`.

| Field | Range / values | Default | Meaning |
|---|---|---|---|
| `name` | non-empty text | `Queue N` | sidebar label |
| `max_simultaneous` | 0–64 | 1 | active items allowed at once; 0 = `Unlimited` |
| `connections` | 0–64 | 0 | connection count per item; 0 = global `MaxConnections` |
| `bandwidthlimit_enabled` | 0/1 | 0 | queue-wide speed limiter |
| `bandwidthlimit` | KB/s | 1024 | total queue budget, kept while the toggle is off |
| `autostart` | 0/1 | 0 | start the queue after the history is restored |
| `notify_item` | 0/1 | 0 | notify when an item completes |
| `notify_complete` | 0/1 | 0 | notify once when all items complete |
| `shutdown_when_complete` | 0/1 | 0 | arm the shutdown countdown on completion |
| `shutdown_delay` | 5–3600 s | 60 | cancellable countdown before poweroff |

`Queue Properties` also displays the current item count. Renaming a queue, the
simultaneous limit and the speed budget apply immediately; a connection-count
change applies to the next transfer started from the queue (a running transfer
keeps its segments), and autostart applies on the next launch. Completion
notifications and shutdown are triggered by a completion event, not by saving
the dialog, so enabling them on an already complete queue does not run them
retroactively.

## Startup behavior

- Load settings, open DB, restore the history, mark stale "downloading" rows in
  the main list as `Paused` (resume manually) and stale queue rows as `Queued`,
  then start queues whose `autostart` is set, followed by the tray icon and
  main window.
- With `--autostart` (used by the login entry), the main window stays hidden
  and only the tray icon appears. If no system tray is available, the window is
  shown as a fallback so the application remains reachable.
- `AppAutoStart`: writes `~/.config/autostart/swiftdm.desktop` when enabled.
  The entry runs `swiftdm --autostart`; the GUI refreshes an existing entry on
  launch so entries written by older releases also start silently.
- `Theme`: the palette is applied before the windows are created, so a
  "follow system" theme still reads the desktop preference. The app follows
  the desktop while it runs when the color scheme changes.
- `ShowTrayIcon`: with the tray icon switched off there is nothing to hide
  into, so the main window is shown at startup and closing it ends the app.
- `CloseToTray`: when switched off, closing the main window ends the app after
  the same confirmation as the Quit action; the confirmation can be cancelled,
  which keeps the window open.
- `ShowDownloadConfirmationDialog`: while it is on, every transfer (manual and
  browser-sent) shows the Download Confirmation window. With it off, the
  confirmation is skipped and the transfer starts right away with the category
  from the file extension and the default folder: the download root, plus the
  category folder when the category folders are enabled. The progress window
  still opens. A configuration file without the key keeps the confirmation on.

## Changing the settings at runtime

The appearance settings take effect as soon as the settings dialog is
confirmed: the theme is applied to the whole application, the toolbar is
resized and the tray icon is replaced. The tray icon can also be created or
removed while the app runs (`ShowTrayIcon`).

Queue properties are applied immediately after their dialog is confirmed: a
new simultaneous limit can pause or start items, a new speed limit is split
across the active items, and a renamed queue updates the sidebar and any open
Download window. Disabling `shutdown_when_complete` cancels a pending
countdown. The per-queue values are persisted in SQLite, so they survive a
restart.
