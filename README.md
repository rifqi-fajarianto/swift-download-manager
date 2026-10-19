# SwiftDM

Clean-room Linux reimplementation (C++20 / Qt6) of a segmented, resumable
download manager, derived from behavioral analysis of the original Windows
application. No code, assets, icons, or branding are taken from the original;
behavior is documented in `docs/spec/` first and implemented from that spec.

## Status

- [x] Milestone 0 — project skeleton, versioning policy, RE toolchain
- [x] Milestone 1 — behavioral spec (`docs/spec/`) — first pass complete
- [x] Milestone 2 — core engine: segmented HTTP/HTTPS, resume, limits, CLI
- [x] Milestone 3 — Qt6 GUI (first increment; v1.1 gaps tracked in
      `docs/spec/06-parity-checklist.md`)
- [x] Chromium integration — development preview (`ext/chromium/`)
- [ ] Milestone 4 — verification & packaging

## Versioning

Single source of truth: `project(... VERSION ...)` in `CMakeLists.txt`.
Feature/UI/UX changes bump MINOR (+0.1.0); small fixes bump PATCH (+0.0.1).
See `AGENTS.md`.

## Building (planned dependencies)

```
sudo apt install cmake ninja-build qt6-base-dev libsqlite3-dev libcurl4-openssl-dev libssl-dev
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build

untuk menjalankan aplikasi

./build/swiftdm
```

## Testing policy

Automated testing is CLI/backend only (unit + integration tests, headless
engine runs). GUI verification is performed manually by the user with a
checklist provided per milestone — see `AGENTS.md`.

## Layout

```
cmake/            version.h.in and CMake helpers
docs/spec/        clean-room behavioral specification + evidence ledger
src/app/          entry points (GUI default, headless `get` CLI)
src/core/         shared utilities
src/engine/       download engine (segmentation, resume, categories)
src/net/          HTTP/curl layer (probe, segmented transfer, streaming)
src/store/        settings (INI); SQLite history planned for v1.1
src/ui/           Qt6 Widgets GUI (main window, download window, dialogs)
ext/chromium/     Chromium Manifest V3 development extension
tests/            CLI/backend tests (+ headless icon rendering test)
binary-original/  analysis-only reference (read-only, not distributed)
ui-assets/        user-provided icons (toolbar SVGs; app-icon/ = app, tray, extension)
tools/            helper executables (extension icon rasterizer)
```
