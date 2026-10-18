# AGENTS.md — Aturan Kerja untuk Agen AI

Berlaku untuk seluruh repositori ini.

## 1. Kebijakan Versi (WAJIB)

Format versi: `MAJOR.MINOR.PATCH`, dengan satu sumber kebenaran di file build:

- `CMakeLists.txt` → `project(SwiftDM VERSION <MAJOR>.<MINOR>.<PATCH>)`
- Versi dipropagasikan otomatis lewat `configure_file` ke header versi
  (`src/app/version.h`, di-generate saat build; tidak boleh diedit manual).
  Versi dipakai untuk flag CLI `--version`, judul window, dan dialog About.

Aturan kenaikan versi pada setiap perubahan aplikasi:

1. **Perubahan major** — fitur baru, penambahan/perubahan fungsi UI, atau
   perubahan UX → naikkan MINOR +1 (`+0.1.0`) dan reset PATCH ke 0.
   Contoh: `0.3.2` → `0.4.0`.
2. **Perubahan minor** — perbaikan bug, refactor, penyesuaian kecil tanpa
   fitur/fungsi UI/UX baru → naikkan PATCH +1 (`+0.0.1`).
   Contoh: `0.4.0` → `0.4.1`.
3. Setiap perubahan perilaku aplikasi wajib disertai kenaikan versi yang
   sesuai dan catatan changelog singkat di commit/PR.

## 2. Pengujian oleh Agen AI (WAJIB)

4. Agen AI DILARANG melakukan pengujian aplikasi secara otomatis melalui UI
   (GUI automation, klik otomatis, screenshot otomatis, xdotool, dsb.).
5. Agen AI HANYA boleh menguji lewat CLI/backend:
   - unit test dan integration test tanpa GUI,
   - menjalankan binary dengan flag CLI (mis. `--version`, mode headless
     engine bila tersedia),
   - pengujian engine/jaringan/DB secara langsung (server uji lokal).
6. Bila pengujian UI diperlukan, agen AI tidak melakukannya sendiri. Agen
   harus: (a) memberi tahu user bagian UI mana yang perlu diuji beserta
   langkah singkatnya, lalu (b) menunggu feedback user sebelum melakukan
   perbaikan/penyempurnaan.

## 3. Konteks Proyek

- Proyek ini adalah reimplementasi **clean-room** untuk Linux dari aplikasi
  Neat Download Manager (Windows). Detail perilaku asal ditulis sebagai
  spesifikasi di `docs/spec/` sebelum implementasi.
- Dilarang menyalin kode, aset, ikon, atau branding dari aplikasi asal.
- `binary-original/` hanya untuk analisis (read-only), tidak boleh diubah
  dan tidak boleh ikut terdistribusi.
- Fitur "phone-home" (update check ke server aplikasi asal) tidak
  direplikasi.
