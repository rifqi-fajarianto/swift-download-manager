# SwiftDM

SwiftDM adalah aplikasi pengelola unduhan desktop untuk Linux. Aplikasi ini
mendukung unduhan bersegmen, jeda dan lanjutkan unduhan, serta penggunaan lewat
antarmuka grafis maupun command line. SwiftDM dibuat dengan C++20, Qt 6,
libcurl, dan SQLite.

Proyek ini merupakan implementasi clean-room berdasarkan spesifikasi perilaku
yang didokumentasikan di [`docs/spec/`](docs/spec/). Kode, aset, ikon, dan
branding tidak diambil dari aplikasi yang menjadi referensi analisis.

## Tangkapan layar

![Jendela utama SwiftDM](screenshots/main-window.png)

| Konfirmasi unduhan | Progres unduhan | Segmen koneksi |
|---|---|---|
| ![Dialog konfirmasi unduhan](screenshots/download-confirm.png) | ![Jendela progres unduhan](screenshots/download-progress.png) | ![Tab koneksi dan segmen](screenshots/download-segment.png) |

| Opsi unduhan | Properti unduhan | Pengaturan aplikasi |
|---|---|---|
| ![Opsi unduhan](screenshots/download-options.png) | ![Properti unduhan](screenshots/download-properties.png) | ![Pengaturan SwiftDM](screenshots/options.png) |

## Fungsi dan fitur

- Mengunduh melalui HTTP, HTTPS, FTP, FTPS, dan playlist HLS (`.m3u8`).
- Membagi unduhan HTTP/HTTPS menjadi beberapa koneksi; jumlah koneksi dapat
  diatur.
- Menjeda, melanjutkan, dan memulihkan unduhan setelah aplikasi dibuka kembali.
- Mengatur kategori dan folder tujuan, serta melihat progres, kecepatan, waktu
  tersisa, dan koneksi/segmen aktif.
- Mengatur batas kecepatan global dan per unduhan, termasuk mengubah batas
  unduhan yang sedang berjalan.
- Menggunakan proxy HTTP, SOCKS4, atau SOCKS5; mengatur header HTTP, cookie,
  referer, User-Agent, dan data POST.
- Menyimpan riwayat unduhan di SQLite dan pengaturan aplikasi dalam file INI.
- Mengunduh dari command line tanpa membuka GUI.
- Integrasi pratinjau dengan ekstensi Chromium: kirim tautan atau halaman,
  intersepsi unduhan HTTP(S), dan deteksi playlist/pasangan media.
- Resolusi format tautan unduhan Google Drive yang didukung.

> Catatan: penjadwalan antrean, ekstensi Firefox, dan penggabungan pasangan
> trek audio/video belum tersedia. Ekstensi Chromium masih berstatus pratinjau
> pengembangan. Lihat [parity checklist](docs/spec/06-parity-checklist.md)
> untuk status dan batasan fitur yang lebih lengkap.

## Menjalankan

Setelah SwiftDM terpasang, jalankan aplikasi grafis dari menu aplikasi atau
dengan perintah:

```sh
swiftdm
```

Untuk menjalankan langsung dari hasil build, gunakan `./build/swiftdm`.
Antarmuka grafis terbuka secara default. Menambahkan URL melalui GUI akan
memeriksa tautan terlebih dahulu dan menampilkan konfirmasi sebelum unduhan
dimulai.

SwiftDM juga menyediakan CLI headless:

```sh
swiftdm get <url>
```

Contoh dengan folder tujuan, nama file, dan batas kecepatan 1.5 MB/s:

```sh
swiftdm get 'https://example.org/file.zip' \
  --dir ~/Downloads --name file.zip --limit 1500
```

Opsi CLI lainnya mencakup `--connections`, `--no-resume`, `--referer`,
`--header`, `--ua`, `--post`, `--config`, `--quiet`, dan `--log-level`. Untuk
daftar lengkap:

```sh
swiftdm --help
swiftdm --version
```

## Build dari source

Platform acuan adalah Debian 13 (trixie). Pasang dependency berikut:

```sh
sudo apt install build-essential cmake ninja-build \
  qt6-base-dev qt6-svg-plugins libqt6svg6 \
  libcurl4-openssl-dev libsqlite3-dev
```

`qt6-svg-plugins` diperlukan agar ikon SVG dapat dimuat saat aplikasi
dijalankan.

Konfigurasi dan build:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Jalankan test backend/unit dan integrasi:

```sh
ctest --test-dir build --output-on-failure
```

Jalankan hasil build:

```sh
./build/swiftdm                 # GUI
./build/swiftdm get <url>       # CLI headless
```

### Instalasi

Buat paket Debian menggunakan CPack:

```sh
cd build
cpack
sudo apt install ./swiftdm_0.18.5_amd64.deb
```

Atau pasang menggunakan aturan instalasi CMake:

```sh
sudo cmake --install build --prefix /usr/local
```

Paket memasang executable, launcher desktop, ikon, metadata AppStream, dan
sumber ekstensi Chromium. Panduan packaging lebih rinci tersedia di
[`packaging/README.md`](packaging/README.md).

## Data aplikasi

Secara default, SwiftDM menyimpan konfigurasi di
`~/.config/swiftdm/swiftdm.ini`, riwayat di `~/.local/share/swiftdm/swiftdm.db`,
dan workspace unduhan sementara di `~/.local/share/swiftdm/tmp`. Lokasi tersebut
mengikuti variabel lingkungan XDG.

## Lisensi dan atribusi

SwiftDM dilisensikan di bawah [Apache License 2.0](LICENSE).

Sebagian ikon antarmuka pengguna menggunakan **Tabler Icons** oleh Tabler.
Tabler Icons dilisensikan di bawah MIT License. Terima kasih kepada para
kontributor Tabler atas ikon-ikon tersebut.

- Situs: <https://tabler.io/icons>
- Repositori dan teks lisensi: <https://github.com/tabler/tabler-icons>
- Lisensi Tabler Icons: <https://github.com/tabler/tabler-icons/blob/main/LICENSE>

## Dokumentasi

- [Spesifikasi dan status fitur](docs/spec/06-parity-checklist.md)
- [Panduan ekstensi Chromium](ext/chromium/README.md)
- [Panduan packaging](packaging/README.md)
- [Changelog](CHANGELOG.md)
