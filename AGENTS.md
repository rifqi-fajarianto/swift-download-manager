# AGENTS.md — Konteks & Aturan Kerja Agen AI

Berlaku untuk seluruh repositori. Baca file ini sebelum menganalisis atau
mengubah kode. Dokumen ini adalah ringkasan operasional; detail perilaku ada
di `docs/spec/`, `README.md`, dan `CHANGELOG.md`.

## 0. Mulai dari sini

Setiap sesi baru lakukan pemeriksaan berikut sebelum bekerja:

1. Jalankan `git status --short`, `git branch --show-current`, dan
   `git log --oneline -5`.
2. Baca `README.md`, `docs/spec/06-parity-checklist.md`, dan entri teratas
   `CHANGELOG.md`.
3. Cocokkan versi kerja dengan `project(... VERSION ...)` di `CMakeLists.txt`.
4. Baca bagian 2 (status pengembangan). Jangan menimpa perubahan yang belum
   di-commit; perlakukan itu sebagai pekerjaan user.
5. Tentukan modul yang tersentuh, lalu baca spec terkait sebelum mengubah
   perilaku aplikasi.
6. Build dan jalankan test sebelum menyimpulkan pekerjaan sudah selesai.

## 1. Ringkasan proyek & tujuan

- **Aplikasi**: SwiftDM (repositori `simple-download-manager`).
- **Tujuan**: download manager desktop Linux dengan unduhan tersegmentasi,
  pause/resume, riwayat, kategori, proxy, pembatas kecepatan, dan integrasi
  browser.
- **Pendekatan**: reimplementasi **clean-room** dari Neat Download Manager
  (Windows). Perilaku asal ditulis lebih dulu sebagai spesifikasi di
  `docs/spec/`, lalu diimplementasikan dari spec tersebut.
- **Stack**: C++20, CMake >= 3.28, Ninja, Qt6 Widgets/Network, libcurl,
  SQLite3. Tidak memakai C++ modules (`CMAKE_CXX_SCAN_FOR_MODULES OFF`).
- **Platform acuan**: Debian 13 (trixie); build Linux lain dapat bekerja selama
  dependency tersedia.
- **Bahasa kode/komentar**: Inggris; dokumentasi dan interaksi dengan user
  menggunakan bahasa Indonesia.

### Batasan clean-room (WAJIB)

- Dilarang menyalin kode, aset, ikon, atau branding dari aplikasi asal.
- `binary-original/` (jika ada) hanya untuk analisis, **read-only**, tidak boleh
  diubah, di-commit, atau didistribusikan.
- Fitur "phone-home" (update check ke server aplikasi asal) tidak direplikasi.
- Ketidakpastian perilaku asal ditandai `UNKNOWN` di spec; jangan menebak lalu
  menganggapnya fakta.

## 2. Status pengembangan (snapshot)

> Snapshot terakhir: **2026-10-19**, versi kerja **0.18.1**. Status ini cepat
> berubah. Verifikasi ulang dengan git dan `CMakeLists.txt`; jika berbeda,
> percayai kondisi repositori dan perbarui bagian ini pada commit berikutnya.

- **Commit terakhir di `main`**: `c45508d` — dokumentasi lifecycle workspace
  sementara dan pembersihan file parsial.
- **Branch saat snapshot**: `speed-limit-improvement`.
- **Rilis terakhir yang sudah di-commit**: 0.18.0.
- **Versi kerja**: 0.18.1 (speed limiter toggle, preset, dan tampilan limit
  aktif). Implementasi dan dokumentasinya sudah ada di working tree tetapi
  **belum di-commit** saat snapshot.
- **Versi ekstensi Chromium**: 0.5.0 (`ext/chromium/manifest.json`), terpisah
  dari versi aplikasi.

### Sudah tersedia sampai 0.18.0

- Engine HTTP/HTTPS tersegmentasi via libcurl multi: resume, retry/backoff,
  proxy HTTP/SOCKS, cookie ter-scope, header kustom, POST body, dan validasi
  state resume.
- FTP (`ftp://`/`ftps://`) dengan probe `SIZE` dan resume `REST`.
- HLS/m3u8: pemilihan varian, `EXT-X-MAP`, unduhan segmen berurutan, resume;
  playlist terenkripsi ditolak dengan pesan jelas.
- Resolusi interstitial Google Drive dan ringkasan alasan penolakan host
  (JSON/HTML body) pada pesan error.
- CLI headless `swiftdm get` dan GUI Qt6 lengkap dengan tray, kategori,
  properties, konfirmasi download, serta pemulihan riwayat dari SQLite.
- Integrasi Chromium MV3: handoff otomatis/manual, cookie opsional, deteksi
  HLS dan pasangan video+audio, status push lewat WebSocket.
- Packaging: aturan install CMake, `.deb` via CPack, desktop entry, AppStream,
  ikon hicolor, dan instalasi ekstensi.

### 0.18.1 — pekerjaan yang ada di working tree

- Toggle "Enable speed limiter" global (`BandWidthLimitEnabled`) dan
  per-download (`bandwidthlimit_enabled` di history).
- `SpeedLimitEditor` bersama untuk Settings dan tab Options Download window:
  preset 500 KB/s, 1/5/10/20/50 MB/s, plus nilai kustom.
- Limit juga diterapkan pada jalur sequential (HTTP tanpa ukuran, FTP, HLS)
  lewat receive-rate cap libcurl.
- Migrasi kompatibel: config/DB lama dengan limit non-nol dianggap "aktif".
- Test `test_settings` dan `test_history` diperbarui; dokumentasi 0.18.1 sudah
  ditambahkan ke `CHANGELOG.md`, `README.md`, dan `docs/spec/`.

### Rencana / gap yang diketahui

- Queue scheduling dan batas unduhan simultan; tombol "Add to queue" masih
  placeholder.
- Per-download log file dan URL yang dapat diedit di Properties.
- UI saved passwords (`auths`).
- Penerapan "Get File Creation Date From Server" ke mtime file.
- Ekstensi Firefox.
- Muxing MKV/WebM untuk pasangan video+audio (saat ini dua file terpisah
  dengan `groupid`).
- In-page media panel dan site extractor (C4–C6, C9 di
  `docs/spec/07-chromium-extension-reference.md`).
- Verifikasi GUI selalu manual oleh user; tidak ada automation GUI.

## 3. Peta codebase

| Path | Tanggung jawab |
|---|---|
| `src/app/main.cpp` | Entry point: GUI default, `--gui`, `--version`, `--help`, dan CLI `get`. |
| `src/core/` | Logging (`log.*`) dan utilitas umum: format byte/speed/durasi, path XDG, sanitasi nama file, pencarian file hasil unduhan. |
| `src/engine/` | `DownloadEngine::run`, request/result/progress, workspace, segment plan + state `segments.json`, kategori, HLS. |
| `src/net/` | Lapisan libcurl: `probe_url`, `fetch_text`, `stream_download`, `SegmentDownloader`, resolver interstitial Google Drive. |
| `src/store/` | `Settings` (INI) dan `HistoryStore` (SQLite: `downloads`, `headers`, `auths`). |
| `src/ui/` | Qt6 Widgets: `MainWindow`, model/filter, `DownloadTask` (worker thread), dialog-dialog, tema/ikon, dan `BrowserIntegration` (WebSocket loopback). |
| `ext/chromium/` | Ekstensi Chromium Manifest V3 (popup, service worker, cookie permission, ikon). |
| `tests/` | Unit/integration test backend, server HTTP/FTP lokal, harness Node untuk logika ekstensi, test ikon offscreen, dan test protokol browser. |
| `docs/spec/` | Spesifikasi perilaku clean-room, data model, engine, protokol, settings, parity checklist, referensi ekstensi, evidence ledger. |
| `packaging/` | Desktop entry, AppStream metadata, ikon hicolor, maintainer script Debian, dokumentasi packaging. |
| `tools/` | Rasterizer ikon ekstensi dan ikon desktop. |
| `ui-assets/` | Sumber SVG ikon toolbar dan ikon aplikasi/ekstensi. |
| `cmake/` | Template `version.h.in` dan helper CMake. |

### Alur data utama

1. **Manual / browser** → probe URL tanpa membuat row/DB entry → dialog
   konfirmasi (kategori + lokasi) → `DownloadItem` + `DownloadTask`.
2. **`DownloadTask`** berjalan di worker thread → `DownloadEngine::run` →
   `probe_url` → segmented download atau sequential stream (HTTP/FTP/HLS) →
   workspace sementara → file final.
3. **Progress** dikirim lewat signal Qt ke model/UI; checkpoint progress
   ditulis ke SQLite sekitar tiap 5 detik dan saat keluar.
4. **Startup** → `HistoryStore::load_all` → `MainWindow::restore_history`
   membangun ulang daftar; row yang masih aktif menjadi Paused dan tidak
   dimulai otomatis.
5. **Browser** → WebSocket `swiftdm.v1` di `127.0.0.1:10007` →
   `BrowserIntegration` → `MainWindow` → konfirmasi yang sama seperti manual.

## 4. Build, run, dan test

Dependency Debian 13:

```sh
sudo apt install build-essential cmake ninja-build \
    qt6-base-dev qt6-svg-plugins libqt6svg6 \
    libcurl4-openssl-dev libsqlite3-dev
```

Build dan test:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Menjalankan aplikasi:

```sh
./build/swiftdm                  # GUI
./build/swiftdm --version
./build/swiftdm get <url>        # CLI headless
```

Test yang tersedia: `core_version`, `plan`, `settings`, `history`, `restore`,
`media`, `http`, `extension_logic` (butuh Node), `icons` (offscreen), dan
`browser_protocol` (tanpa GUI). Test HTTP/FTP memakai server lokal di
`tests/http_test_server.h` dan `tests/ftp_test_server.h`.

Regenerasi ikon setelah mengubah SVG:

```sh
cmake --build build --target render-desktop-icons
cmake --build build --target render-extension-icons
```

## 5. Kebijakan versi (WAJIB)

Format `MAJOR.MINOR.PATCH`, dengan satu sumber kebenaran di `CMakeLists.txt`:

- `project(SwiftDM VERSION <MAJOR>.<MINOR>.<PATCH>)`
- `cmake/version.h.in` di-`configure_file` menjadi
  `build/generated/swiftdm/version.h`. File itu **generated** dan tidak boleh
  diedit manual. Versi dipakai oleh `--version`, judul window, dan About.
- Istilah "major" pada kebijakan ini berarti **perubahan fitur**, bukan
  kenaikan SemVer MAJOR. Selama fase 0.x, MAJOR tetap 0.

Aturan kenaikan versi pada setiap perubahan aplikasi:

1. **Perubahan fitur** — fitur baru, penambahan/perubahan fungsi UI, atau
   perubahan UX → MINOR +1 dan PATCH di-reset ke 0.
   Contoh: `0.18.1` → `0.19.0`.
2. **Perubahan non-fitur** — perbaikan bug, refactor, penyesuaian kecil tanpa
   fitur/fungsi UI/UX baru → PATCH +1.
   Contoh: `0.18.1` → `0.18.2`.
3. Setiap perubahan perilaku aplikasi wajib disertai kenaikan versi yang
   sesuai, catatan `CHANGELOG.md`, dan pembaruan status di `README.md`.
4. Untuk rilis, periksa juga versi contoh di `packaging/README.md` dan
   `packaging/io.github.swiftdm.SwiftDM.metainfo.xml`.
5. Perubahan dokumentasi murni (tanpa perilaku aplikasi) tidak perlu menaikkan
   versi aplikasi.

## 6. Pengujian oleh agen AI (WAJIB)

1. Agen AI **DILARANG** menguji aplikasi secara otomatis lewat UI: GUI
   automation, klik otomatis, screenshot otomatis, `xdotool`, dan sejenisnya.
2. Agen AI **HANYA** boleh menguji lewat CLI/backend:
   - unit test dan integration test tanpa GUI,
   - binary dengan flag CLI (mis. `--version`, `get`),
   - engine/jaringan/DB secara langsung dengan server uji lokal,
   - test Qt headless yang memang offscreen (mis. test ikon dan protokol
     browser), selama tidak menampilkan window.
3. Bila pengujian UI diperlukan, agen tidak melakukannya sendiri. Agen harus:
   (a) memberi tahu user bagian UI mana yang perlu diuji beserta langkah
   singkatnya, lalu (b) menunggu feedback user sebelum memperbaiki atau
   menyempurnakan UI.
4. Jangan menganggap build sukses sebagai bukti perilaku benar. Jalankan
   `ctest` dan, untuk perubahan engine/jaringan, tambahkan test regresi.

## 7. Konvensi implementasi

- Gunakan C++20 dan `std::`; jangan menambah dependency tanpa alasan kuat.
  Dependency yang sudah ada: Qt6, libcurl, SQLite3, pthread.
- Namespace inti: `swiftdm`; kode UI: `swiftdm::ui`.
- `src/core`, `src/engine`, `src/net`, dan `src/store` tidak boleh bergantung
  pada Qt Widgets. UI boleh memakai semuanya.
- `DownloadTask` berjalan di worker thread. Jangan menyentuh widget langsung
  dari worker; gunakan signal/slot dan salin data yang diperlukan.
- Migrasi SQLite harus **additive** (`ALTER TABLE ... ADD COLUMN`) dan
  mempertahankan baris lama. Beri backfill untuk semantik baru bila perlu
  (contoh: `bandwidthlimit_enabled` mengikuti limit non-nol pada row lama).
- Nama key settings mengikuti nama asli (`MaxConnections`,
  `BandWidthLimitEnabled`, dst.) agar pemetaan ke spec tetap jelas. Path
  runtime menggunakan XDG, bukan registry Windows.
- Pesan error harus bisa dibaca manusia: sertakan status HTTP/reason dan, bila
  ada, ringkasan alasan dari body host. Jangan menulis body 4xx/5xx ke file
  parsial.
- Jangan mengubah file generated (`build/generated/...`), artefak build, atau
  `compile_commands.json`.
- Untuk test, gunakan direktori sementara lewat helper `tests/test_util.h` dan
  bersihkan setelah selesai.
- Ikon dan aset hanya dari `ui-assets/` atau `src/ui/icons/`; jangan mengambil
  aset dari aplikasi asal.
- Pertahankan gaya kode sekitar: komentar menjelaskan alasan, bukan mengulang
  kode; fungsi kecil dan nama deskriptif.

## 8. Data dan path runtime

- Config: `$XDG_CONFIG_HOME/swiftdm/swiftdm.ini`
  (default `~/.config/swiftdm/swiftdm.ini`).
- Database: `$XDG_DATA_HOME/swiftdm/swiftdm.db`
  (default `~/.local/share/swiftdm/swiftdm.db`); tabel `downloads`, `headers`,
  `auths`.
- Temp/workspace: default `$XDG_DATA_HOME/swiftdm/tmp`, satu direktori
  `download-<hash>/` berisi `<nama>.part` dan `segments.json`. Workspace
  dipertahankan saat pause/error/keluar dan dihapus saat selesai atau row
  dihapus.
- Autostart: `~/.config/autostart/swiftdm.desktop`.
- Browser integration: WebSocket loopback `127.0.0.1:10007`, subprotokol
  `swiftdm.v1`; ekstensi hasil install ada di `/usr/share/swiftdm/chromium`.
- Log per-download direncanakan di
  `$XDG_DATA_HOME/swiftdm/logs/<id>-<filename>.log`; ini belum menjadi fitur
  yang selesai (lihat parity checklist).

## 9. Dokumen acuan

| Dokumen | Isi |
|---|---|
| `README.md` | Ringkasan proyek, status rilis, build/install. |
| `CHANGELOG.md` | Riwayat perubahan per versi; sumber kebenaran fitur terbaru. |
| `docs/spec/01-product.md` | Inventaris fitur, UX, layout window, alur pengguna. |
| `docs/spec/02-data-model.md` | Skema SQLite, state file, layout temp/destination. |
| `docs/spec/03-engine.md` | Segmentasi, resume, retry, rate limiting, FTP/HLS. |
| `docs/spec/04-protocols.md` | HTTP/FTP, protokol WebSocket ekstensi, format error. |
| `docs/spec/05-settings.md` | Key settings, default, dan per-download override. |
| `docs/spec/06-parity-checklist.md` | Status parity, fitur yang ditunda/di-drop, verification notes. |
| `docs/spec/07-chromium-extension-reference.md` | Referensi perilaku ekstensi asal dan decision log. |
| `ext/chromium/README.md` | Instalasi, troubleshooting, dan batasan ekstensi. |
| `packaging/README.md` | Build/install `.deb`, ikon, dan layout paket. |

## 10. Definition of done & commit

Sebuah perubahan dianggap selesai bila:

1. Perilaku yang berubah sudah sesuai dengan `docs/spec/` (perbarui spec bila
   perlu) dan tidak melanggar batasan clean-room.
2. Build berhasil dan `ctest --test-dir build --output-on-failure` lulus.
3. Test baru/regresi ditambahkan untuk perubahan engine, jaringan, store, atau
   logika yang berisiko.
4. Versi dinaikkan sesuai bagian 5 dan `CHANGELOG.md` diperbarui.
5. `README.md` dan `docs/spec/06-parity-checklist.md` diperbarui bila status
   fitur berubah.
6. Untuk perubahan UI, checklist pengujian manual sudah diberikan ke user dan
   feedback-nya sudah diterima.
7. Bagian 2 (status) di file ini diperbarui bila ada rilis, merge besar, atau
   perubahan fokus pengembangan.

Setelah development dan testing selesai, langsung commit dengan pesan yang
menjelaskan perubahan. Ikuti gaya commit yang sudah dipakai:

- `feat(scope): ringkasan fitur (0.x.0)`
- `fix(scope): ringkasan perbaikan (0.x.y)`
- `refactor(scope): ...`
- `docs: ...`

Aturan commit:

- Stage file secara spesifik; **jangan** `git add -A` jika ada perubahan lain
  yang belum menjadi bagian pekerjaan.
- Jangan commit `build/`, artefak generated, `binary-original/`, atau file
  sementara.
- Jangan mengubah, revert, atau meng-amend commit/perubahan user tanpa izin.
- Jangan push ke remote kecuali diminta.
