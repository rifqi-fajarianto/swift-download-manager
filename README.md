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
- [x] Milestone 4 — packaging: CMake install rules + `.deb` (CPack)
- [x] 0.17.0 — restore the download history at startup
- [x] 0.18.0 — confirm every download before it starts (category/location,
      Download Now / Cancel)
- [x] 0.18.1 — speed limiter toggle, presets and active-limit display
- [x] 0.18.2 — change a download's speed limit while it is running
- [x] 0.18.3 — keep the main window hidden during browser download confirmation
- [x] 0.18.4 — confirm before fully quitting the application
- [x] 0.18.5 — preserve/encode URL paths with spaces for browser and manual downloads
- [x] 0.18.6 — show missing files in the list without a startup dialog
- [x] 0.18.7 — start silently in the tray when launched at login

## Versioning

Single source of truth: `project(... VERSION ...)` in `CMakeLists.txt`.
Feature/UI/UX changes bump MINOR (+0.1.0); small fixes bump PATCH (+0.0.1).
See `AGENTS.md`.

## Building and installing

Debian 13 (trixie) is the reference platform. Dependencies:

```
sudo apt install build-essential cmake ninja-build \
    qt6-base-dev qt6-svg-plugins libqt6svg6 \
    libcurl4-openssl-dev libsqlite3-dev
```

Build, test, and run:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build

./build/swiftdm                  # GUI
./build/swiftdm get <url>        # headless CLI
```

Install as a Debian package (recommended) or with CMake:

```
cd build && cpack
sudo apt install ./swiftdm_0.18.7_amd64.deb

# or, without a package:
sudo cmake --install build --prefix /usr/local
```

The package installs `/usr/bin/swiftdm`, a `.desktop` launcher, hicolor icons,
AppStream metadata, the Apache-2.0 copyright file, and the Chromium extension
under `/usr/share/swiftdm/chromium`. Full details and icon regeneration
targets: `packaging/README.md`.

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
src/store/        settings (INI) + SQLite history (downloads/headers)
src/ui/           Qt6 Widgets GUI (main window, download window, dialogs)
ext/chromium/     Chromium Manifest V3 development extension
tests/            CLI/backend tests (+ headless icon rendering test)
binary-original/  analysis-only reference (read-only, not distributed)
ui-assets/        user-provided icons (toolbar SVGs; app-icon/ = app, tray, extension)
packaging/        .desktop entry, AppStream metadata, hicolor icons, .deb maintainer scripts
tools/            helper executables (extension and desktop icon rasterizers)
```
