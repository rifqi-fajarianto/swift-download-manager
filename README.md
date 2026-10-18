# SwiftDM

Clean-room Linux reimplementation (C++20 / Qt6) of a segmented, resumable
download manager, derived from behavioral analysis of the original Windows
application. No code, assets, icons, or branding are taken from the original;
behavior is documented in `docs/spec/` first and implemented from that spec.

## Status

- [x] Milestone 0 — project skeleton, versioning policy, RE toolchain
- [ ] Milestone 1 — behavioral spec (`docs/spec/`)
- [ ] Milestone 2 — core engine (segmented HTTP/HTTPS, resume, queue, limits)
- [ ] Milestone 3 — Qt6 GUI
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
```

## Testing policy

Automated testing is CLI/backend only (unit + integration tests, headless
engine runs). GUI verification is performed manually by the user with a
checklist provided per milestone — see `AGENTS.md`.

## Layout

```
cmake/            version.h.in and CMake helpers
docs/spec/        clean-room behavioral specification + evidence ledger
src/app/          entry points (CLI, later GUI)
src/core/         shared utilities
src/engine/       download engine (later)
src/net/          HTTP/curl/proxy layer (later)
src/store/        SQLite persistence + settings (later)
src/ui/           Qt6 Widgets GUI (later)
tests/            CLI/backend tests
binary-original/  analysis-only reference (read-only, not distributed)
```
