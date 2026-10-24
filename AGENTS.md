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

> Snapshot terakhir: **2026-10-24**, versi kerja **0.19.3**. Status ini cepat
> berubah. Verifikasi ulang dengan git dan `CMakeLists.txt`; jika berbeda,
> percayai kondisi repositori dan perbarui bagian ini pada commit berikutnya.

- **Commit terakhir di `main`**: `05e18de` — docs: credit Neat Download
  Manager as the inspiration (queue work through 0.19.3 merged).
- **Branch kerja saat snapshot**: `main`.
- **Rilis terakhir yang sudah di-commit di `main`**: 0.19.3.
- **Versi kerja**: 0.19.3. Item queue tidak lagi memicu aksi completion
  global: dialog `Download Complete` dan opsi buka folder hanya berlaku untuk
  daftar utama, sedangkan item queue memakai `Notify when an item completes`
  milik queue. Sebelumnya (0.19.2) `Add to queue` pada Download Confirmation
  tidak lagi memakai queue pertama secara diam-diam: bila ada lebih dari satu
  queue, tombol membuka dialog `Choose Queue` berisi nama + jumlah item, tanpa
  preseleksi. Satu queue langsung dipakai, dan download yang dimulai dari view
  queue tetap menarget queue itu tanpa picker. Row `Queue:` di dialog
  konfirmasi dihapus dan tinggi window kembali ke 650×330 (minimum 560×320).
  Perubahan UX 0.19.2 memakai patch atas keputusan maintainer, meskipun
  kebijakan normal untuk UX adalah MINOR +1.
  Named queues: cabang `Queues` di sidebar, tabel
  `queues` + kolom `downloads.queueid`, dialog `Queue Properties` (max
  simultaneous 0 = Unlimited, connections 0 = global, limit kecepatan total
  queue, autostart, notifikasi, shutdown dengan delay), `Start Queue`/`Stop
  Queue` dengan urutan insertion, pemindahan item antara Downloads dan queue,
  hapus queue, notifikasi all-complete, dan shutdown countdown yang bisa
  dibatalkan. Memilih cabang `Queues` langsung mengaktifkan mode toolbar queue
  dan menampilkan overview queue (nama + jumlah item, atau prompt `New Queue`
  bila kosong); memilih queue dari overview membuka daftar unduhannya. Urutan
  toolbar queue: `New Queue`, `Delete Queue`, `Queue Properties` | `Start
  Queue`, `Stop Queue` | `New URL`, `Delete`, `Delete File`, dengan aksi
  queue-specific nonaktif sampai satu queue dibuka. Window Download Properties
  memakai empat section
  berlabel (`General`, `Location`, `Source`, `Time`) dengan judul bold dan
  label sentence case; field dan perilakunya tidak berubah. Window dibuka
  650×690, nama file panjang di-elide di tengah dengan tooltip, dan baris
  Source kondisional (Page URL/Page title/Pair) hanya muncul bila datanya ada.
  Window progress memakai bahasa section yang sama: tab Download berisi
  `Details`/`Progress`, tab Options berisi `Destination`/`Transfer`, dan tab
  Connections menaruh tabel segmen dalam satu frame. Window progress dibuka
  650×500 (minimum 520×420); label memakai sentence case, tombol Download
  rata kanan, dan speed-limit editor memakai dua baris. Window Download
  Confirmation memakai section `File` (Name, Size) dan `Save` (URL, Category,
  Location), dibuka 650×330 (minimum 560×320), dengan `Cancel` di kiri serta
  `Add to queue`/`Download Now` di kanan. Batch layout ini memakai patch
  0.18.22 atas keputusan maintainer, meskipun kebijakan normal untuk UX adalah
  MINOR +1.
  Tombol toolbar `Resume` aktif hanya jika minimal
  satu item terpilih bisa di-resume (Paused/Error/Queued, task tidak aktif) dan
  `Stop` aktif hanya jika minimal satu item sedang `Downloading`; seleksi
  campuran menyalakan tombol yang punya item cocok. Toolbar memakai divider
  setelah `New URL` dan setelah `Stop`. Batch UX ini memakai patch 0.18.21 atas
  keputusan maintainer, meskipun kebijakan normal untuk UX adalah MINOR +1.
  Window Settings memakai section berlabel di kelima
  tab (General, Interface, Download, Proxy/Socks, Passwords) agar opsi lebih
  mudah dipindai; hanya layout/teks yang berubah, tanpa perubahan key, default,
  atau perilaku. Batch layout ini memakai patch 0.18.20 atas keputusan
  maintainer, meskipun kebijakan normal untuk UX adalah MINOR +1.
  Konfirmasi unduhan langsung muncul saat probe
  berjalan di background; tombol Download Now aktif setelah metadata tersedia,
  dan probe sukses dipakai ulang untuk transfer awal (Resume tetap probe ulang).
  Extension Chromium mempertahankan WebSocket perintah selama koneksi aktif.
  Batch UX/perbaikan latensi ini memakai patch 0.18.19 atas keputusan
  maintainer, meskipun kebijakan normal untuk UX adalah MINOR +1.
  Nama file yang melebihi batas komponen path
  (240 byte UTF-8) dipotong otomatis: awal nama dan extension dipertahankan,
  hash FNV-1a 64-bit ditambahkan; nama device Windows disanitasi. Lihat
  0.18.17 untuk opsi "Show Download Confirmation Dialog" di tab `General`;
  saat dimatikan, unduhan (manual maupun dari browser) langsung jalan
  tanpa dialog konfirmasi, memakai kategori bawaan dan folder default, dengan
  window progress tetap dibuka. Note di bawah opsi menjelaskan hal ini dan
  hanya tampil saat opsi dimatikan. Key: `ShowDownloadConfirmationDialog`
  (section `[general]`, default 1).
  Ikon launcher (hicolor PNG) dirender otomatis saat
  build dari `ui-assets/app-icon/main-icon.svg` ke `build/generated/icons/`;
  PNG lama yang disimpan di `packaging/icons/` dihapus.
  Tombol Quit di toolbar memakai glyph power dari
  `ui-assets/quit.svg` (gaya ikon tabler); glyph logout buatan sendiri dihapus.
  Tab `General` offers Show System Tray Icon,
  Minimize to Tray When Closing the Window and Open the Download Folder When a
  Download Completes; the tray icon can be switched off while the app runs, and
  closing the main window then ends the app (with a confirmation).
  Tab `Interface` offers toolbar size (Normal/Small),
  "Show Toolbar Text", app theme (Light/Dark/Follow System) and tray icon style
  (Simple/Colorful); the changes apply immediately after the settings dialog is
  confirmed. Warna yang sebelumnya hardcoded untuk tema gelap (ikon toolbar
  netral, status tabel, segment bar) mengikuti tema aktif.
  File konfigurasi dipisah per tab settings
  (`[general]`, `[download]`, `[interface]`, `[proxy]`); config lama dengan
  opsi unduhan di `[general]` tetap terbaca karena section `[general]`
  dibaca lebih dulu dan `[download]` menimpanya.
  Window Settings dibagi menjadi lima tab
  (`General`, `Interface`, `Download`, `Proxy/Socks`, `Passwords`); opsi
  unduhan pindah ke tab `Download`, tab `General` menyimpan opsi perilaku
  aplikasi, dan tab `Interface` menyimpan opsi tampilan.
  Dialog konfirmasi download memakai section `File` dan `Save` dengan judul
  bold, ukuran default 650×330 (minimum 560×320), nama file di-elide di tengah
  dengan nama penuh di tooltip, ukuran file tampil pada baris `Size` sendiri,
  dan semua field serta tombol memakai tinggi yang sama. `Cancel` ada di kiri,
  `Add to queue`/`Download Now` di kanan.
  Paket `.deb` tidak lagi mendeklarasikan browser sebagai `Recommends`.
  Autostart login menjalankan `swiftdm --autostart` sehingga aplikasi hanya
  muncul di system tray; tanpa tray, main window ditampilkan sebagai fallback.
  Semua message box memakai ikon aplikasi sendiri (filled) alih-alih ikon
  bawaan platform: info untuk Browser Extensions, question untuk konfirmasi
  quit, check untuk Download Complete, alert untuk semua peringatan.
- **Versi ekstensi Chromium**: 0.5.2 (`ext/chromium/manifest.json`), terpisah
  dari versi aplikasi. Popup mengikuti desain `ext/extension-ui-template.html`
  (referensi desain, tidak ikut dipaketkan).

### Sudah tersedia sampai 0.19.3

- Engine HTTP/HTTPS tersegmentasi via libcurl multi: resume, retry/backoff,
  proxy HTTP/SOCKS, cookie ter-scope, header kustom, POST body, dan validasi
  state resume.
- FTP (`ftp://`/`ftps://`) dengan probe `SIZE` dan resume `REST`.
- HLS/m3u8: pemilihan varian, `EXT-X-MAP`, unduhan segmen berurutan, resume;
  playlist terenkripsi ditolak dengan pesan jelas.
- Resolusi interstitial Google Drive dan ringkasan alasan penolakan host
  (JSON/HTML body) pada pesan error.
- CLI headless `swiftdm get` dan GUI Qt6 lengkap dengan tray, kategori,
  properties, konfirmasi download, queue, serta pemulihan riwayat dari SQLite.
- Named queues (0.19.0): workspace terpisah dari daftar utama, Queue
  Properties, scheduling dan batas simultan, limit kecepatan bersama,
  koneksi per queue, autostart, notifikasi, shutdown, pemindahan item, dan
  penghapusan queue. Sejak 0.19.1, cabang `Queues` menampilkan overview queue
  dan toolbar queue dengan urutan `New Queue`, `Delete Queue`, `Queue
  Properties` | `Start Queue`, `Stop Queue` | `New URL`, `Delete`,
  `Delete File`.
- Integrasi Chromium MV3: handoff otomatis/manual, cookie opsional, deteksi
  HLS dan pasangan video+audio, status push lewat WebSocket; socket command
  dipakai ulang untuk handoff berurutan selama masih terbuka.
- Dialog konfirmasi tampil saat probe berjalan dan memakai ulang hasil probe
  untuk transfer awal; resume selalu validasi ulang server.
- Packaging: aturan install CMake, `.deb` via CPack, desktop entry, AppStream,
  ikon hicolor (dirender saat build dari SVG ikon aplikasi), dan instalasi
  ekstensi.

### 0.18.1–0.19.3 — riwayat fitur

- 0.19.3 memisahkan aksi completion global dari queue: item queue tidak lagi
  membuka dialog `Download Complete` maupun folder tujuan, dan hanya memakai
  `Notify when an item completes` milik queue. Opsi global tersebut berlaku
  untuk daftar utama.
- 0.19.2 meminta user memilih queue lewat dialog `Choose Queue` saat `Add to
  queue` dipakai dan ada lebih dari satu queue; picker menampilkan nama +
  jumlah item dan tidak mempreseleksi apa pun. Satu queue langsung dipakai,
  download dari view queue memakai queue tersebut tanpa picker, dan row
  `Queue:` di dialog konfirmasi dihapus. Patch UX ini disetujui maintainer.
- 0.19.1 memilih cabang `Queues` langsung mengaktifkan toolbar queue dan
  menampilkan overview queue di panel kanan; queue-specific actions
  dinonaktifkan sampai satu queue dipilih. Toolbar queue memakai urutan `New
  Queue`, `Delete Queue`, `Queue Properties` | `Start Queue`, `Stop Queue` |
  `New URL`, `Delete`, `Delete File`, dan icon queue baru (`queue.svg`,
  `queue-add.svg`, `queue-delete.svg`, `queue-settings.svg`). `Move to
  Downloads` hanya ada di context menu item. Judul overview memakai warna teks
  tema aktif dengan font bold sedikit lebih besar, dan warna judul/empty-state
  di-refresh saat tema berubah agar terbaca di mode terang maupun gelap.
- 0.19.0 menambahkan named queues: tabel `queues` dan kolom
  `downloads.queueid` (migrasi additive), cabang `Queues` di sidebar,
  dialog `Queue Properties`, scheduling `Start Queue`/`Pause Queue` dengan
  urutan insertion, batas simultan (0 = Unlimited), koneksi per queue (0 =
  global), limit kecepatan total yang dibagi ke item aktif, autostart,
  notifikasi per item/all-complete, shutdown countdown, pemindahan item
  Downloads ↔ queue, dan hapus queue. Test baru `queue_logic` dan
  `queue_dialog`; `history`, `confirm_dialog`, dan `download_window`
  diperluas.
- Toggle "Enable speed limiter" global (`BandWidthLimitEnabled`) dan
  per-download (`bandwidthlimit_enabled` di history).
- `SpeedLimitEditor` bersama untuk Settings dan tab Options Download window:
  preset 500 KB/s, 1/5/10/20/50 MB/s, plus nilai kustom.
- Limit diterapkan pada jalur sequential (HTTP tanpa ukuran, FTP, HLS) lewat
  shared receive-rate gate; toggle/value dapat diubah live selama transfer.
- Migrasi kompatibel: config/DB lama dengan limit non-nol dianggap "aktif".
- Test `test_settings` dan `test_history` diperbarui; dokumentasi 0.18.1 sudah
  ditambahkan ke `CHANGELOG.md`, `README.md`, dan `docs/spec/`.
- 0.18.1–0.18.2 sudah di-commit pada branch `ux` (`52f89c6`); 0.18.3 menjaga
  main window tetap tersembunyi saat konfirmasi browser (sudah diverifikasi
  manual user). 0.18.4 meminta konfirmasi sebelum Quit menutup aplikasi.
- 0.18.5 mempertahankan percent-encoding URL browser/manual hingga libcurl;
  URL path ber-spasi dan nama fallback dari `%XX` kini ditangani. Verifikasi
  GUI folder Drive masih menunggu user.
- 0.18.6 menghapus dialog ringkasan file hilang saat startup; status
  `Complete (file missing)` tetap tampil di tabel dan popup hanya muncul saat
  Open File dipilih untuk file yang sudah tidak ada.
- 0.18.7 menambahkan `--autostart`: entry login XDG menjalankan flag tersebut
  dan aplikasi start silent di tray. Entry lama di-refresh saat GUI dijalankan
  dengan setting autostart aktif; tanpa system tray, window tetap ditampilkan.
- 0.18.8 menghapus `Recommends` Chromium dari paket Debian; instalasi tidak
  lagi menarik Chromium maupun rantai `system-config-printer`/`avahi-utils`.
  Ekstensi tetap opsional dan kompatibel dengan browser Chromium-based mana
  pun. Seluruh 0.18.1–0.18.8 sudah masuk `main` lewat merge `dd6b179`.
- 0.18.9 memperbaiki ukuran dialog konfirmasi download: ukuran tetap (default
  640×250, minimum 520×230) sehingga tidak lagi melebar mengikuti nama file
  atau URL, nama file di-elide di tengah dengan nama penuh di tooltip, dan
  ukuran file dipindah ke baris `Size` sendiri. Label deskripsi (`File :`,
  `Size :`, `URL :`, `Category :`, `Location :`) di-center vertikal terhadap
  widget di sebelahnya supaya semua baris sejajar, dan semua field serta
  tombol memakai tinggi yang sama. Test headless baru `confirm_dialog`
  menjaga perilaku ini.
- 0.18.10 membagi window Settings menjadi lima tab: `General` (opsi perilaku
  aplikasi), `Interface` (placeholder untuk opsi tampilan), `Download` (opsi
  unduhan yang sebelumnya ada di `General`), `Proxy/Socks`, dan `Passwords`.
  Test headless baru `settings_dialog` menjaga urutan tab dan penempatan
  setiap kontrol. Batch sisanya (opsi Interface, opsi General tambahan, dan
  pemisahan section INI) Planned sebagai 0.18.11–0.18.13.
- 0.18.11 memisahkan file konfigurasi per tab settings: `[general]`,
  `[download]`, `[interface]`, `[proxy]`. Opsi unduhan pindah ke `[download]`;
  config lama (opsi unduhan masih di `[general]`) tetap terbaca karena
  loader membaca `[general]` lebih dulu dan `[download]` menimpanya, tanpa
  bergantung urutan section di file. Key baru: `ShowTrayIcon`, `CloseToTray`,
  `OpenFolderAfterCompletion` (general) serta `Theme`, `ToolbarSize`,
  `ShowToolbarText`, `TrayIconStyle` (interface). Nilai interface yang tidak
  dikenal ditolak dan kembali ke default.
- 0.18.12 menambahkan tab `Interface`: toolbar size (Normal/Small), "Show
  Toolbar Text", app theme (Light/Dark/Follow System), tray icon style
  (Simple/Colorful). Perubahan langsung diterapkan saat dialog Settings
  dikonfirmasi. Tema terang memakai palet terang; mode "Follow System"
  memakai color scheme desktop dan mengikuti perubahannya saat aplikasi
  berjalan. Ikon toolbar netral, warna status tabel, dan segment bar ikut
  mengikuti tema aktif. Test headless baru `theme` menjaga nilai setelan,
  kedua palet, dan resolusi mode sistem.
- 0.18.13 menambah tiga opsi pada tab `General`: "Show System Tray Icon"
  (ikon tray bisa dimatikan saat aplikasi berjalan), "Minimize to Tray When
  Closing the Window" (jika dimatikan, menutup main window mengakhiri aplikasi
  dengan konfirmasi), dan "Open the Download Folder When a Download Completes".
- 0.18.14 mengganti ikon Quit di toolbar dengan glyph power dari
  `ui-assets/quit.svg`; glyph logout buatan sendiri di `src/ui/icons/quit.svg`
  dihapus. Perilaku tombol tidak berubah.
- 0.18.15 merender ikon launcher hicolor otomatis saat build dari
  `ui-assets/app-icon/main-icon.svg`; PNG lama di `packaging/icons/` dihapus
  sehingga satu SVG adalah sumber kebenaran.
- 0.18.16 mengganti ikon bawaan platform pada semua message box dengan ikon
  filled milik aplikasi (`ui-assets/info-filled.svg`, `allert-filled.svg`,
  `question-filled.svg`, `complete-filled.svg`); ikon hanya muncul di area ikon
  dialog, judul baris tetap memakai ikon aplikasi. Helper baru
  `dialog_info_pixmap()`, `dialog_warning_pixmap()`,
  `dialog_question_pixmap()`, `dialog_complete_pixmap()` di `src/ui/icons.*`.
- 0.18.17 menambahkan opsi "Show Download Confirmation Dialog" di tab
  `General` (key `ShowDownloadConfirmationDialog`, default 1). Saat dimatikan,
  `MainWindow::probe_and_confirm` melewati dialog konfirmasi dan memakai
  kategori bawaan serta lokasi default lewat helper
  `default_download_location()` di `src/ui/confirm_download_dialog.*`; window
  progress tetap dibuka. Note di bawah opsi hanya tampil saat opsi dimatikan.
- 0.18.18 memperbaiki error "File name too long" saat nama file dari URL atau
  `Content-Disposition` lebih panjang dari batas komponen path. Nama dipotong
  di `sanitize_filename()` (`src/core/util.*`) memakai helper baru
  `limit_filename_length()`: batas 240 byte UTF-8, extension (maks 16 byte)
  dipertahankan, stem dipotong pada batas UTF-8, dan hash FNV-1a 64-bit
  (16 hex) dari nama asli ditambahkan. Nama device Windows (`CON`, `PRN`,
  `AUX`, `NUL`, `COM1-9`, `LPT1-9`) disanitasi dengan prefix `_`.
  `make_workspace()` dan `unique_path()` juga menjamin komponennya masuk
  batas. Test baru `filenames`. Batas full path (259 karakter Windows) dan
  `longPathAware` ditunda ke milestone port Windows.
- 0.18.19 mengurangi latensi konfirmasi dan awal transfer: dialog tampil saat
  probe berlangsung, metadata diperbarui saat siap, dan hasil probe dipakai
  ulang untuk transfer awal (resume tetap probe ulang). Extension memakai ulang
  WebSocket command dan mencatat timing handoff. Test baru menjaga reuse probe,
  status dialog pending, dan reuse socket. Seri patch 0.18.x dipilih maintainer
  untuk batch ini meski mencakup perubahan UX.
- 0.18.20 menata ulang window Settings menjadi section berlabel di kelima tab
  (`General`: Downloads, Startup, System tray; `Interface`: Toolbar,
  Appearance, System tray; `Download`: Connections, Bandwidth, User-Agent,
  Files and folders; `Proxy/Socks`: Proxy type, Server, Authentication;
  `Passwords`: Saved passwords). Label memakai sentence case, spasi/punctuation
  lama dirapikan, dan hint "8 connections recommended" mengikuti warna link
  tema aktif. Ukuran dialog menjadi 720×580 dan minimumnya mengikuti layout
  section. Tidak ada key,
  default, atau perilaku yang berubah; test `settings_dialog` diperluas untuk
  menjaga judul/urutan section dan penempatan kontrol.
- 0.18.21 menyesuaikan tombol toolbar `Resume` dan `Stop` dengan status item
  terpilih: Resume nonaktif untuk item `Downloading`, Stop nonaktif untuk
  `Paused`/`Error`/`Complete`, dan seleksi campuran menyalakan tombol yang
  memiliki minimal satu item cocok. `on_resume()`/`on_stop()` memakai helper
  status yang sama dengan tombol. Toolbar diberi divider setelah `New URL` dan
  setelah `Stop`. Test headless baru `action_state`.
- 0.18.22 menata ulang window Download Properties menjadi empat section
  berlabel: `General` (file size, status, category, downloaded, resumable,
  segments), `Location` (Saved to + Open Folder, Temp path), `Source` (URL,
  Page URL/Page title/Pair bila ada) dan `Time` (Added on, Last try). Judul
  section bold; label sentence case. Nilai pendek memakai label yang bisa
  diseleksi, path/URL tetap read-only field. Nama file panjang di-elide di
  tengah dengan tooltip. Default window 650×690 dan minimum mengikuti layout.
  Test headless baru `properties_dialog`. Window progress ikut memakai section
  bold: tab Download (`Details`, `Progress`), tab Options (`Destination`,
  `Transfer`) dan tab Connections (tabel dalam satu frame). Tab Download
  menampilkan persentase di dalam progress bar dan `Segments: N`; tombol
  Resume/Cancel rata kanan. Window progress dibuka 650×500 (minimum 520×420),
  dan speed-limit editor memakai dua baris (toggle + preset, lalu nilai
  kustom). Test headless baru `download_window`. Window Download Confirmation
  juga memakai section bold: `File` (Name, Size) dan `Save` (URL, Category,
  Location), dibuka 650×330 (minimum 560×320). Tombol disusun `Cancel` di
  kiri, `Add to queue` dan `Download Now` di kanan. Test `confirm_dialog`
  diperluas untuk section, penempatan field, urutan tombol, dan hint queue.

### Rencana / gap yang diketahui

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
| `src/store/` | `Settings` (INI) dan `HistoryStore` (SQLite: `downloads`, `headers`, `auths`, `queues`). |
| `src/ui/` | Qt6 Widgets: `MainWindow`, model/filter, `DownloadTask` (worker thread), dialog-dialog, `queue_logic`/`queue_dialog`, tema/ikon, dan `BrowserIntegration` (WebSocket loopback). |
| `ext/chromium/` | Ekstensi Chromium Manifest V3 (popup, service worker, cookie permission, ikon). |
| `tests/` | Unit/integration test backend, server HTTP/FTP lokal, harness Node untuk logika ekstensi, test ikon offscreen, dan test protokol browser. |
| `docs/spec/` | Spesifikasi perilaku clean-room, data model, engine, protokol, settings, parity checklist, referensi ekstensi, evidence ledger. |
| `packaging/` | Desktop entry, AppStream metadata, maintainer script Debian, dokumentasi packaging. Ikon launcher hicolor dirender saat build ke `build/generated/icons/`. |
| `tools/` | Rasterizer ikon ekstensi dan ikon desktop. |
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

Menjalankan aplikasi:

```sh
./build/swiftdm                  # GUI
./build/swiftdm --version
./build/swiftdm get <url>        # CLI headless
```

Test yang tersedia: `core_version`, `plan`, `settings`, `history`, `restore`,
`media`, `filenames`, `http`, `extension_logic` (butuh Node), `icons` (offscreen),
`confirm_dialog` (offscreen), `settings_dialog` (offscreen),
`properties_dialog` (offscreen), `download_window` (offscreen), `queue_logic`,
`queue_dialog` (offscreen), `action_state`, `theme` (offscreen), dan
`browser_protocol` (tanpa GUI). Test HTTP/FTP memakai
server lokal di `tests/http_test_server.h` dan `tests/ftp_test_server.h`.

Regenerasi ikon setelah mengubah SVG:

- Ikon launcher (hicolor) dirender otomatis saat build dari
  `ui-assets/app-icon/main-icon.svg`; hasilnya ada di
  `build/generated/icons/hicolor/` dan ikut ter-install.
- Ikon aksi ekstensi Chromium masih manual:

```sh
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
`CHANGELOG.md` versi tersebut.

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
- Workflow release membuat release sebagai draft, mengunggah `.deb`, lalu
  mempublikasikannya. Re-run workflow akan memakai release yang sudah ada dan
  mengganti asset `.deb` (`--clobber`).
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
