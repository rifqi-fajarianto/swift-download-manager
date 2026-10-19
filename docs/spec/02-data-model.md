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

## 2. On-disk download state (behavioral)

The SQLite `downloads` row is now written by SwiftDM as downloads are added and
updated (`HistoryStore`), including `pageurl`/`pagetitle`, method, POST body, and
extra header rows. Restoring the list into the GUI at startup is still planned.

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
