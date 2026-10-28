<p align="center">
  <img src="ui-assets/app-icon/main-icon.svg" alt="SwiftDM icon" width="128">
</p>

# SwiftDM

[![CI](https://github.com/rifqi-fajarianto/swift-download-manager/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/rifqi-fajarianto/swift-download-manager/actions/workflows/ci.yml)

SwiftDM is a desktop download manager for Linux, with an experimental Windows
10 (version 1809+) port in progress. It supports segmented
downloads, pausing and resuming transfers, and both graphical and command-line
interfaces. SwiftDM is built with C++20, Qt 6, libcurl, and SQLite.

This project is a clean-room implementation based on behavioral specifications
documented in [`docs/spec/`](docs/spec/). No code, assets, icons, or branding are
taken from the application used as a behavioral reference.

SwiftDM is inspired by [Neat Download Manager](https://www.neatdownloadmanager.com/index.php/en/),
a download manager for Windows. Credit for the original concept and user
experience goes to its authors. SwiftDM is an independent project: it is not
affiliated with, endorsed by, or sharing code, assets, icons, or branding with
Neat Download Manager.

## Screenshots

![SwiftDM main window](screenshots/main-window.png)

| Download confirmation | Download progress | Connection segments |
|---|---|---|
| ![Download confirmation dialog](screenshots/download-confirm.png) | ![Download progress window](screenshots/download-progress.png) | ![Connections and segments tab](screenshots/download-segment.png) |

| Download options | Download properties | Application settings |
|---|---|---|
| ![Download options](screenshots/download-options.png) | ![Download properties](screenshots/download-properties.png) | ![SwiftDM settings](screenshots/options.png) |

> The settings screenshot still shows the window of 0.18.9 (three tabs). Since
> 0.18.10 the window has the tabs `General`, `Interface`, `Download` and
> `Proxy/Socks`; since 0.18.20 each tab groups its options into labeled
> sections. The `Passwords` tab is planned and currently hidden (0.20.0).

## Features

- Download over HTTP, HTTPS, FTP, FTPS, and HLS playlists (`.m3u8`).
- Split HTTP/HTTPS downloads across multiple connections; configure the
  connection count.
- Pause, resume, and restore downloads after restarting the application.
- Organize downloads by category and destination folder; view progress, speed,
  estimated time remaining, and active connections/segments.
- Group downloads into named queues. Selecting the `Queues` branch shows a
  queue overview and switches the toolbar to queue mode. Each queue has its own
  workspace, simultaneous-download limit, connection count, shared speed limit,
  autostart, completion notifications, and an optional shutdown when it
  finishes. Queues keep completed items visible and can be started or paused
  independently.
- Configure global, per-download, and per-queue speed limits, including
  changing the limit while a transfer is running.
- Use HTTP, SOCKS4, or SOCKS5 proxies; configure HTTP headers, cookies, referer,
  User-Agent, and POST data.
- Store download history in SQLite and application settings in an INI file.
- Keep a per-download log (`logs/<id>-<name>.log`, opened from Download
  Properties) and optionally stamp the file with the server's date
  (`Get File Creation Date From Server`).
- Handle long file names: a name that does not fit the filesystem is shortened
  automatically, keeping the beginning of the name and its extension.
- Adjust the appearance: light, dark, or follow the system theme; toolbar size
  and toolbar text; simple or colorful tray icon. The changes apply right away.
  The message boxes use the app's own icons instead of the platform ones.
- Download from the command line without opening the GUI.
- Preview integration with the Chromium extension: send links or pages,
  intercept HTTP(S) downloads, and detect playlists/media pairs.
- Browser download confirmations appear while link metadata is checked; the
  initial transfer reuses that probe instead of repeating network checks.
- Resolve supported Google Drive download link formats.

> Note: a Firefox extension and muxing paired audio/video tracks are not
> available yet. The Chromium extension is a development preview. See the
> [parity checklist](docs/spec/06-parity-checklist.md) for feature status and
> limitations.

## Building and running

Each platform has its own build and run sections. Debian 13 (trixie) is the
reference platform; the Windows port is experimental.

### Linux

#### Building on Linux

Install the dependencies:

```sh
sudo apt install build-essential cmake file ninja-build \
  qt6-base-dev qt6-svg-plugins libqt6svg6 \
  libcurl4-openssl-dev libsqlite3-dev
```

`qt6-svg-plugins` is required to load SVG icons at runtime.

Configure, build, and run the tests:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Create a Debian package with CPack, or use the CMake install rules:

```sh
cd build
cpack
sudo apt install ./swiftdm_0.20.2_amd64.deb
```

```sh
sudo cmake --install build --prefix /usr/local
```

The package installs the executable, desktop launcher, icons, AppStream
metadata, and Chromium extension source. It does not install a browser; load the
extension in any Chromium-based browser. See
[`packaging/README.md`](packaging/README.md) for more packaging details.

#### Running on Linux

After installation, launch the graphical application from your desktop menu or
run:

```sh
swiftdm
```

The freshly built binary behaves the same way:

```sh
./build/swiftdm                 # GUI
./build/swiftdm get <url>       # headless CLI
```

The GUI starts by default. When you add a URL, SwiftDM checks the link and shows
a confirmation dialog before starting the download.

For example, set the output directory, filename, and a 1.5 MB/s speed limit:

```sh
swiftdm get 'https://example.org/file.zip' \
  --dir ~/Downloads --name file.zip --limit 1500
```

Other CLI options include `--connections`, `--no-resume`, `--referer`,
`--header`, `--ua`, `--post`, `--config`, `--quiet`, and `--log-level`. For a
complete list, run `swiftdm --help`; `swiftdm --version` prints the version.

### Windows

Windows 10 (version 1809+) is an experimental target, not a supported release
yet. The build uses MSYS2 UCRT64/MinGW-w64 with GCC, CMake, Ninja, Qt 6.5–6.12,
libcurl, and SQLite (Qt 6.12 is the last Qt release that supports Windows 10).

#### Building on Windows

In an MSYS2 UCRT64 shell:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-qt6-base \
  mingw-w64-ucrt-x86_64-qt6-svg mingw-w64-ucrt-x86_64-curl \
  mingw-w64-ucrt-x86_64-sqlite3
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-win --parallel
ctest --test-dir build-win --output-on-failure
```

From PowerShell, set `$env:QT_QPA_PLATFORM = "offscreen"` before `ctest` so the
Qt tests run without a desktop.

The build produces a self-contained `build-win/windows-deploy/` directory,
`build-win/swiftdm-<version>-win64.zip` (`cd build-win && cpack`), and an Inno
Setup installer `build-win/swiftdm-<version>-win64-setup.exe`
(`cmake --build build-win --target windows-installer`, skipped when `ISCC.exe`
is not installed; the build, the tests, and the ZIP package still work without
it).

The packages contain the executable, the Qt plugins, the MinGW/libcurl/SQLite
runtime DLLs, the `ca-bundle.crt` CA bundle used by libcurl for HTTPS, and the
Chromium extension. The installer is per-user, so it needs no administrator
rights; neither it nor the executables are code signed yet, so SmartScreen warns
on first run. The executable carries the SwiftDM icon, which the installer and
its shortcuts use as well.

#### Running on Windows

Start `swiftdm.exe` from the installer shortcut, from
`build-win/windows-deploy/`, or from the unzipped portable package. The
executable uses the Windows GUI subsystem, so starting it from Explorer opens no
console window.

CLI runs attach to the parent console, but shells do not wait for a
GUI-subsystem process, so scripts that must block on a download use
`Start-Process -Wait` (PowerShell) or `start /wait` (cmd):

```powershell
Start-Process -Wait .\swiftdm.exe -ArgumentList `
  'get', 'https://example.org/file.zip', '--dir', "$HOME\Downloads"
```

```bat
start /wait swiftdm.exe get https://example.org/file.zip --dir %USERPROFILE%\Downloads
```

The CLI options are the same as on Linux; run `swiftdm.exe get --help` for the
full list. Version 0.20.2 is the port tracking `main` (per-download logs, the
server file date, named queues, audit hardening, the module split, the Windows
file I/O fixes, and cross-origin redirect handling); GUI checks on Windows 10
are still manual.

### Continuous integration and releases

CI runs the Linux build and test sequence above on Debian 13 for pull requests
and pushes to `main`. Pushing a `vX.Y.Z` tag publishes the matching `.deb`
package, the Windows portable ZIP, and the Windows installer to GitHub Releases;
a `workflow_dispatch` run with `dry_run` enabled builds and tests both platforms
without publishing. The release checklist is in [`AGENTS.md`](AGENTS.md).

## Application data

By default, SwiftDM stores its configuration in
`~/.config/swiftdm/swiftdm.ini`, download history in
`~/.local/share/swiftdm/swiftdm.db`, and temporary download workspaces in
`~/.local/share/swiftdm/tmp`. These locations follow the XDG environment
variables.
The settings file and history database are owner-only (`0600`) inside private
app directories (`0700`). Temporary download workspaces are also owner-only
(`0700`); partial files inside them retain mode `0644`. Proxy passwords remain
stored as plaintext in the settings file for now; Secret Service integration is
deferred.

On Windows the same files live under `%APPDATA%\SwiftDM\swiftdm.ini` and
`%LOCALAPPDATA%\SwiftDM\` (database and `tmp`).

## License and attribution

SwiftDM is licensed under the [Apache License 2.0](LICENSE).

Some user-interface icons are from **Tabler Icons** by Tabler. Tabler Icons are
licensed under the MIT License. Thanks to the Tabler contributors for creating
and maintaining these icons.

- Website: <https://tabler.io/icons>
- Repository and license text: <https://github.com/tabler/tabler-icons>
- Tabler Icons license: <https://github.com/tabler/tabler-icons/blob/main/LICENSE>

## Documentation

- [Specifications and feature status](docs/spec/06-parity-checklist.md)
- [Chromium extension guide](ext/chromium/README.md)
- [Packaging guide](packaging/README.md)
- [Changelog](CHANGELOG.md)
