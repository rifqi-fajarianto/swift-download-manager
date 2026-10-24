# Clean-room specification

This directory holds the behavioral specification for SwiftDM, written from
reverse-engineering evidence before any implementation code. Rules:

1. Every claim about the original application cites its evidence in
   `evidence-ledger.md` (REA Evidence ID, binary offset, or screenshot).
2. The spec is written in our own words and structure. No pseudocode is
   copied verbatim from decompiler output; algorithms are re-described.
3. The implementation under `src/` is written from this spec only.
4. Unknowns are marked `UNKNOWN` rather than guessed; they are resolved by
   further RE or by an explicit design decision recorded here.

## Planned documents

| File | Contents |
|---|---|
| `01-product.md` | Feature inventory, UX flows, UI map (from screenshots + code) |
| `02-data-model.md` | SQLite schema, download state files, temp/destination layout |
| `03-engine.md` | Segmented download algorithm, resume, retry, bandwidth limits |
| `04-protocols.md` | HTTP behaviors, proxy/SOCKS, auth, extension WebSocket protocol |
| `05-settings.md` | Settings keys, defaults, config file layout on Linux |
| `06-parity-checklist.md` | What is replicated vs intentionally dropped vs deferred |
| `07-chromium-extension-reference.md` | Original Chromium-extension behavior, protocol, differences, candidate work items |
| `evidence-ledger.md` | Evidence IDs and raw observations backing every claim |

## Status

Everything through milestone 4 is implemented: engine core (`src/engine`,
`src/net`, `src/store`) with segmented downloads, resume state, bandwidth
limits, retries, category routing and INI settings; headless CLI
(`swiftdm get`); the Qt6 Widgets GUI (`src/ui`) with main window, per-download
worker tasks, download window (segment bar + connections table), settings
dialog, tray icon and completion/error dialogs; CMake install rules and a
`.deb` package. The download list is restored from SQLite at startup (0.17.0),
and every transfer — manual or browser-sent — is confirmed before it starts,
with category and location overrides (0.18.0). Named queues with independent
scheduling, limits, notifications and optional shutdown are implemented in
0.19.0; the queue overview and queue-mode toolbar arrived in 0.19.1, the
confirmation window asks for the target queue when several exist (0.19.2), and
queue items keep the global completion dialog and open-folder actions away
from their own completion path (0.19.3).
Verified by CLI, integration tests and headless UI tests (see
CHANGELOG.md). Remaining gaps (per-download log file and editable URL,
passwords UI, Firefox extension) are tracked in `06-parity-checklist.md`.
