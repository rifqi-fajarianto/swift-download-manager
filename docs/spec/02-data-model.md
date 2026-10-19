# 02 — Data model

## 1. SQLite database

File name: `NeatDB.db` (E5). Windows location is the app data folder
(`...\AppData\Roaming\NeatDM\`, E22); for SwiftDM the database lives in
`$XDG_DATA_HOME/swiftdm/` (default `~/.local/share/swiftdm/swiftdm.db`).

Schema as recovered from the binary (E4), preserved verbatim as our schema
because it is a functional data model:

```sql
CREATE TABLE "downloads" (
    "id"              INTEGER,
    "url"             TEXT,
    "method"          TEXT,
    "filename"        TEXT,
    "ltype"           TEXT,
    "filesize"        NUMERIC,
    "category"        TEXT,
    "status"          TEXT,
    "bandwidthlimit"  NUMERIC,
    "connections"     NUMERIC,
    "lasttry"         NUMERIC,
    "firsttry"        NUMERIC,
    "useragent"       TEXT,
    "resumable"       NUMERIC,
    "pageurl"         TEXT,
    "pagetitle"       TEXT,
    "hittitle"        TEXT,
    "mimetype"        TEXT,
    "errortext"       TEXT,
    "urla"            TEXT,
    "postdata"        TEXT,
    "folderpath"      TEXT,
    "temppath"        TEXT,
    "groupid"         TEXT,   -- adaptive pair token (video + audio handoff)
    "done"            NUMERIC, -- bytes already written (restore/resume progress)
    PRIMARY KEY("id")
);
CREATE TABLE headers (id NUMERIC, header TEXT);      -- extra request headers per download
CREATE TABLE auths (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    target TEXT, protocol TEXT, user TEXT, pass TEXT  -- saved credentials (passwords tab)
);
```

Migration note: the original appended `folderpath`/`temppath` columns with
`ALTER TABLE downloads ADD COLUMN ... DEFAULT ''` (observed migration string),
so schema migrations must be additive.

Field semantics:

| Column | Meaning |
|---|---|
| `url` | requested URL (post-redirect effective URL is stored here per properties dialog "URL" field being editable) |
| `method` | HTTP method (`GET`/`POST`; POST uses `postdata`) |
| `filename` | output file name (without folder) |
| `ltype` | download kind discriminator (values UNKNOWN; strings suggest at least normal / HLS / FTP variants) |
| `filesize` | total size in bytes when known |
| `category` | one of Video/Audio/Compressed/Document/Application/Misc (E19) |
| `status` | `Complete`, `Paused`, `Error`, and an active/downloading state (exact token set UNKNOWN) |
| `bandwidthlimit` | per-download limit in KB/s (0 = unlimited) |
| `connections` | requested connection (segment) count for this download |
| `lasttry`/`firsttry` | unix timestamps of last/first attempt |
| `useragent` | UA used for this download |
| `resumable` | 1 if server advertised `Accept-Ranges`/range support |
| `pageurl`/`pagetitle` | referrer page info from browser integration |
| `hittitle` | server-provided hint for the filename (e.g. Content-Disposition) |
| `mimetype` | MIME type |
| `errortext` | last error message for display |
| `urla` | original/alternate URL (used by resume when server file changed) |
| `postdata` | POST body |
| `folderpath` | destination directory for this download |
| `temppath` | temporary directory for this download |
| `groupid` | adaptive pair token shared by the video and audio halves of a browser handoff (empty otherwise) |
| `done` | bytes already written; added by SwiftDM 0.17.0 (additive `ALTER TABLE`) for progress restore. Older rows read as 0 and fall back to `segments.json`. |

## 2. On-disk download state (behavioral)

The SQLite `downloads` row is written by SwiftDM as downloads are added and
updated (`HistoryStore`), including `pageurl`/`pagetitle`, method, POST body, and
extra header rows. `MainWindow::restore_history()` reloads every row at startup
(newest first) and rebuilds the list: `Complete` stays complete, `Paused`/`Error`
stay inactive with a Resume action, and stale `Downloading`/`Queued` rows are
normalized to `Paused` (nothing starts automatically). Progress is checkpointed
every ~5 s and on exit; a paused row also reads `segments.json` so a hard kill
loses at most the last engine checkpoint. Rows deleted by the user are removed
from `downloads` and `headers`.

Completed rows store the actual saved basename, so a file saved as
`name (1).ext` is found again. If the file is gone, the row shows
`Complete (file missing)`, a startup summary lists it, and right-click Open File
explains that the file was deleted or moved (with Open File Location).

- Partial content lives under the *temp path* (`%APPDATA%\Roaming\NeatDM\` on
  Windows; SwiftDM: `~/.local/share/swiftdm/tmp/<id>/`).
- Segment table state persists in `segments.bin` next to the partial data,
  with `segments.bak` kept as a fallback (E14). On startup the engine reloads
  it and logs how many segments were restored; corrupt state falls back to a
  fresh start (backup exists for exactly this recovery).
- Per-segment temporary files use a `.seg.x` suffix (E14); at completion the
  parts are merged into the destination file (strings: "Merged To Segment",
  "Internal Error, Failed in Merging segments.").
- Final placement: file is moved/created in `folderpath`; with category
  folders enabled, a per-category subfolder is used
  (e.g. `~/Downloads/Video/`).

SwiftDM design decision (documented divergence): our state file is
`segments.json` (schema below) — the original's binary `segments.bin` layout is
not required for a clean-room implementation and stays UNKNOWN.

```json
{ "version": 1, "fileSize": 12345, "etag": "\"abc\"", "lastModified": "...",
  "segments": [ { "start": 0, "end": 999, "done": 999, "fileIndex": 0 } ] }
```

## 3. Logging

Per-download `Log File` buttons imply a per-download log; extension/name is
`.LogFile.txt` (static wide string). SwiftDM writes
`~/.local/share/swiftdm/logs/<id>-<filename>.log`.

## 4. Settings store (Linux divergence)

Windows keeps settings in the registry (E6–E8); SwiftDM uses an INI file
`~/.config/swiftdm/swiftdm.ini` with the same key names (see 05-settings.md).
