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
| `evidence-ledger.md` | Evidence IDs and raw observations backing every claim |

## Status

Milestone 1 in progress: static evidence collected (strings, PE metadata,
SQLite schema, UI screenshots). Deep RE (Ghidra via REA) pending toolchain
setup.
