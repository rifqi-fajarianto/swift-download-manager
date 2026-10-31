# 01 — Product & UX specification

Derived from the Windows application (see `evidence-ledger.md`, E18/E19 for the
screenshots). This describes **what SwiftDM must do**, not how the original is
implemented.

## 1. Purpose

A desktop download manager for Linux that accelerates HTTP/HTTPS downloads by
splitting each file into multiple parallel connections, with pause/resume,
queueing, per-download speed limits, proxy support, and a download history.

## 2. Main window

Layout (from `main-window.png`):

- **Toolbar** (left to right): `New URL` | `Resume`, `Stop` | `Delete`,
  `Delete File` | (right) `Settings`, `Browsers`, `About`, `Quit`.
  (`Delete File` is a SwiftDM addition, documented below.) `Resume` is enabled
  while at least one selected row is paused, failed or queued; `Stop` is
  enabled while at least one selected row is downloading. A mixed selection
  enables each button that has a matching row (0.18.21). The vertical bars are
  toolbar dividers. When the `Queues` branch or a queue is selected, the
  toolbar switches to queue mode: `New Queue`, `Delete Queue`, `Queue
  Properties` | `Start Queue`, `Stop Queue` | `New URL`, `Delete`, `Delete
  File` (0.19.1). The queue actions that need a specific queue are disabled
  while only the `Queues` branch (overview) is selected. `Move to Downloads`
  is available only from the item context menu.
- **Left panel**: category tree.
  - `All Downloads` with children: `Video`, `Audio`, `Compressed`, `Document`,
    `Application`, `Misc`.
  - `Complete`, `Incomplete` (views, shown under All Downloads).
  - `Queues`, with one child per named queue and a `No queues` placeholder
    when none exist. The branch is selectable: it shows the queue overview in
    the right panel, while selecting a queue child filters the table to that
    workspace (0.19.1). `All Downloads`, the categories, `Complete` and
    `Incomplete` show only items outside queues (`queueid = 0`) (0.19.0).
- **Right panel**: the downloads table, or the queue overview when the
  `Queues` branch is selected. The overview lists the queues (name + item
  count); with no queues it shows a `New Queue` prompt. Clicking a queue opens
  its downloads (0.19.1). The downloads table has columns:
  `File Name`, `Size`, `Status`, `Bandwidth`, `Remaining Time`, `Last Try`.
  Status column shows e.g. `Complete`, `Paused ( 54% )`, `Error ( 0% )`.
- **Item context menu** (right-click on a row): `Open File`, `Open File
  Location`, `Properties`; it selects the row under the cursor first. The
  original's version of this menu also carries `Resume`, `Stop`, `Redownload`,
  `Show Window` and `Delete` (E25) — here those stay on the toolbar and in the
  per-download window.
- **Refresh download URL** (0.22.0): paused and failed rows that have a
  history id also show `Refresh Download URL`. Choosing it marks that single
  download as waiting, opens the page it came from in the browser (or, when no
  page URL was captured, asks the user to start the file again), and returns.
  A compact always-on-top `Refresh Download URL` window lists `File Name:` and
  `File Size:` and offers `Cancel` (closing it cancels too). It hides while the
  confirmation prompt is up and returns if the URL is rejected. While the wait
  lasts, **every** browser capture is routed to the `Confirm New Download URL`
  prompt with `File Name:` and `New URL:` — a mismatching file name paints the
  warning in red and defaults to `Cancel` — and a rejected capture never becomes
  a new download; the manual `New URL` action is blocked with a warning that
  offers `Cancel Refresh`. `Use New URL` moves the new URL, cookies, referer,
  user agent, and headers onto the same row and resumes from the bytes already
  on disk. One download waits at a time.
- **History restore**: at startup the table is rebuilt from SQLite with every
  row (Complete, Paused, Error). Rows left `Downloading`/`Queued` in the main
  list are shown as `Paused` and never resume by themselves; the user presses
  Resume. Rows in a queue return as `Queued`, and a queue with autostart starts
  them. Deleting a row removes it from the database permanently and deletes
  its temp workspace (the partial `.part` file and `segments.json`); see
  `02-data-model.md` §2.
- Window title: application name + version.

## 3. Core user flows

1. **New download (manual)**: `New URL` toolbar button opens a small dialog
   with a URL field and `Download`/`Cancel`. On accept, the app starts probing
   the URL and immediately shows the *Download Confirmation* window (no list
   row or database entry yet): full URL, server-resolved file name and size, a
   `Category` dropdown (file-type default, user overridable) and `Location`
   (category folder by default, user overridable). `Download Now` adds the
   item to the main Downloads list and starts the transfer; `Cancel` leaves no
   trace; `Add to queue` adds it to the chosen queue and starts it only when
   that queue is running (0.19.0). While probing, the fallback filename and
   `Checking…` size are shown and Download Now is disabled; server metadata
   replaces them when ready. The successful probe is reused by the initial
   transfer, while Resume probes again (0.18.19).
   - Fixed default size (650×330, minimum 560×320): the window never grows with
     the file name or the URL length (0.18.9). The file name line is elided in
     the middle and keeps the full name in its tooltip.
   - Rows are grouped into two bold-titled sections (0.18.22): `File` holds
     `Name` and `Size` (content length, `Unknown size` when the server reports
     none); `Save` holds `URL` (read-only field), `Category` and `Location`.
     The size is no longer inline with the file name (0.18.9). Every
     description is vertically centered with the widget next to it (0.18.9).
   - `Add to queue` asks for the target queue (0.19.2): with more than one
     queue it opens a `Choose Queue` picker that lists every queue with its
     item count, starts without a selection and enables its accept button only
     after a choice; `Cancel` returns to the confirmation window. A single
     queue is used directly, and a download started from a queue view targets
     that queue without a picker (`Add to queue` is the default button then).
     `Download Now` stays the default otherwise and targets Downloads. The
     confirmation window has no inline queue row. With the confirmation
     window off, `New URL` from a queue view targets that queue, while the
     main view and browser handoff target Downloads (0.19.0).
   - The buttons sit in one row: `Cancel` on the left, `Add to queue` and
     `Download Now` on the right; `Download Now` is the default button.
   - All inputs and buttons (URL field, location field, `Browse...`, category
     dropdown, `Download Now`, `Add to queue`, `Cancel`) share one height, so
     the rows look even in every style (0.18.9).
2. **Download progress window**: double-click opens the per-download window
   (`download-window.png`):
   - Fixed default size (650×500, minimum 520×420): the window never grows with
     the URL length. The URL line is elided in the middle and keeps the full
     URL in its tooltip and link.
   - Tabs: `Download`, `Options`, `Connections`. Each tab groups its fields in
     bold-titled sections, like the Settings and Download Properties windows
     (0.18.22).
   - Download tab: URL; `Details` section with Status, File size, Downloaded
     (bytes + %), Bandwidth, Remaining time and Resumable (Yes/No); `Progress`
     section with the main progress bar (percentage drawn inside), segment
     progress bar (segments shown as colored slices) and the segment count
     (`Segments: N`); `Resume`/`Cancel` buttons aligned to the right. When the
     per-download speed limiter is on, an orange `Speed limit: <value>` line
     sits next to the Bandwidth value (0.18.1).
   - Options tab: `Destination` section with Save to and File name; `Transfer`
     section with Connections, Bandwidth limit (enable toggle, preset selector
     and custom KB/s value) and Referer, plus a note explaining that speed-limit
     changes apply while downloading and other changes on resume; `Apply` at
     the bottom right (0.18.1). Presets are 500 KB/s, 1 MB/s, 5 MB/s, 10 MB/s,
     20 MB/s, 50 MB/s, or a custom KB/s value. While a transfer is active, only
     the speed-limit editor remains enabled and its changes apply after 500 ms
     without pausing. Other fields are disabled until the transfer is paused;
     `Apply` saves them then (0.18.2).
   - Connections tab: the segment table sits inside a single frame, with columns
     `#`, `Range (bytes)`, `Downloaded` and `%`; the table itself does not draw
     grid lines or a second border (0.18.22).
3. **Item actions** (right-click a download row):
   - `Open File` — opens the downloaded file with the default handler. It is
     available for every completed row; when the file was deleted or moved it
     shows a warning ("The file has been deleted or moved") with `Open File
     Location` instead of failing silently. A completed row whose file is
     missing is labelled `Complete (file missing)` in amber with a tooltip; no
     dialog appears at startup, only when `Open File` is chosen for that row.
   - `Open File Location` — opens the containing folder in the file manager.
   - `Properties` — read-only *Download Properties* window (see below).
4. **Delete File** (toolbar): deletes the downloaded file of the selected
   completed download(s) from disk after a warning confirmation ("This cannot
   be undone"); the list entries are removed as well. SwiftDM addition, not in
   the original toolbar.
5. **Pause/Resume**: toolbar or download window; paused state keeps partial
   data and can resume after restart.
6. **Completed**: `download-complete-window.png` with `Open Folder`, `Open`,
   `Close`. Shown only when the completion-dialog setting is on.
7. **Error**: `download-error-window.png` shows `Error : <message>`, the URL, and
   offers `Close` and `Log File`. The message carries the host's own explanation
   when the response body has one (JSON `message`/`value`, HTML title or text),
   e.g. `Error : HTTP/1.1 403 Forbidden - hotlink_detected: Hotlinking was
   detected for this download, hotlinking is only allowed on premium accounts`.
   When the download came from a browser page, an additional `Open Page in
   Browser` button opens that page (hosts such as pixeldrain require a CAPTCHA
   there before any tool may download the file).
   Google Drive links are resolved before the confirmation step (0.13.0):
   viewer/file links are rewritten to Drive's direct download URL and the
   virus-scan notice for files above Google's scan limit is confirmed
   automatically, so the dialog shows the real file name and size. Links that
   never lead to a file — Drive folder listings, Docs editor pages, sign-in
   walls for private or removed files — fail with an explanation instead of
   saving a page.
8. **Properties** (`download-properties-window.png`): icon, file size bytes,
   status, Saved To path (+ Open Folder), Temp Path, URL (editable), Page URL
   (+ Show Page), Page Title, Browser (source that submitted it), Added On,
   Last Try timestamps, `Log File` button.
   *Implemented (0.18.22)*: the read-only window groups the values into four
   labeled sections, in this order: `General` (file size in bytes plus the
   formatted size, status in its status color, category, downloaded amount,
   resumable, segment count), `Location` (Saved To path with `Open Folder`,
   Temp Path, Log File), `Source` (read-only URL; Page URL, Page title and Pair
   only when the item carries them) and `Time` (Added On, Last Try). The header
   keeps the properties icon and file name; a long name is elided in the middle
   and keeps the full name in its tooltip. Section titles are bold, labels use
   sentence case and left alignment. The dialog opens at 650×690 and its
   minimum size follows the sections; optional source rows make it taller.
   Since 0.20.0 the `Log file` row shows the per-download log path with an
   `Open Log` button (disabled until the engine has written one). Editable URL
   and the Browser source remain future work.
9. **Settings** (`options-window.png`), tabs:
   Every tab groups its options into labeled sections (0.18.20): `General`
   (Downloads, Startup, System tray), `Interface` (Toolbar, Appearance, System
   tray), `Download` (Connections, Bandwidth, User-Agent, Files and folders),
   `Proxy/Socks` (Proxy type, Server, Authentication); the `Passwords` tab is
   planned but not shown until the `auths` store exists (removed from the
   dialog in 0.20.0). Queue settings are not in this window; each queue has its
   own `Queue Properties` dialog (0.19.0).
   - *General*: app-wide behavior options — Show Download Confirmation Dialog
     (with a note that all files are downloaded automatically without
     confirmation while it is off, 0.18.17); Always Show the Confirmation
     Dialog on Top (0.21.4); Show Download Completion Dialog; Always Show the
     Completion Dialog on Top (0.21.4);
     Start Application on System Startup; Show System Tray Icon; Minimize to
     Tray When Closing the Window; Open the Download Folder When a Download
     Completes (0.18.10 split the tabs: the download options moved to the
     `Download` tab). The completion dialog and the open-folder option apply
     only to downloads in the main Downloads list; queue items follow their
     queue notification setting (0.19.3).
   - *Interface*: appearance options — Toolbar Size (`Normal` / `Small`);
     Show Toolbar Text; App Theme (`Light` / `Dark` / `Follow System`); Tray
     Icon Style (`Simple` / `Colorful`). The changes apply immediately after
     the dialog is confirmed (0.18.12).
   - *Download*: Max Connections per Download (default 8, "8 Connections
     Recommended"); `Create additional connections:` policy (`All at once` /
     `One by one`); Bandwidth
     Limit per Download with an enable toggle and presets (500 KB/s, 1 MB/s,
     5 MB/s, 10 MB/s, 20 MB/s, 50 MB/s, or custom KB/s; value editable while
     the toggle is off — 0.18.1); Default User-Agent (+Reset,
     + "use for both manual and browser-sent downloads"); Download Directory
     (+ category folders toggle); Temporary Directory; Get File Creation Date
     From Server (since 0.20.0 the finished file gets the server's
     `Creation-Date`/`Last-Modified`, or the FTP MDTM time, as its modification
     time). Since 0.21.0 a `Category folders` section picks a category from a
     combo and shows one folder field below it (with `...` picker and `Reset`);
     an empty field shows the default location as placeholder. A filled field
     sends that category straight into the folder (no category subfolder) and
     applies even when the category folders toggle is off.
   - *Proxy/Socks*: No Proxy/Socks | HTTP Proxy | Socks Proxy (V4/V5),
     Address/Port/User/Password, "https protocol" handling note.
   - *Passwords*: credential table (Target/User/Password; remembered auth per
     server), matching the `auths` DB table. Planned; the tab is hidden until
     the store exists (0.20.0).
10. **Browser integration**: `Browsers` toolbar button shows supported browsers
    (Chrome, Firefox, Edge, Opera) and instructions; the app runs a local
    WebSocket server accepting download requests from browser extensions.
11. **Tray behavior**: closing the main window hides to tray; tray menu can
    restore the window (per strings "Quit NeatDownloadManager", "You can hide
    ... in Notification Area..."). The tray shows the colored SwiftDM mark
    (same artwork as the browser integration), so it stays recognizable on
    both light and dark panels; the `Simple` tray style uses the monochrome
    mark instead (0.18.12). When started at login through the autostart
    entry (`swiftdm --autostart`), the main window stays hidden and only the
    tray icon appears; without a system tray the window is shown as a fallback
    (0.18.7). Toolbar and tray-menu `Quit` actions ask
    for confirmation before fully exiting; active downloads are paused and can
    be resumed on the next launch, while Cancel keeps the app running (0.18.4).
    The `General` tab can switch the tray icon off and can turn the
    close-to-tray behavior off, in which case closing the main window ends the
    app after the same confirmation (0.18.13).
12. **About**: version, license tier, support contact, website; update check
    button (we replace this with "no update check").
13. **Message-box icons**: the message boxes of the app show the app's own
    filled glyphs instead of the built-in platform icons (0.18.16): the
    information glyph for the Browser Extensions window, the question glyph for
    the quit confirmation, the check glyph for the Download Complete window and
    the alert glyph for every warning (Delete File, Download Error, a missing
    file or folder, and the warnings of the Settings and Download Confirmation
    windows). The glyphs are tinted with the palette of the app — blue for
    information and question, red for warnings, green for the completion notice
    — and only the icon area of the dialog changes; the title bar keeps the
    application icon.
14. **Queues (0.19.0)**: a queue is a named workspace for related downloads,
    not a FIFO status on the main list.
    - Create a queue with `New Queue` in the toolbar or the sidebar context
      menu, then rename it in `Queue Properties`.
    - Selecting the `Queues` branch shows the queue overview (0.19.1). The
      overview lists the queues with their item counts; with no queues it
      shows a `New Queue` prompt. Selecting a queue from the list opens its
      downloads.
    - `Queue Properties` sets the simultaneous-download limit (0 = `Unlimited`,
      maximum 64), the connection count per item (0 = `Default`, meaning the
      global setting), a total speed limit shared by the active items,
      autostart, per-item and all-complete notifications, and an optional
      shutdown with a cancellable delay (5–3600 s, default 60 s). The values
      are stored in the `queues` table, not in the INI file.
    - `Start Queue` resumes `Queued`, `Paused` and `Error` items in insertion
      order (`history_id` ascending). The automatic scheduler starts only
      `Queued` items and stops starting new ones once the simultaneous limit is
      reached. A manual start from the context menu or the progress window also
      honors the limit. Lowering the limit pauses the newest active items; those
      rows return to `Queued` when the pause finishes, while a manual pause
      stays `Paused`. The toolbar exposes the pause operation as `Stop Queue`
      (0.19.1).
    - A queue speed limit is divided across its active items
      (`total / active`, at least 1 KB/s), and queue items ignore their
      per-item speed limit while they belong to the queue. A queue connection
      count overrides the per-item value; the stored per-item values are kept
      for a later move back to Downloads.
    - Inactive items move between Downloads and a queue from the item context
      menu (`Move to Queue` / `Move to Downloads`). Completed items can move
      too and stay visible in the queue view. Moving an item does not change
      its file or workspace. `Move to Downloads` is not a toolbar action
      (0.19.1).
    - Deleting a queue asks for confirmation, deletes its rows and temporary
      workspaces, and leaves completed files on disk. Active items are
      cancelled as part of the deletion, and the view returns to the queue
      overview (0.19.1).
    - Completion actions are queue-owned (0.19.3): a finished queue item never
      opens the global `Download Complete` window and never triggers the
      global open-folder option. `Notify when an item completes` is the only
      per-item notice; the global completion options apply to the main
      Downloads list only.
    - When every item in a queue is complete, the queue can notify once (tray
      message, status-bar fallback) and, if enabled, show a cancellable
      shutdown countdown. Only one countdown is active at a time; if another
      queue also completes, the pending countdown stays. At the deadline the
      app pauses every active download and queue, persists their state, and
      runs `systemctl poweroff` (falling back to `shutdown -h now`). Changing
      queue settings on an already complete queue does not re-trigger the
      notification or countdown.

## 4. Non-goals for v1 (see 06-parity-checklist.md)

- Browser extension + WebSocket server: v2.
- FTP, HLS/m3u8 + MKV muxing: v3.
- Any phone-home/update-check behavior: dropped intentionally.

## 5. Open questions (UNKNOWN, from E23)

- Exact status string set and % rounding in the table.
- Sort order and refresh cadence of the table.
- Tray icon menu item set.
