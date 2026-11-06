# Laporan Analisis REA — Fitur "Refresh download address" pada Internet Download Manager

| | |
|---|---|
| **Target** | `app/IDMan.exe` — Internet Download Manager 6.41.x, PE32 (x86), Delphi/Windows |
| **SHA-256** | `c3b99b8812bb6168794582bc1afcac9bd8dcd1cc61b3c23c6a56b6339c62d45b` |
| **Engine** | Ghidra 12.1.4 (headless) via REA `rea-agents@6.5.0` |
| **Metode** | Analisis statis artifact terkirim (`shipped-artifact`) — **tidak ada eksekusi** |
| **Status** | Alur utama terpetakan; 4 pertanyaan tersisa tercatat di `notes/04-open-questions.md` |

---

## 1. Jawaban singkat

**"Refresh download address" tidak membuat URL baru.** IDM tidak pernah menandatangani ulang
atau menghitung ulang URL sementara milik server. Yang dilakukan IDM adalah memindahkan
**alamat baru dan data sesi baru yang diterima dari browser** ke record unduhan yang lama, lalu
melanjutkannya dari byte yang sudah terunduh.

Alur tersebut bekerja sebagai berikut:

```
klik kanan di daftar unduhan  ->  "Refresh download address"
        |
        v
IDM menandai unduhan itu sebagai "expired"  (hanya satu boleh menunggu)
        |
        v
IDM membuka kembali di browser HALAMAN PEMILIK unduhan itu
        |
        v
user memulai unduhan yang sama lagi dari browser
        |
        v
integrasi browser IDM (idmnmcl.dll / IDMIntegrator64.exe) menangkap permintaan baru
        |
        v
IDM mencocokkan capture baru dengan unduhan yang menunggu
        |  (tanya user: "Is this the new address for your expired download?")
        v
IDM menulis URL + data sesi baru ke record unduhan lama  ->  unduhan dilanjutkan
```

Ada satu jalur tambahan: ketika **server** yang menyatakan session/alamat sudah invalid, IDM
menawarkan menu item yang sama, dan IDM juga bisa **mengunduh ulang halaman pemilik sendiri**
untuk mencari tautan pengganti (string 20878).

---

## 2. Terminologi

| Istilah | Arti |
|---|---|
| **Owner page / owner web page** | Halaman tempat unduhan pertama kali ditangkap. Disimpan pada record unduhan di offset `+0x144`. Di UI: *"The web page which this file was obtained from"* (string 1741). |
| **Capture** | Permintaan unduhan yang diserahkan integrasi browser ke IDM (URL baru + header/cookie/referer). |
| **Expired download** | Unduhan yang URL/session server-side-nya sudah tidak valid. |
| **Record unduhan** | Struktur data unduhan di `IDMan.exe`; field penting: `+0x20` (objek URL, berisi beberapa varian string di `+0x18`/`+0x1c`), `+0x38`, `+0x3c`, `+0x144` (owner page), `+0x148` (nama berkas), `+0x174`, `+0xe8`, `+0x194`, `+0x1a4`, `+0x1c0`. |

---

## 3. Titik masuk UI

Fitur ini adalah item pada **menu klik-kanan di daftar unduhan IDM**.

* Item dibuat di `0x468B0E`:
  `push <"Refresh download address"> ; push 0x2B40 ; push 1 ; push <popup menu> ; call <menu add>`
  → **command ID `0x2B40` (11072)**, label = string lokalisasi **20872**.
* Item tetangga di menu yang sama (dipastikan lewat `analysis/tools/idm_string_map.py`):

  | ID menu | String | Teks |
  |---|---|---|
  | `0x271D` | 20329 | *Resume Download* |
  | `0x271B` | 32774 | *Stop Download* |
  | **`0x2B40`** | **20872** | ***Refresh download address*** |
  | `0x271C` | 1050 | *Remove* |
  | `0x8016` | 20356 | *Add to download queue* |
  | `0x2724` | 20357 | *Delete from queue* |

* Item dinonaktifkan/diaktifkan dengan `EnableMenuItem(menu, 0x2B40, flag)` di `0x457017`
  dan `0x4572C3`, memakai flag yang sama dengan item `0x271D`.
* Perintah didispatch lewat tabel 12-byte di `.rdata 0x68B680`
  (`id_lo, id_hi, 0x39, handler, WM_COMMAND`); record fitur ini berada di `0x68B6C8` dan
  menunjuk ke handler **`0x459D40`**.

---

## 4. Handler menu `0x459D40` (REA Evidence `analyze_function`)

Evidence ID: `ev_f3f6a2f40ba4e2a2039d6cc83ad2a3c3e2a61df7355d13ce31c6a9419202308c`
(prosedur `FUN_00459d40`, badan `0x459D40–0x459DC8`, 137 byte).

Pseudocode Ghidra yang diperoleh lewat REA:

```c
void FUN_00459d40(void)
{
  ...
  local_18 = FUN_00455b10();                       /* list punya seleksi? */
  if (local_18 != 0) {
    iVar1 = FUN_00455b30(&local_18);              /* index item terpilih */
    uVar2 = CListCtrl::GetItemData((CListCtrl *)(in_ECX + 0x5e8), iVar1);
    FUN_004015b0(0x111, 0x1469, uVar2);           /* WM_COMMAND, wParam=0x1469 */
  }
  ...
}
```

Artinya handler menu **tidak melakukan apa-apa selain meneruskan** ke form daftar unduhan
sebagai perintah internal **`0x1469` (5225)**.

---

## 5. Cabang `0x1469` di form daftar unduhan

Evidence ID: `ev_ef4ab7e810ae449805d60f2c182637781d6a7729317d8785c202d3a2e1c09564`
(prosedur `FUN_0048fdb0`, badan `0x48FDB0–0x49AB8F`).

`0x1469` dibandingkan di tiga tempat dalam prosedur besar form daftar unduhan:
`0x4941F3`, `0x4942D3`, `0x494412`. Switch perintah pada prosedur ini memuat kasus
`0x1461`, `0x1462`, `0x1463`, **`0x1467`**, **`0x1469`**.

### 5.1 Eksklusivitas mutual (string 20873)

```asm
4941e0: cmp  DWORD PTR [esi+0x370],0x0      ; sudah ada unduhan yang menunggu?
4941e7: jle  4942c1
4941ed: cmp  DWORD PTR [ebp+0x220d0],0x1469 ; perintah kita?
4941f7: jne  494231
4941f9: push  0x0
4941fb: mov   edx,[0x76759c]                ; judul: "Refresh download address" (20872)
494201: push  edx
494202: mov   eax,[0x7675a0]                ; 20873 "…hanya satu unduhan expired boleh memakai fitur ini"
494207: push  eax
494208: call  [GetDesktopWindow]
49420e: push  eax
49420f: call  0x475430                      ; MessageBox
```

⇒ Kalau ada unduhan lain yang sedang menunggu, IDM **menolak** dengan pesan 20873.

Pseudocode Ghidra/REA (potongan verbatim dari `FUN_0048fdb0`):

```c
case (undefined1 *)0x1463:
case (undefined1 *)0x1469:
  iVar10 = FUN_00460b70();                 /* index unduhan terpilih      */
  piVar13 = (int *)FUN_004d2c80();         /* daftar unduhan              */
  iVar10 = *piVar13;                       /* record unduhan              */
  FUN_005bd6a0();                          /* nama berkas                 */
  if (*(int *)(in_ECX + 0x370) < 1) {      /* belum ada yang menunggu     */
    *(undefined4 *)(in_ECX + 0x370) = *(undefined4 *)(iVar10 + 0x508);   /* tandai */
    if (param_1 == (undefined1 *)0x1469) { FUN_005d5bd0(); }            /* catat id */
    ...
  }
  else {
    if (param_1 == (undefined1 *)0x1469) {
      uVar15 = DAT_007675a0;               /* 20873 */
      pHVar9 = GetDesktopWindow();
      FUN_00475430(pHVar9, uVar15);         /* judul "Refresh download address" */
      return;
    }
    ...
  }
```

### 5.2 Menandai unduhan yang menunggu

```asm
4942c1: mov  ecx,[ebx+0x508]     ; id unduhan
4942c7: mov  [esi+0x370],ecx     ; => field "unduhan yang menunggu"
4942d9: mov  edx,[ebx+0x508]
4942e0: lea  ecx,[ebx+0x20]
4942e3: call 0x5d5bd0           ; catat id pada record unduhan
```

### 5.3 Menentukan halaman pemilik & membuka browser

```asm
494475: mov  eax,[ebx+0x144]     ; owner page
       … strlen …
494489: mov  eax,0x7675a4        ; 20874 "IDM will open a web page in your browser …"
49448e: jne  494495
494490: mov  eax,0x7675ac        ; 20876 "Please start the download … manually"
```

* Ada owner page → pesan **20874** (IDM yang membuka halamannya).
* Tidak ada owner page → pesan **20876** (user harus memicu unduhan sendiri).

Pesan ditampilkan lewat `MessageBox` (`0x475890`) dan jawaban dicek:

```asm
494576: cmp  eax,0x1             ; IDOK
494579: je   4945a1
49457b: mov  DWORD PTR [esi+0x370],0   ; <-- BATAL: buang status menunggu
```

Jika **OK**, halaman pemilik dibuka di browser default:

```asm
4945a1: mov  ecx,[ebp-0x44]      ; &ownerPage
4945a4: mov  edx,[ecx]
       … strlen …
4945bb: push edx                 ; URL
4945bc: call [GetDesktopWindow]  ; hWnd
4945c2: push eax
4945c3: call 0x4b7b10            ; buka URL (helper ShellExecute)
```

### 5.4 Membuat panel "menunggu data baru"

```asm
4945e8: push 0x94
4945ed: call 0x425e60            ; instantiate kelas panel
494616: call 0x4ffb70            ; bangun panel (hwnd, judul, teks)
494623: mov  [esi+0x17e8],eax    ; simpan objek panel
49463b: push 0x158               ; pesan kustom 344
494648: call edx                 ; panel->WndProc
49464a: push 0x5
494652: call 0x5faa2a            ; tampilkan panel
```

Tiga field status pada form daftar unduhan:

| Field | Arti |
|---|---|
| `[esi+0x370]` | id unduhan yang sedang menunggu alamat baru (`0` = tidak ada) |
| `[esi+0x17e8]` | objek panel "menunggu" |
| `[esi+0x17ec]` | flag `1` selama proses refresh berjalan (`0x494430`) |

### 5.5 Jalur otomatis (server menyatakan session expired)

Bila perintah **bukan** `0x1469` (yaitu IDM sendiri yang menemukan address/session expired),
jalur yang sama tetap dijalankan, hanya pesannya lebih lengkap:

```asm
49443c: … 20867 "Cannot resume downloading the file:" + nama berkas
        … 20869 "It's possible that the address of the file is not valid anymore or your session has expired."
        … 20874 atau 20876
4944ff: push 0x68b354            ; format "%s\r\n%s\r\n%s\r\n\r\n%s"
49450a: call 0x43b4b0            ; susun teks
494563: push 0x1                 ; MB_OK
49456e: call 0x475890            ; MessageBox
```

---

## 6. Ketika browser mengunduh ulang berkas tersebut

Setiap **capture baru** dari integrasi browser diproses oleh prosedur daftar unduhan di
`0x49333A`:

```asm
49333a: mov  eax,[esi+0x370]      ; ada yang menunggu?
493340: test eax,eax
493342: je   end
493348: push eax
49334b: call 0x460b70             ; record unduhan yang menunggu -> ebx
49335c: mov  ecx,[esi+0x23c]
493362: call 0x4d2c80             ; daftar unduhan
493367: mov  eax,[eax]            ; ebx = record unduhan yang menunggu
493377: call 0x460b70             ; record capture baru -> edi
4933ad: mov  eax,[edi+0x3c]       /* bandingkan kedua record  */
4933b1: mov  ecx,[ebx+0x3c]
4933b5: mov  edx,[edi+0x20]       /* URL                    */
4933b9: mov  eax,[ebx+0x20]
4933bd: push eax
4933c2: mov  ecx,0x7663a8
4933c7: call 0x433cb0             /* -> "berkas sama?"        */
4933c7: …  [edi+0x144] / [edi+0x148]   (owner page + nama berkas)
4933e8: call 0x4340e0             /* -> "owner page sama?"    */
4933ed: …  [edi+0x174] / [edi+0xe8]
49340e: call 0x4340e0             /* -> "berkas sama?"        */
493413: cmp  BYTE PTR [esi+0x17ec],0x0  /* refresh sedang berjalan? */
49341a: jne  4936eb                     /* -> terapkan alamat baru */
```

Jadi **pencocokan** dilakukan dengan membandingkan field-field identitas kedua record
(URL, owner page `+0x144`, nama berkas `+0x148`, dan field `+0x174` / `+0xe8`).

### 6.1 Memindahkan alamat & data sesi baru

Di `0x4936EB` (hanya reached bila flag refresh aktif):

```asm
4936eb: mov  eax,[edi+0x44]      ; state capture baru
4936f3: cmp  eax,4
4936f5: je   493aa4             ; state ini dilewati
4936fb: cmp  eax,5
4936fe: je   493aa4
493704: cmp  DWORD PTR [edi+0x38],0x0   ; ada data string?
493708: je   49372a
49370a: mov  edx,[edi+0x38]
493721: push edx
493722: mov  ecx,[ebp-0x14]     ; record unduhan yang menunggu
493725: call 0x5bc280           /* 0x5bc280 -> add ecx,0x20 ; jmp 0x454a40
                                   menulis string ke varian +0x18 dari objek URL */
493736: push 0x0
49373c: lea  ecx,[edi+0x20]     ; objek URL capture baru
49373f: call 0x596940           /* ambil string URL */
493753: mov  eax,[edi+0x194]    ; bandingkan dengan URL lama
4937a8: mov  eax,[edi+0x30]
4937ab: mov  ecx,[edi+0x2c]
4937b0: mov  ecx,[ebp-0x14]
4937b3: call 0x5bbc20           /* field sesi lain */
4937b8: mov  edx,[edi+0x3c]
4937bf: add  ecx,0x20
4937c2: call 0x454ac0           /* varian URL kedua */
```

⇒ Record unduhan lama **diperbarui di tempat** dengan URL baru dan data sesi dari capture.
Berkas **tidak** diunduh ulang dari nol.

Pseudocode Ghidra/REA untuk kasus `0x1467` di `FUN_0048fdb0` (potongan verbatim):

```c
case (undefined1 *)0x1467:
  ... /* piStack_220e0 = unduhan yang menunggu ; iVar10 = capture baru */
  uStack_22354 = 1;
  FUN_00433cb0(piStack_220e0[8],            *(undefined4 *)(iVar10 + 0x20));  /* bandingkan URL        */
  FUN_004340e0(piVar13[0x51],               *(undefined4 *)(iVar10 + 0x144)); /* bandingkan owner page */
  FUN_004340e0(piVar13[0x3a],               *(undefined4 *)(iVar10 + 0xe8));  /* bandingkan identitas  */
  ...
  else {   /* cabang refresh */
    if (*(int *)(iVar10 + 0x38) != 0) { ... FUN_005bc280(); }   /* string dari capture */
    FUN_00402500();
    FUN_00596940();      /* ambil URL baru dari objek URL capture (ecx = &capture+0x20) */
    FUN_0054fac0();
    iVar32 = FUN_00551350();                        /* bandingkan dengan URL lama */
    if ((iVar32 == 0) || (iVar32 = FUN_00550a00(), iVar32 != 0)) { /* batal */ }
    FUN_005bbc20();
    FUN_00454ac0();      /* tulis varian URL kedua pada unduhan lama */
    ...
  }
```

`piVar13[0x51] = 0x144` (owner page), `piVar13[0x3a] = 0xE8`, `piStack_220e0[8] = 0x20` —
cocok persis dengan pembacaan disassembly di atas.

---

## 7. Dialog konfirmasi

Evidence ID: `ev_9c14fae67fee030378a5eb62a85ef7b8ff62bc3e79e03aba83bd4a75ca6c7e65`
(prosedur `FUN_00441910`, badan `0x441910–0x442C9B`).

Formulir daftar capture (`FUN_00441910`, kasus switch `0x1465`) bertanya ke user sebelum memakai
alamat baru:

```asm
44219f: push  0x4                          ; MB_YESNO
4421a1: mov   ecx,[0x767598]                ; "Waiting for new data"        (20871)
4421a7: push  ecx
4421a8: mov   edx,[0x767594]                ; "Is this the new address for your expired download?" (20870)
4421ae: push  edx
4421af: mov   eax,[esi+0x20]
4421b2: push  eax
4421b3: call  0x475430                      ; MessageBox
4421bb: cmp   eax,0x6                       ; IDYES
4421be: jne   441d80
4421d1: push  0x1467                        ; wParam = 5223 "ya, pakai alamat ini"
4421d6: push  0x111                         ; WM_COMMAND
4421e2: call  [SendMessageA]
```

* **Ya** → `WM_COMMAND (0x111, 0x1467, idUnduhan)` → unduhan expired memakai alamat baru.
* **Tidak** → formulir ditutup (`0x5FB532`).

Pseudocode Ghidra/REA (potongan verbatim dari `FUN_00441910`):

```c
case 0x1465:
  iVar17 = FUN_00475430(*(undefined4 *)(param_1 + 0x20),
                         DAT_00767594,   /* "Is this the new address for your expired download?" (20870) */
                         DAT_00767598,   /* "Waiting for new data"                        (20871) */
                         4);             /* MB_YESNO */
  if (iVar17 == 6) {                     /* IDYES */
    LVar9 = SendMessageA(*(HWND *)(param_1 + 0x84), 0x111 /*WM_COMMAND*/, 0x1467,
                         *(LPARAM *)(*(int *)(param_1 + 0x88) + 0x508));
    ...
  }
```

---

## 8. Pesan sukses

```asm
4932af: mov  eax,[0x765d80]                  ; judul kosong
4932b5: mov  ecx,[0x7675a8]                  ; 20875 "New link address for this file was received successfully."
493302: call [GetDesktopWindow]
493309: call 0x475890                        ; MessageBox
```

---

## 9. Varian "IDM mengunduh ulang halaman pemilik sendiri"

Evidence ID: `ev_ca92e5eec9e6f367ee3158b6114e9506e502f9269e483a7b819512cf1ed3a9eb`
(prosedur `FUN_005c3610`, badan `0x5C3610–0x5C5446`) — mesin unduhan; serta dump Ghidra
`decompilation/ghidra/fn-5ccf4a.txt` untuk `FUN_005ccb90`.

Di `0x5CCF4A` IDM mengambil halaman pemilik itu sendiri dan mencari tautan baru:

```asm
5ccf26: mov  ecx,[edi+0xc]        ; record unduhan
5ccf29: mov  eax,[ecx+0x1a4]      ; URL owner page
5ccf2f: push eax
5ccf30: lea  ecx,[ebp+0xf240]
5ccf36: call 0x5bef70             ; salin URL
5ccf3b: push 0x6bb4e8
5ccf42: call 0x5b88f0             /* set referer: 0x5b88f0 -> mov ecx,[ecx+0x20]; jmp 0x56e660 */
5ccf47: push esi
5ccf48: mov  edx,[0x7675b8]       ; 20878 "Downloading owner web page to refresh link address"
5ccf4e: push edx
5ccf51: call 0x5b9a40             ; set status di daftar unduhan
5ccf56: push esi
5ccf57: lea  ecx,[ebp+0xf240]
5ccf5d: call 0x5ccaa0             ; perform request HTTP
```

Fungsi yang memuat kode ini dimulai di `0x5CCB90` dengan frame stack `0x10CD0` (fungsi besar).

---

## 10. Sisi browser

| Komponen | Bukti |
|---|---|
| `idmnmcl.dll` | "IDM Network Monitor" — monitor request di dalam browser; named pipe `\\.\pipe\IDMNetworkMonitor`; export `SendDwnlResult`, `SendDwnlResult2`, `SendRepairH`, `SendUnblock`; memakai `SendMessageA` |
| `IDMIntegrator64.exe` | "IDM Integration Loader" (`Tonec.IDM.IDMIntegrator64` v6.41.23.4) — dijalankan ekstensi browser untuk menyerahkan capture ke IDM |
| `idmBroker.exe` | COM broker (`idmBroker.OptionsReader`, `GetBCaptureDownloads`) agar ekstensi bisa membaca opsi capture IDM |
| `IDMGetAll.dll` / `IDMGetAll64.dll` | helper enumerasi link halaman (fitur *Download all*, bukan fitur refresh) |
| `IDMan.exe` | objek event `Local\IDMEventMonitor`, opsi `MonitorUrlClipboard` / `ControlMonitoring`, import `PeekNamedPipe` |

Sesi unduhan IDM memang membawa cookie: string `Cookie: `, `Cookie2: %s`, `Set-Cookie:`,
`cookies=%ld bytes`, `originalUrl_cookie=%ld bytes`, `Change cookies, try again.`, serta path
database cookie Chrome / Edge / Vivaldi / Opera / Firefox.

---

## 11. Tabel rujukan cepat

| Alamat | Isi |
|---|---|
| `0x459D40` | handler item menu "Refresh download address" (`0x2B40`) → WM_COMMAND `0x1469` |
| `0x468B0E` | pembuatan item menu `0x2B40` |
| `0x457017`, `0x4572C3` | enable/disable item menu `0x2B40` |
| `0x68B6C8` | record tabel dispatch untuk `0x2B40` → handler `0x459D40` |
| `0x68B680` | awal tabel dispatch perintah WM_COMMAND (record 12-byte) |
| `0x4941F3` | cek `0x1469`; pesan 20873 (hanya satu unduhan boleh menunggu) |
| `0x4942C7` | set `[esi+0x370]` = unduhan yang menunggu |
| `0x494489` | pilih pesan 20874 vs 20876 berdasarkan keberadaan owner page (`+0x144`) |
| `0x4945C3` | membuka owner page di browser (`0x4B7B10`) |
| `0x4945E8` | membuat panel "menunggu data baru" (kelas `0x94`) |
| `0x49333A` | capture baru masuk → pencocokan dengan unduhan yang menunggu |
| `0x4936EB` | transplant URL + data sesi baru ke record unduhan lama |
| `0x44219F` | dialog "Is this the new address for your expired download?" → `0x1467` |
| `0x4932B5` | pesan sukses 20875 |
| `0x5C4646`, `0x5C47A3` | mesin unduhan: "session expired" |
| `0x5C4AB0` | mesin unduhan: saran memakai menu "Refresh download address" (20877) |
| `0x5CCF4A` | varian: unduh ulang owner web page (20878) |

---

## 12. Bukti & file

Semua bukti REA ada di `analysis/evidence/`:

* `evidence/records/*.json` — record Evidence REA mentah per query (7 rekaman)
* `evidence/snapshots/*.snapshot` — snapshot analisis REA (query yang sama dilayani dari cache)
* `evidence/bundles/*.bundle.json` — bundle Evidence kanonik per query
* `evidence/rea-evidence-bundle.json` — **bundle gabungan hasil `export_evidence_bundle`** (7 rekaman,
  tervalidasi ulang dengan `rea inspect-evidence-bundle`)
* `evidence/errors/*.json` — envelope error dari query yang gagal (catatan proses)
* `decompilation/rea/*.md` — pseudocode + call graph dari Evidence REA, format Markdown
* `decompilation/ghidra/*.txt` — dump dekomposisi massal (semua fungsi kunci, 18 211 fungsi, xref string)

### 12.1 Daftar Evidence ID

| Evidence ID | Operasi | Target |
|---|---|---|
| `ev_70cc11db4bd5d075ae13fac94e41331da90c204f20003358f050410ff0cf687d` | `binary_overview` | `IDMan.exe` |
| `ev_c038ac15294a96f4e1884fcb1127c22de8f8df14bbaa3466fe5017ab4d0fb163` | `search_strings` | `"Refresh download address"` di `0x69A47C` |
| `ev_f3f6a2f40ba4e2a2039d6cc83ad2a3c3e2a61df7355d13ce31c6a9419202308c` | `analyze_function` | `FUN_00459d40` (handler menu) |
| `ev_ef4ab7e810ae449805d60f2c182637781d6a7729317d8785c202d3a2e1c09564` | `analyze_function` | `FUN_0048fdb0` (form daftar unduhan) |
| `ev_9c14fae67fee030378a5eb62a85ef7b8ff62bc3e79e03aba83bd4a75ca6c7e65` | `analyze_function` | `FUN_00441910` (dialog konfirmasi) |
| `ev_ca92e5eec9e6f367ee3158b6114e9506e502f9269e483a7b819512cf1ed3a9eb` | `analyze_function` | `FUN_005c3610` (mesin unduhan) |
| `ev_6e4ea0ae77d7e53086702bb4bd4cf533528655e7729c2172f9a5a4451b5760fd` | `procedure_pseudo_code` | `FUN_00483200` (rekaman sampingan) |

Indeks lengkap ada di `notes/03-evidence-index.md`.

### 12.2 Cara memverifikasi ulang

```bash
# 1. bundle Evidence hasil REA
rea inspect-evidence-bundle "$(python3 -c "import json;print(json.dumps({'path':'$PWD/analysis/evidence/rea-evidence-bundle.json','detail':'summary'}))")"

# 2. peta string-ID ke slot global + xref kode (tanpa Ghidra, <1 detik)
python3 analysis/tools/idm_string_map.py --grep refresh --refs

# 3. dekomposisi ulang massal
mkdir -p analysis/ghidra-tmp
"$GHIDRA_INSTALL_DIR/support/analyzeHeadless" "$PWD/analysis/ghidra-tmp" idm-dump \
  -import app/IDMan.exe -overwrite -scriptPath "$PWD/analysis/tools/ghidra" \
  -postScript IdmRefreshDump.java "$PWD/analysis/decompilation/ghidra" -deleteProject
```

---

## 13. Batasan & hal yang belum terbukti

* **Statis saja** — tidak ada eksekusi IDM, tidak ada capture jaringan, tidak ada screenshot UI.
* Tidak ada PDB/symbol ⇒ nama fungsi di laporan ini adalah `FUN_<alamat>` (Ghidra) atau deskripsi fungsional.
* Batas fungsi Ghidra untuk prosedur Delphi yang sangat besar dapat berbeda dari tool lain;
  semua alamat dalam laporan ini diverifikasi terhadap disassembly instruksi.
* Pertanyaan yang masih terbuka: apakah field `+0x38` adalah cookie atau varian URL kedua;
  apakah perbandingan "berkas sama" dinormalisasi; bagaimana HTML owner page diparse;
  dan siapa sebenarnya yang menulis field cookie ke record unduhan.
  → Lihat `notes/04-open-questions.md` untuk probe lanjutan yang disarankan.
