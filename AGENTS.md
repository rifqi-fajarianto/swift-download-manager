# AGENTS.md — Konteks & Aturan Kerja Agen AI

Berlaku untuk seluruh repositori. Baca file ini sebelum menganalisis atau
mengubah kode. Dokumen ini adalah ringkasan operasional; detail perilaku ada
di `docs/spec/`, `README.md`, dan `CHANGELOG.md`.

## 0. Mulai dari sini

Setiap sesi baru lakukan pemeriksaan berikut sebelum bekerja:

1. AI Agent tidak boleh memodifikasi, meng-edit, mengubah file ini tanpa izin dari user. AI Agent hanya boleh mengubah isi file ini jika di minta oleh user
2. Jalankan `git status --short`, `git branch --show-current`, dan
   `git log --oneline -5`.
3. Baca `README.md`, `docs/spec/06-parity-checklist.md`, dan entri teratas
   `CHANGELOG.md`.
4. Cocokkan versi kerja dengan `project(... VERSION ...)` di `CMakeLists.txt`.
5. Baca bagian 2 (status pengembangan). Jangan menimpa perubahan yang belum
   di-commit; perlakukan itu sebagai pekerjaan user.
6. Tentukan modul yang tersentuh, lalu baca spec terkait sebelum mengubah
   perilaku aplikasi.
7. Build dan jalankan test sebelum menyimpulkan pekerjaan sudah selesai.
8. Sarankan refactor ke user untuk file yang berisi lebih dari 2000 line of code, 
9. setiap refactor jangan sampai ada fitur dan UI yang berubah atau tidak berfungsi.

## 1. Ringkasan proyek & tujuan

- **Aplikasi**: SwiftDM (repositori `simple-download-manager`).
- **Tujuan**: download manager desktop Linux dengan unduhan tersegmentasi,
  pause/resume, riwayat, kategori, proxy, pembatas kecepatan, dan integrasi
  browser.
- **Pendekatan**: reimplementasi **clean-room** dari Neat Download Manager
  (Windows). Perilaku asal ditulis lebih dulu sebagai spesifikasi di
  `docs/spec/`, lalu diimplementasikan dari spec tersebut.
- **Stack**: C++20, CMake >= 3.28, Ninja, Qt6 Widgets/Network, libcurl,
  SQLite3. Port Windows eksperimental memakai MSYS2 UCRT64/MinGW-w64 dan Qt
  6.5–6.12. Tidak memakai C++ modules (`CMAKE_CXX_SCAN_FOR_MODULES OFF`).
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

## 2. Status pengembangan

Status pengembangan dipindahkan ke file terpisah:
[`docs/development_status.md`](docs/development_status.md)

## 3. Peta codebase

| Path | Tanggung jawab |
|---|---|
| `src/app/main.cpp` | Entry point: GUI default, `--gui`, `--version`, `--help`, dan CLI `get`. |
| `src/core/` | Logging (`log.*`), file I/O lintas platform (`file_io.*`), include Windows terpusat (`win_headers.h`), dan utilitas umum: format byte/speed/durasi, path platform, sanitasi nama file, pencarian file hasil unduhan, penghapusan direktori aman path panjang. |
| `src/engine/` | `DownloadEngine::run`, request/result/progress, workspace, segment plan + state `segments.json`, kategori, HLS. |
| `src/net/` | Lapisan libcurl: `probe_url`, `fetch_text`, `stream_download`, `SegmentDownloader`, resolver interstitial Google Drive. |
| `src/store/` | `Settings` (INI) dan `HistoryStore` (SQLite: `downloads`, `headers`, `auths`, `queues`). |
| `src/ui/` | Qt6 Widgets: `MainWindow`, model/filter, `DownloadTask` (worker thread), dialog-dialog, `queue_logic`/`queue_dialog`, tema/ikon, dan `BrowserIntegration` (WebSocket loopback). |
| `ext/chromium/` | Ekstensi Chromium Manifest V3 (popup, service worker, cookie permission, ikon). |
| `tests/` | Unit/integration test backend, server HTTP/FTP lokal, harness Node untuk logika ekstensi, test ikon offscreen, dan test protokol browser. |
| `docs/spec/` | Spesifikasi perilaku clean-room, data model, engine, protokol, settings, parity checklist, referensi ekstensi, evidence ledger. |
| `packaging/` | Desktop entry, AppStream metadata, maintainer script Debian, skrip installer Inno Setup (`packaging/windows/swiftdm.iss`), dokumentasi packaging. Ikon launcher hicolor dirender saat build ke `build/generated/icons/`. |
| `tools/` | Rasterizer ikon ekstensi, ikon desktop, dan ikon Windows (`.ico`). |
| `ui-assets/` | Sumber SVG ikon toolbar dan ikon aplikasi/ekstensi. |
| `cmake/` | Template `version.h.in` dan helper CMake. |

### Alur data utama

1. **Manual / browser** → probe URL tanpa membuat row/DB entry → dialog
   konfirmasi (kategori + lokasi + queue) → `DownloadItem` + `DownloadTask`.
2. **`DownloadTask`** berjalan di worker thread → `DownloadEngine::run` →
   `probe_url` → segmented download atau sequential stream (HTTP/FTP/HLS) →
   workspace sementara → file final.
3. **Progress** dikirim lewat signal Qt ke model/UI; checkpoint progress
   ditulis ke SQLite sekitar tiap 5 detik dan saat keluar.
4. **Startup** → `HistoryStore::load_all` → `MainWindow::restore_history`
   membangun ulang daftar; row aktif di main menjadi Paused, row queue menjadi
   Queued, dan queue dengan autostart dijalankan.
5. **Browser** → WebSocket `swiftdm.v1` di `127.0.0.1:10007` →
   `BrowserIntegration` → `MainWindow` → konfirmasi yang sama seperti manual.

## 4. Build, run, dan test

Windows 10 (1809+) adalah target port eksperimental, belum menjadi platform
rilis yang didukung. Gunakan MSYS2 UCRT64 dengan GCC, CMake, Ninja, Qt 6.5–6.12,
libcurl, dan SQLite; Qt 6.12 adalah lini Qt terakhir yang mendukung Windows 10.
CI membangun executable Windows, menjalankan seluruh `ctest`, dan membuat paket
ZIP serta installer Inno Setup; verifikasi GUI manual di Windows 10 masih
menjadi tugas user. Installer hanya dibuat bila `ISCC.exe` ditemukan, jadi
mesin tanpa Inno Setup tetap bisa build dan `cpack`.

Dependency Debian 13:

```sh
sudo apt install build-essential cmake file ninja-build \
    qt6-base-dev qt6-svg-plugins libqt6svg6 \
    libcurl4-openssl-dev libsqlite3-dev
```

Build dan test:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

CI (`.github/workflows/ci.yml`) menjalankan build dan test yang sama di
container Debian 13 untuk push ke `main` dan semua PR. Job kedua membangun
dengan `-DSWIFTSDM_BUILD_UI=OFF` tanpa Qt untuk menjaga batas arsitektur inti.
Job ketiga mengulang build headless itu di bawah sanitizer (matriks
`address+undefined` dan `thread`); `ASAN_OPTIONS=detect_leaks=0` karena Qt-free
test menutup banyak handle socket. Rilis (`.github/workflows/release.yml`)
menerbitkan `.deb`, ZIP portable Windows, installer `-setup.exe`, `SHA256SUMS`,
dan build provenance attestation untuk ketiganya. `workflow_dispatch` punya
input `dry_run` (default aktif): job yang sama dibangun dan diuji, artifact
diunggah untuk diperiksa, tetapi release tidak dibuat, diubah, atau diberi
attestation.

Menjalankan aplikasi:

```sh
./build/swiftdm                  # GUI
./build/swiftdm --version
./build/swiftdm get <url>        # CLI headless
```

Test yang tersedia: `core_version`, `plan`, `settings`, `history`, `restore`,
`media`, `filenames`, `file_io`, `ca_bundle`, `http`, `extension_logic` dan
`extension_popup` (butuh Node), `icons` (offscreen), `confirm_dialog`
(offscreen), `settings_dialog` (offscreen), `properties_dialog` (offscreen),
`download_window` (offscreen), `queue_logic`, `queue_dialog` dan `queue_picker`
(offscreen), `action_state`, `theme` (offscreen), dan `browser_protocol` (tanpa
GUI). Seluruh
test berjalan di Linux dan Windows; test HTTP/FTP memakai server lokal di
`tests/http_test_server.h` dan `tests/ftp_test_server.h` yang memakai Winsock di
Windows. Di Windows, jalankan dengan `$env:QT_QPA_PLATFORM = "offscreen"`.

Regenerasi ikon setelah mengubah SVG:

- Ikon launcher (hicolor) dirender otomatis saat build dari
  `ui-assets/app-icon/main-icon.svg`; hasilnya ada di
  `build/generated/icons/hicolor/` dan ikut ter-install.
- Ikon Windows (`ui-assets/app-icon/main.ico`, dipakai resource `swiftdm.exe`
  dan `SetupIconFile`) dan ikon aksi ekstensi Chromium masih manual:

```sh
cmake --build build --target render-ico
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
   `packaging/io.github.swiftdm.SwiftDM.metainfo.xml`. Prosedur rilis otomatis
   ada di bagian 11.
5. Perubahan dokumentasi murni (tanpa perilaku aplikasi) tidak perlu menaikkan
   versi aplikasi.
6. **Pengecualian yang disetujui**: batch restrukturisasi window Settings
   (0.18.10 → 0.18.13) memakai seri patch `0.18.x` atas keputusan maintainer,
   walaupun isinya perubahan fitur, karena seluruhnya adalah satu batch
   restrukturisasi yang tidak dirilis terpisah. Penyimpangan ini dicatat di
   `CHANGELOG.md` (0.18.10) dan di bagian 2 dokumen ini. Batch berikutnya
   kembali memakai aturan biasa (fitur baru → MINOR +1).

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
  `auths`, `queues`; `downloads.queueid` menunjuk queue pemilik row
  (0/NULL = daftar utama).
- Temp/workspace: default `$XDG_DATA_HOME/swiftdm/tmp`, satu direktori
  `download-<hash>/` berisi `<nama>.part` dan `segments.json`. Workspace
  dipertahankan saat pause/error/keluar dan dihapus saat selesai atau row
  dihapus.
- Autostart: `~/.config/autostart/swiftdm.desktop`.
- Browser integration: WebSocket loopback `127.0.0.1:10007`, subprotokol
  `swiftdm.v1`; ekstensi hasil install ada di `/usr/share/swiftdm/chromium`
  (Linux) atau folder `chromium-extension/` di samping `swiftdm.exe` (Windows).
- Windows: config di `%APPDATA%\SwiftDM\swiftdm.ini`, database dan temp di
  `%LOCALAPPDATA%\SwiftDM\`, autostart lewat nilai `SwiftDM` di
  `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`.
- Log per-download ditulis ke
  `$XDG_DATA_HOME/swiftdm/logs/<id>-<filename>.log` (Windows:
  `%LOCALAPPDATA%\SwiftDM\logs`) sejak 0.20.0; cap 1 MiB per file, dihapus
  bersama row history-nya.

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
   fitur berubah. Saat menambahkan fitur besar, periksa README root dan
   tambahkan atau perbarui deskripsi fitur tersebut jika belum tercantum;
   README adalah ringkasan untuk pengguna, jadi tidak perlu mencatat setiap
   detail implementasi atau perbaikan kecil di sana.
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
- Jangan push ke remote kecuali diminta. Permintaan eksplisit untuk membuat
  rilis versi (bagian 11) adalah izin untuk push commit rilis dan tag.

## 11. Rilis otomatis dan tag versi

Rilis dipicu oleh tag `vMAJOR.MINOR.PATCH` (contoh `v0.19.0`) yang di-push ke
`origin`. Tag itu menjalankan `.github/workflows/release.yml`, yang memvalidasi
kesamaan tag dengan `project(... VERSION ...)`, membangun dan menguji di Debian
13, membuat `.deb`, lalu menerbitkan GitHub Release dengan catatan dari section
`CHANGELOG.md` versi tersebut. Job `windows-package` di runner `windows-latest`
membangun port Windows dari tag yang sama, menjalankan seluruh `ctest`, lalu
menghasilkan ZIP portable dan installer Inno Setup; job `release` menunggu
artifact itu sebelum menerbitkan.

Rilis dapat dimulai dengan dua cara:

1. Maintainer membuat dan push tag versi rilis.
2. Maintainer secara eksplisit meminta agen membuat rilis versi tertentu
   (contoh: "buat rilis 0.19.0"). Permintaan itu adalah izin untuk menjalankan
   seluruh prosedur di bawah, termasuk push commit rilis dan tag. Tanpa
   permintaan eksplisit, agen tetap tidak push (bagian 10).

Checklist sebelum tag:

1. Tentukan versi sesuai bagian 5.
2. Naikkan `project(... VERSION ...)` di `CMakeLists.txt`.
3. Pindahkan entri `Unreleased` di `CHANGELOG.md` menjadi section
   `## X.Y.Z — YYYY-MM-DD` (heading harus memakai format itu; workflow
   mengekstrak catatan rilis dari section ini).
4. Perbarui contoh versi di `README.md` dan `packaging/README.md`, serta
   tambahkan entri `<release version="X.Y.Z" date="YYYY-MM-DD">` di
   `packaging/io.github.swiftdm.SwiftDM.metainfo.xml`.
5. Jalankan configure, build, dan `ctest --test-dir build --output-on-failure`.
6. Commit dengan `chore(release): X.Y.Z`, lalu buat annotated tag:
   `git tag -a vX.Y.Z -m "SwiftDM X.Y.Z"`.
7. Push commit dan tag (`git push origin HEAD` dan `git push origin vX.Y.Z`),
   hanya setelah langkah 1-6 lulus.

Catatan:

- Jangan memindahkan atau menghapus tag yang sudah dipublikasikan. Jika rilis
  bermasalah, perbaiki dan naikkan patch baru.
- Workflow release membuat release sebagai draft, mengunggah `.deb`, ZIP
  Windows, dan installer `-setup.exe`, lalu mempublikasikannya. Re-run workflow
  akan memakai release yang sudah ada dan mengganti aset tersebut (`--clobber`).
- `workflow_dispatch` dengan `dry_run: true` (default) adalah cara aman menguji
  pipeline: build, `ctest`, `cpack`, installer, dan ekstraksi catatan rilis
  tetap dijalankan, hasilnya hanya artifact. Set `dry_run: false` untuk benar-
  benar menerbitkan.
- Installer dan ZIP Windows belum ditandatangani, jadi SmartScreen memperingatkan
  pengguna saat memasang. Tambahkan code signing sebelum port Windows dianggap
  siap rilis stabil.
- Jika rilis gagal sebelum release terbit, perbaiki workflow di `main`, lalu
  jalankan workflow `Release` secara manual dengan input tag yang sama
  (`workflow_dispatch`). Tag tidak perlu dipindahkan.
- Jika tag tidak sama dengan versi di `CMakeLists.txt` atau section changelog
  tidak ada, workflow gagal sebelum menerbitkan apa pun.
- `workflow_dispatch` hanya bisa memakai tag yang sudah di-push ke `origin`.
  Step pemeriksaan tag berjalan sebelum checkout dan gagal dengan pesan yang
  menyebutkan perintah `git tag`/`git push` bila tag belum ada.
- Badge CI di `README.md` menunjuk ke repository
  `rifqi-fajarianto/swift-download-manager`; perbarui URL badge jika
  repository dipindahkan atau diganti nama.
