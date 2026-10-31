## 2. Status pengembangan (snapshot)

> Snapshot terakhir: **2026-10-31**, versi kerja **0.22.4**. Status ini cepat
> berubah. Verifikasi ulang dengan git dan `CMakeLists.txt`; jika berbeda,
> percayai kondisi repositori dan perbarui bagian ini pada commit berikutnya.

- **Branch kerja saat snapshot**: `main`, sudah memuat port Windows 10
  eksperimental yang sebelumnya dikerjakan di branch `windows` (merge
  fast-forward ke 0.19.15).
- **Rilis terakhir di jalur `main`**: 0.22.4 (progress dan kecepatan dikirim ke
  UI tiap 50 ms). Sebelumnya 0.22.3 menyesuaikan UI/UX jendela *Refresh download
  URL*: teks, ukuran compact, nama field, dan tombol yang lebih jelas.
- **Versi kerja**: 0.22.4. Progress download dilaporkan ke UI tiap 50 ms
  (kecepatan tetap dihaluskan per jendela 200 ms), jadi tabel dan window
  progress menyegar maksimal 20 Hz. Refresh download URL (0.22.0, diperketat
  0.22.1–0.22.3):
  item menu klik kanan `Refresh Download URL` pada baris Paused/Error yang punya
  history menandai unduhan itu sebagai menunggu URL baru, membuka `page_url` di
  browser (atau meminta user memulai unduhan sendiri bila tidak ada page URL),
  lalu menampilkan jendela compact always-on-top `Refresh Download URL` berisi
  `File Name:`, `File Size:`, dan tombol `Cancel` (menutup jendela juga
  membatalkan). Jendela itu disembunyikan selama dialog konfirmasi tampil dan
  muncul lagi bila user memilih `Cancel`. Selama menunggu, **setiap** capture
  dari ekstensi browser diarahkan ke dialog `Confirm New Download URL` berisi
  `File Name:` dan `New URL:` dengan tombol `Use New URL` / `Cancel` — nama
  berkas yang tidak cocok menampilkan peringatan berwarna merah dan default ke
  `Cancel` — dan capture yang ditolak tidak pernah menjadi unduhan baru;
  `New URL` diblokir dengan peringatan yang menawarkan `Cancel Refresh`.
  `Use New URL` memindahkan URL, cookie, referer, user agent, dan header baru ke
  baris yang sama, mengganti nama folder workspace ke key URL baru, dan menulis
  ulang `segments.json` (URL baru, validator etag/last-modified dikosongkan)
  sehingga engine melanjutkan dari byte yang sudah ada selama ukuran berkas masih
  cocok. Signal capture di-queue agar balasan WebSocket terkirim sebelum jendela
  modal dibuka, dan capture yang mengulang URL yang baru diterapkan diabaikan
  selama 15 detik, sehingga retry perintah dari ekstensi tidak memunculkan
  konfirmasi ganda (0.22.2). Hanya satu unduhan yang boleh menunggu pada satu
  waktu dan status baris tampil `Waiting for new URL…` (amber). Aturan routing
  capture, guard `New URL`, dan guard echo adalah fungsi murni yang diuji test
  headless `refresh_logic`.
- **Port Windows 10** eksperimental ada di `main` dan
  sudah berhasil dibangun serta lulus seluruh `ctest` (termasuk
  server uji HTTP/FTP/HLS versi Winsock) dengan MSYS2 UCRT64 (GCC 16.2,
  Qt 6.12, curl 8.23, SQLite 3.54). Lapisan file I/O lintas platform (`core/file_io.*`),
  dukungan path panjang `\\?`, argumen command line UTF-8, jalur data
  `%APPDATA%`/`%LOCALAPPDATA%`, autostart registry, paket ZIP mandiri
  (windeployqt + `tools/deploy_msys_runtime.sh`), installer Inno Setup per-user
  (`cmake --build build --target windows-installer`), dan CA bundle
  `ca-bundle.crt` (libcurl MSYS2 default-nya memakai path POSIX yang tidak ada
  di Windows, sehingga HTTPS gagal error 77; kini `CURLOPT_CAINFO` menunjuk ke
  bundle di samping exe, dengan override env `SWIFTDM_CA_BUNDLE`) sudah ada.
  Sejak 0.19.14 `resize_file()` memperpanjang part file lewat
  `SetEndOfFile()` sehingga prealokasi tidak lagi menulis nol fisik
  (`_chsize_s()` sebelumnya memblokir unduhan besar sebelum byte pertama tiba).
  Sejak 0.19.15 progress dilaporkan tiap 100 ms dan kecepatan disampel per
  jendela 200 ms; sejak 0.22.4 interval report dipangkas ke 50 ms, jadi tabel
  serta window progress menyegar maksimal 20 Hz tanpa timer UI tersendiri.
  Sejak 0.19.18 `swiftdm.exe` membawa resource ikon aplikasi (7 ukuran,
  16–256 px, dari `ui-assets/app-icon/main.ico`) dan installer Inno Setup
  memakai berkas `.ico` yang sama lewat `SetupIconFile`, sehingga shortcut,
  taskbar, dan uninstaller tidak lagi memakai ikon bawaan Windows.
  Sejak 0.19.19 redirect lintas-origin pada probe dan `fetch_text` membuang
  header kustom, `Referer`, dan POST body sebelum request ke origin baru, lalu
  transfer memakai URL hasil resolve. Unduhan rilis GitHub dari ekstensi tidak
  lagi gagal dengan pesan `redirect blocked`.
  Yang masih terbuka: verifikasi GUI manual di Windows 10 (tray, dialog
  konfirmasi, pause/resume, autostart) dan code signing (installer serta exe belum
  ditandatangani sehingga SmartScreen masih memperingatkan).
  Platform acuan dan rilis stabil tetap Debian 13/Linux.
- **Audit hardening yang ikut masuk (0.19.4–0.19.11)**: nama file final diklaim
  atomik (tanpa menimpa file lain, memakai hard link dan fallback salin
  eksklusif di Windows); nama berawalan titik diberi awalan `_` dan akhiran
  `.desktop` dinetralkan; URL dengan userinfo ditolak di jalur CLI juga; entri
  autostart mengutip path `Exec`; resume mengirim `If-Range` dan menolak
  `Content-Range` yang tidak cocok; redirect lintas-origin diblokir bila request
  membawa header kustom atau Referer. Origin WebSocket hanya menerima ID
  ekstensi dari manifest proyek, dan handshake yang menggantung ditutup setelah
  lima detik. URL transfer dibatasi ke `http/https/ftp/ftps`; form konfirmasi
  interstitial hanya diikuti pada origin penyaji atau handoff antardomain Google
  Drive; config atomik mode `0600`, DB `0600`, direktori config/data/temp/
  workspace privat `0700`, dan file parsial `0644` di dalam workspace privat.
  Password proxy masih plaintext dan integrasi Secret Service/catatan UI
  ditunda. Item queue tidak lagi memicu aksi completion global: dialog
  `Download Complete` dan opsi buka folder hanya berlaku untuk daftar utama,
  sedangkan item queue memakai `Notify when an item completes` milik queue.
  Sebelumnya (0.19.2) `Add to queue` pada Download Confirmation tidak lagi
  memakai queue pertama secara diam-diam: bila ada lebih dari satu queue,
  tombol membuka dialog `Choose Queue` berisi nama + jumlah item, tanpa
  preseleksi. Satu queue langsung dipakai, dan download yang dimulai dari view
  queue tetap menarget queue itu tanpa picker. Row `Queue:` di dialog
  konfirmasi dihapus dan tinggi window kembali ke 650×330 (minimum 560×320).
  Perubahan UX 0.19.2 memakai patch atas keputusan maintainer, meskipun
  kebijakan normal untuk UX adalah MINOR +1.
- **Named queues (0.19.0)**: cabang `Queues` di sidebar, tabel `queues` + kolom
  `downloads.queueid`, dialog `Queue Properties` (max simultaneous 0 =
  Unlimited, connections 0 = global, limit kecepatan total queue, autostart,
  notifikasi, shutdown dengan delay), `Start Queue`/`Stop Queue` dengan urutan
  insertion, pemindahan item antara Downloads dan queue, hapus queue, notifikasi
  all-complete, dan shutdown countdown yang bisa dibatalkan. Memilih cabang
  `Queues` langsung mengaktifkan mode toolbar queue dan menampilkan overview
  queue (nama + jumlah item, atau prompt `New Queue` bila kosong); memilih queue
  dari overview membuka daftar unduhannya. Urutan toolbar queue: `New Queue`,
  `Delete Queue`, `Queue Properties` | `Start Queue`, `Stop Queue` | `New URL`,
  `Delete`, `Delete File`.
- **Engine HTTP/HTTPS tersegmentasi via libcurl multi**: resume, retry/backoff,
  proxy HTTP/SOCKS, cookie ter-scope, header kustom, POST body, dan validasi
  state resume.
- **FTP (`ftp://`/`ftps://`)** dengan probe `SIZE` dan resume `REST`.
- **HLS/m3u8**: pemilihan varian, `EXT-X-MAP`, unduhan segmen berurutan, resume;
  playlist terenkripsi ditolak dengan pesan jelas.
- **Resolusi interstitial Google Drive** dan ringkasan alasan penolakan host
  (JSON/HTML body) pada pesan error.
- **CLI headless `swiftdm get`** dan GUI Qt6 lengkap dengan tray, kategori,
  properties, konfirmasi download, queue, serta pemulihan riwayat dari SQLite.
- **Named queues (0.19.0)**: workspace terpisah dari daftar utama, Queue
  Properties, scheduling dan batas simultan, limit kecepatan bersama,
  koneksi per queue, autostart, notifikasi, shutdown, pemindahan item, dan
  penghapusan queue. Sejak 0.19.1, cabang `Queues` menampilkan overview queue
  dan toolbar queue dengan urutan `New Queue`, `Delete Queue`, `Queue

  Properties` | `Start Queue`, `Stop Queue` | `New URL`, `Delete`,
  `Delete File`.
- **Integrasi Chromium MV3**: handoff otomatis/manual, cookie opsional, deteksi
  HLS dan pasangan video+audio, status push lewat WebSocket; socket command
  dipakai ulang untuk handoff berurutan selama masih terbuka.
- **Dialog konfirmasi tampil saat probe berjalan** dan memakai ulang hasil probe
  untuk transfer awal; resume selalu validasi ulang server.
- **Packaging**: aturan install CMake, `.deb` via CPack, desktop entry, AppStream,
  ikon hicolor (dirender saat build dari SVG ikon aplikasi), dan instalasi
  ekstensi.
- **Struktur modul (0.19.12)**: `src/net/http.cpp` dipecah menjadi
  `http_request`/`http_error`/`http_probe`/`http_segment`/`http_stream` dengan
  header internal `net/http_internal.h`, dan `src/ui/main_window.cpp` dipecah
  menjadi unit `main_window_*`. `net/http.h` tetap satu-satunya antarmuka publik
  lapisan net.
- **Port Windows (branch `windows`)**: file I/O lintas platform `core/file_io.*`
  (termasuk flag `FileExclusive` dan `read_file`), path panjang `\\?`,
  argumen UTF-8, `sqlite3_open16`, autostart registry, paket ZIP mandiri,
  `ca-bundle.crt`, dan target CPack ZIP.

### Rencana / gap yang diketahui

- URL yang dapat diedit di Properties. (Per-download log file selesai di 0.20.0:
  `logs/<id>-<filename>.log`, dibuka dari `Open Log`.
- UI saved passwords (`auths`); tab `Passwords` sengaja belum ditampilkan.
- Ekstensi Firefox.
- Muxing MKV/WebM untuk pasangan video+audio (saat ini dua file terpisah
  dengan `groupid`).
- In-page media panel dan site extractor (C4–C6, C9 di
  `docs/spec/07-chromium-extension-reference.md`).
- Verifikasi GUI selalu manual oleh user; tidak ada automation GUI.
- Windows: verifikasi GUI manual di Windows 10 (tray, dialog konfirmasi,
  pause/resume, autostart) belum dilakukan. Sejak 0.19.17 executable GUI
  memakai subsistem GUI dan mode CLI meminjam console induk lewat
  `AttachConsole`, jadi membuka aplikasi dari Explorer tidak lagi memunculkan
  jendela console. Installer (MSI/NSIS) dan code signing belum ada; paket masih
  ZIP + installer Inno Setup.
