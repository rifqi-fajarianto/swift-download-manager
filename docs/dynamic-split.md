# Desain implementasi: Dynamic Segment Split (work-stealing)

| | |
|---|---|
| **Status** | Diimplementasi di `0.25.0` (P1-P4). P5 (§12) masih ditunda. |
| **Versi** | `0.25.0` (fitur baru; `0.24.0` sudah terpakai rilis sebelumnya) |

> **Catatan kejujuran.** Potongan kode di dokumen ini adalah **kerangka** asli
> dari hasil membaca kode. Implementasi akhir mengikuti gaya dan tipe di kode
> sekitar, dan perilakunya dibuktikan oleh test (§6). Perbedaan yang disengaja
> dari kerangka: `pick_split` punya `min_thief` eksplisit (bukan konstanta 1 MiB
> di dalam aturan), test straggler memakai perbandingan relatif terhadap
> baseline `DynamicSplit = 0` (bukan batas waktu absolut), dan kecepatan
> referensi juga diambil dari rentang yang selesai sebelum sampler 200 ms
> pertama berjalan. Hal yang belum terverifikasi ditandai `UNKNOWN` (§9).

---

## 0. Cara memakai dokumen ini (untuk AI agent)

Aturan wajib (ringkasan dari `AGENTS.md`; kalau berbeda, `AGENTS.md` yang menang):

1. Baca `AGENTS.md` lebih dulu. **Jangan mengubah `AGENTS.md`** tanpa izin user.
2. Jalankan `git status --short`, `git branch --show-current`, `git log --oneline -5`. Jangan menimpa perubahan yang belum di-commit.
3. Kerjakan **per fase (§5), satu commit per fase**. Fase 1 adalah refactor murni: tidak boleh ada perubahan perilaku maupun UI.
4. Setiap fase selesai hanya jika: build sukses, `ctest --test-dir build --output-on-failure` hijau seluruhnya, dan test baru untuk fase itu sudah ada.
5. **Dilarang menguji lewat GUI** (klik otomatis, screenshot, `xdotool`). Bagian UI diuji user secara manual (§11).
6. `src/core`, `src/engine`, `src/net` tidak boleh bergantung pada Qt Widgets.
7. Komentar dan nama di kode memakai bahasa **Inggris**. Dokumen ini dan komunikasi dengan user memakai bahasa Indonesia. Teks `docs/spec/*` dan `CHANGELOG.md` memakai bahasa Inggris (mengikuti file yang ada).
8. Jangan menambah dependency. Jangan menyalin kode dari proyek lain; rancangan ini ditulis sendiri.
9. Build dan test:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

CI juga menjalankan sanitizer (`address+undefined` dan `thread`) pada build headless. Jangan menulis kode yang bergantung pada urutan thread atau timing yang rapuh.

---

## 1. Masalah dan tujuan

**Masalah.** `plan_segments()` membagi file menjadi `N` rentang sama besar di awal. Kecepatan tiap koneksi tidak sama, jadi koneksi yang selesai lebih awal menganggur sementara satu koneksi lambat masih memegang sisa besar. Contoh: file 1 GB, 8 koneksi, tiap segmen 128 MB. Segmen terakhir tersisa 64 MB di ~1 Mbps butuh ~8,5 menit, padahal 7 koneksi lain sudah idle.

**Tujuan.** Saat ada slot koneksi kosong dan tidak ada segmen yang menunggu giliran, ambil bagian belakang dari segmen aktif yang paling lambat dan unduh bagian itu lewat koneksi baru. Jumlah koneksi aktif **tidak pernah** melebihi pengaturan pengguna.

**Bukan tujuan.**
- Menambah kecepatan ketika bandwidth line sudah penuh. Fitur ini hanya membantu bila lambatnya berasal dari batas per koneksi (throttle server/CDN, RTT, node yang buruk).
- Mengubah jalur FTP, HLS, atau `stream_download()` (unduhan tanpa Range).
- Mengubah format `segments.json`.
- Mengubah UI di fase mana pun, kecuali keputusan K2 dijawab "ya".

---

## 2. Keadaan kode saat ini

Semua rujukan memakai nama simbol, bukan nomor baris. Simbol ada di `src/net/http_segment.cpp` kecuali disebut lain.

### 2.1 Yang sudah mendukung

| Fakta | Dampak |
|---|---|
| `write_cb` membatasi tulisan ke `seg.length() - seg.done` dan membuang kelebihannya (`if (seg.done >= seg.length()) return total;`) | Mengecilkan `seg.end` pada segmen yang sedang berjalan aman untuk data di disk |
| Start loop di `run()` memakai `if (active >= parallel_limit) break;` | Segmen lebih banyak dari slot sudah didukung sebagai antrean |
| `validate()` di `segment_plan.cpp` hanya mensyaratkan segmen berurutan, kontigu dari 0 sampai `file_size`, dan `done <= length` | Format `segments.json` tidak perlu diubah |
| Engine menyalin `state.segments` ke `ProgressInfo` tiap callback progres, dan `DownloadTask` menyalinnya ke `Snapshot` | UI tahan terhadap jumlah segmen yang berubah |
| `SegmentBar::set_segments` dan `refresh_connections()` memakai urutan vector dan offset kumulatif | Tetap benar bila segmen disisipkan di tengah, selama vector tetap terurut |

### 2.2 Yang akan rusak jika segmen dipecah tanpa perubahan

| # | Masalah | Lokasi | Dampak |
|---|---|---|---|
| R1 | `runtime` dan `contexts` adalah `std::vector` yang diindeks `i`; `CURLOPT_WRITEDATA` menyimpan `&contexts[i]`; `HandleCtx::index` menunjuk `state.segments[index]` | `Impl`, `start_segment`, `write_cb`, `header_cb` | Sisipan di tengah menggeser indeks dan membuat pointer curl menggantung |
| R2 | `const size_t count = ...segments.size()` dihitung sekali di awal `run()` | `SegmentDownloader::run` | Segmen baru tidak pernah dimulai |
| R3 | `header_cb` membandingkan `Content-Range.end` dengan `seg.end` **saat ini** | `header_cb` | Bila split terjadi antara request dikirim dan header tiba: `content_range_bad`, lalu download gagal fatal |
| R4 | Koneksi korban tetap menerima data sampai `end` lama, lalu `write_cb` membuangnya | `write_cb` | Bandwidth terbuang dan koneksi korban tidak selesai tepat waktu |
| R5 | `concurrency = AdaptiveConcurrency(count)` memakai jumlah segmen sebagai batas koneksi | `run()` | Setelah resume dengan 12 segmen, batas jadi 12 koneksi, melebihi pengaturan pengguna |
| R6 | `rt.done` dan `rt.attempts` dipakai per indeks; blok "No active handles" menelusuri `runtime` | `run()` | Perlu diadaptasi ke struktur worker baru |
| R7 | Tidak ada kecepatan per segmen, hanya `speed` agregat | `run()` | Tidak ada dasar memilih korban |
| R8 | `AdaptiveConcurrency::on_success()` mereset `deferrals_` | `retry_policy.h` | Tidak bisa dipakai sebagai penanda "host pernah menolak koneksi"; butuh flag sticky sendiri |

---

## 3. Keputusan desain

Kata **HARUS** berarti persyaratan, bukan saran.

**D1. Identitas segmen = `Segment::start`.** Nilai `start` tidak pernah berubah selama hidup segmen (hanya `end` dan `done` yang berubah), sehingga cocok sebagai kunci stabil. Struct `Segment` dan `segments.json` **tidak berubah**. Vector `state.segments` HARUS selalu terurut menurut `start`.

**D2. Satu `Worker` per segmen.** `Worker` menggabungkan `HandleCtx` dan `SegRuntime`, dialokasikan di heap sehingga alamatnya stabil, dan disimpan di `std::map<uint64_t, std::unique_ptr<Worker>>` dengan kunci `Segment::start`. Seluruh pemakaian indeks `i` diganti dengan `Worker&` atau lookup `seg_of(start)`.

**D3. Worker dibuat eager.** `run()` membuat Worker untuk semua segmen di awal, dan `try_split` membuat Worker segmen baru saat memecah. Dengan begitu logika "menunggu retry" dan "stalled" tetap seragam tanpa pembuatan Worker secara lazy.

**D4. Pemecahan atomik di thread `run()`.** `try_split` dipanggil dari loop `run()` di antara iterasi, **sebelum** `curl_multi_perform`, dan tidak pernah dari dalam callback curl. Karena semua callback, progress lambda, dan penyimpanan state berjalan di thread yang sama, `segments.json` selalu valid di titik mana pun ia ditulis.

**D5. Primitif pemecah murni.** `split_segment(segs, i, mid)` adalah fungsi murni di `segment_plan.cpp`. Segmen `[start,end]` menjadi `[start, mid-1]` (korban) dan `[mid, end]` (thief, `done = 0`). Syarat: `next_offset() < mid <= end`. `done_bytes()` total tidak berubah, jadi progres tidak melompat.

**D6. Korban mempertahankan bagian depan.** Korban sudah streaming dan koneksinya hangat, jadi ia menyelesaikan bagian depan. Thief mengambil bagian belakang. Tulisan korban selalu di bawah `mid`, tulisan thief di atasnya, sehingga tidak ada `pwrite` yang tumpang tindih.

**D7. Korban dihentikan secara sengaja.** `HandleCtx::requested_end` menyimpan `end` yang dikirim di header `Range`. Di `write_cb`, setelah `seg.finished()` dan `seg.end < ctx->requested_end`, set `ctx->split_stop = true` lalu `return 0` (curl melaporkan `CURLE_WRITE_ERROR`). `finalize_segment` HARUS memperlakukan `split_stop && seg.finished()` sebagai **sukses**, dan HARUS memeriksanya **sebelum** cabang "disk write failed".

**D8. `header_cb` memakai `requested_end`.** Perbandingan `Content-Range.end` HARUS dengan `ctx->requested_end`, bukan `seg.end`. Ini menutup R3. Sebagai lapis kedua, kandidat korban HARUS sudah `streaming` (lihat D10).

**D9. Batas koneksi berasal dari pengaturan, bukan dari jumlah segmen.** `SegmentDownloader::set_max_connections(n)` menentukan batas awal `AdaptiveConcurrency` (`n > 0`). Nilai `0` mempertahankan perilaku lama (`count`). Engine mengirim `can_resume ? connections : 1`.

**D10. Kebijakan pemilihan korban dipisah sebagai fungsi murni** (`pick_split`, §4.2) agar bisa diuji unit tanpa jaringan. Ambang awal:

| Konstanta | Nilai | Alasan |
|---|---|---|
| `min_remaining` | 2 MiB | Di bawah ini biaya handshake lebih besar dari manfaat; juga menjamin thief >= `min_chunk` (1 MiB) |
| `min_eta_ms` | 4000 | Sisa waktu minimal supaya layak dipecah |
| `warmup_ms` | 1500 | Kecepatan EMA belum bisa dipercaya sebelum ini, dihitung sejak byte body pertama |
| `cooldown_ms` | 2000 | Mencegah memecah segmen yang baru dibuat atau baru dipecah |
| `min_keep` / `max_keep` | 0.2 / 0.5 | Rentang porsi yang dipertahankan korban |

Porsi korban `frac = clamp(v / (v + ref), min_keep, max_keep)`, dengan `v` kecepatan EMA korban dan `ref` puncak EMA koneksi tercepat yang meluruh. Korban lambat mendapat bagian kecil, korban sehat 50/50. Semua angka adalah **titik awal**; tuning dilakukan dari log per download (D12).

**D11. Kapan split dilarang.** Split HARUS dimatikan bila salah satu benar: `one_by_one`, `dynamic_split == false`, `active >= parallel_limit`, ada segmen yang belum aktif dan belum selesai (termasuk yang menunggu retry), atau `narrowed()`. `narrowed()` benar bila `concurrency.limit() < initial_limit` atau flag sticky `host_refused_` pernah diset (R8).

**D12. Log keputusan.** `SegmentDownloader::set_event_log(std::function<void(const std::string&)>)`. Engine mengisinya dengan `append_download_log(reporter.log_path, msg)`, sehingga tiap split tercatat di log per-download (`logs/<id>-<filename>.log`) untuk tuning ambang dari download nyata.

**D13. Kill-switch.** `Settings::dynamic_split` (default `true`), disimpan di INI `[download]` sebagai `DynamicSplit = 1`. Tidak perlu fallback ke section `[general]` karena key ini baru. Checkbox UI bersifat opsional (K2).

---

## 4. Spesifikasi perubahan per file

### 4.1 `src/engine/segment_plan.h` dan `.cpp`

Tambahkan deklarasi dan implementasi:

```cpp
// Splits segment `index` at absolute offset `mid`: the segment becomes
// [start, mid-1] and a new segment [mid, end] with done = 0 is inserted right
// after it. Requires next_offset() < mid <= end. The total done_bytes() and the
// contiguity of the plan are preserved. Returns false (and changes nothing)
// when the arguments are out of range. References into `segs` are invalidated.
bool split_segment(std::vector<Segment>& segs, size_t index, uint64_t mid);
```

```cpp
bool split_segment(std::vector<Segment>& segs, size_t index, uint64_t mid) {
    if (index >= segs.size()) return false;
    Segment& s = segs[index];
    if (mid <= s.next_offset() || mid > s.end) return false;
    const Segment tail{mid, s.end, 0};
    s.end = mid - 1;
    segs.insert(segs.begin() + static_cast<std::ptrdiff_t>(index) + 1, tail);
    return true;  // `s` is dangling after the insert; do not touch it
}
```

### 4.2 File baru: `src/engine/split_policy.h` dan `.cpp`

Tambahkan `src/engine/split_policy.cpp` ke daftar sumber `swiftdm_core` di `CMakeLists.txt`, di dekat `src/engine/segment_plan.cpp`.

```cpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace swiftdm {

struct SplitPolicy {
    uint64_t min_remaining = 2u << 20;
    int64_t  min_eta_ms = 4000;
    int64_t  warmup_ms = 1500;
    int64_t  cooldown_ms = 2000;
    double   min_keep = 0.2;
    double   max_keep = 0.5;
    double   speed_floor_bps = 1024.0;  // avoids dividing by ~0 for a stalled segment
};

struct SplitView {
    size_t   index = 0;          // index into TransferState::segments
    uint64_t remaining = 0;      // length() - done
    double   speed_bps = 0;      // per-segment EMA
    int64_t  streaming_ms = -1;  // ms since the first body byte of the current request; < 0 = not streaming yet
    int64_t  since_split_ms = 0; // ms since the segment was created or last split (large = never)
};

struct SplitChoice {
    size_t   index = 0;
    uint64_t keep = 0;           // bytes the victim keeps; 0 < keep < remaining
};

// Picks the active segment with the largest estimated remaining time, or
// nullopt when nothing is worth splitting. Pure function: no I/O, no clock.
std::optional<SplitChoice> pick_split(const std::vector<SplitView>& views,
                                      double ref_speed_bps,
                                      const SplitPolicy& policy = {});
}  // namespace swiftdm
```

Perilaku `pick_split` (normatif):
1. Kandidat valid bila `streaming_ms >= warmup_ms`, `since_split_ms >= cooldown_ms`, dan `remaining >= min_remaining`.
2. `eta_ms = remaining / max(speed_bps, speed_floor_bps) * 1000`. Pilih kandidat dengan `eta_ms` terbesar; kembalikan `nullopt` bila `eta_ms < min_eta_ms`.
3. `v = max(speed_bps, speed_floor_bps)`, `r = max(ref_speed_bps, v)`, `frac = clamp(v / (v + r), min_keep, max_keep)`, `keep = static_cast<uint64_t>(remaining * frac)`.
4. Jamin `0 < keep < remaining` dan `remaining - keep >= 1 MiB`.
5. Bila seri, pilih indeks terkecil (deterministik untuk test).

### 4.3 `src/net/http.h` (kelas `SegmentDownloader`)

```cpp
void set_max_connections(int n);   // 0 = number of segments (legacy behavior)
void set_dynamic_split(bool on);   // default false at the class level; the engine opts in
void set_event_log(std::function<void(const std::string&)> sink);
```

Default `dynamic_split = false` di `Impl` supaya pemakai `SegmentDownloader` lain (misalnya test langsung) tidak berubah perilakunya.

### 4.4 `src/net/http_segment.cpp`

Kerjakan dalam sub-langkah berikut (A dikerjakan penuh di Fase 1, sisanya di Fase 2 dan 3).

**A. Refactor `Worker` (Fase 1, tanpa perubahan perilaku).**

```cpp
struct HandleCtx {              // existing fields stay; `size_t index` is replaced by:
    uint64_t seg_start = 0;     // stable key (Segment::start)
    // new in phase 2:
    uint64_t requested_end = 0;
    bool     split_stop = false;
    int64_t  first_byte_ms = 0; // 0 = no body byte yet
    // ...
};

struct Worker {
    HandleCtx ctx;
    SegRuntime rt;
    uint64_t last_done = 0;     // phase 3: per-segment speed sampling
    double   speed_ema = 0;     // phase 3, bytes/s
    int64_t  last_split_ms = 0; // phase 3
};
std::map<uint64_t, std::unique_ptr<Worker>> workers;   // key = Segment::start

Worker& worker_at(uint64_t start) { return *workers.at(start); }
Segment& seg_of(uint64_t start) {
    auto it = std::lower_bound(state.segments.begin(), state.segments.end(), start,
        [](const Segment& s, uint64_t v) { return s.start < v; });
    return *it;                  // guaranteed to exist: start is unique and immutable
}
```

Tabel penggantian:

| Lama | Baru |
|---|---|
| `runtime[i]` | `worker_at(seg.start).rt` |
| `contexts[i]` | `worker_at(seg.start).ctx` |
| `ctx.index`, `ctx->index` | `ctx.seg_start`, `ctx->seg_start` |
| `state.segments[ctx->index]` | `impl->seg_of(ctx->seg_start)` |
| `start_segment(size_t i)` | `start_segment(Worker& w)` |
| `finalize_segment(size_t i, ...)` | `finalize_segment(Worker& w, ...)` |
| log `"segment " + std::to_string(i)` | `"segment @" + std::to_string(seg.start)` |
| `impl.runtime.assign(count, ...)`, `impl.contexts.assign(...)` | `impl.workers.clear()`, lalu satu Worker per segmen di `state.segments` |
| `cleanup_handles()` iterasi `runtime` | iterasi `workers` |
| scan linear `runtime[i].easy == msg->easy_handle` | `CURLOPT_PRIVATE` di `start_segment`, lalu `curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &priv)` dan `reinterpret_cast<Worker*>(priv)` |

`run()` HARUS memanggil `workers.clear()` lalu membangun ulang Worker di setiap pemanggilan, karena engine memanggil `downloader.run()` dua kali pada fallback concurrency refusal. Loop `for (size_t i = 0; i < count; ++i)` diganti iterasi atas `state.segments` (ukurannya bisa berubah di Fase 3, jadi **jangan** menyimpan `count`). Hati-hati dengan `goto finish`: jangan mendeklarasikan variabel baru ber-initializer di scope fungsi setelah `goto` pertama.

Fase 1 selesai bila seluruh test lama hijau dan tidak ada perubahan perilaku apa pun.

**B. Pembatasan tulisan dan penghentian korban (Fase 2).**

```cpp
// start_segment(): after computing `range`
ctx.requested_end = seg.end;
ctx.split_stop = false;
ctx.first_byte_ms = 0;
w.last_done = seg.done;      // phase 3
w.speed_ema = 0;             // phase 3

// header_cb: compare with what THIS request asked for
ctx->content_range_end != ctx->requested_end        // was: seg.end

// write_cb, after the existing guards for status >= 300 and bandwidth wait:
if (!ctx->first_byte_ms) ctx->first_byte_ms = now_ms();
Segment& seg = impl->seg_of(ctx->seg_start);
if (seg.done >= seg.length()) {
    if (seg.end < ctx->requested_end) { ctx->split_stop = true; return 0; }
    return total;                                   // existing: nothing left; discard
}
// ... existing capped write ...
seg.done += to_write;
if (seg.finished() && seg.end < ctx->requested_end) {
    ctx->split_stop = true;                         // the tail now belongs to another segment
    return 0;                                       // curl reports CURLE_WRITE_ERROR
}
return total;
```

```cpp
// finalize_segment(): right after the `interrupted` check, BEFORE content_range_bad,
// range_bad, the 2xx success branch and the CURLE_WRITE_ERROR "disk write failed" branch
if (ctx.split_stop && seg.finished()) {
    rt.done = true;
    concurrency.on_success();
    return 0;
}
```

Tambahkan juga flag sticky `bool host_refused_ = false;`, diset `true` di cabang concurrency refusal pada `finalize_segment` (tempat `on_deferral()` dipanggil).

**C. Kecepatan per segmen dan `try_split` (Fase 3).**

Di blok sampling 200 ms yang sudah ada, sambil menghitung `speed` agregat:

```cpp
best_conn_bps *= 0.98;   // decay once per sample; declared next to `speed` before the loop
for (auto& [start, w] : impl.workers) {
    if (!w->rt.active) continue;
    const Segment& s = impl.seg_of(start);
    const double inst = static_cast<double>(s.done - w->last_done) / dt;
    w->speed_ema = (w->speed_ema == 0.0) ? inst : (0.6 * w->speed_ema + 0.4 * inst);
    w->last_done = s.done;
    best_conn_bps = std::max(best_conn_bps, w->speed_ema);
}
```

Di `Impl`:

```cpp
bool dynamic_split = false;
int  max_connections = 0;
int  initial_limit = 0;
bool host_refused_ = false;
SplitPolicy split_policy;
std::function<void(const std::string&)> event_log;

bool narrowed() const { return host_refused_ || concurrency.limit() < initial_limit; }

bool has_pending() const {           // not finished and not active (includes waiting for retry)
    for (const Segment& s : state.segments) {
        if (s.finished()) continue;
        const Worker& w = *workers.at(s.start);
        if (!w.rt.active) return true;
    }
    return false;
}

bool try_split(int64_t now, double best_conn_bps) {
    std::vector<SplitView> views;
    for (size_t i = 0; i < state.segments.size(); ++i) {
        const Segment& s = state.segments[i];
        Worker& w = *workers.at(s.start);
        if (!w.rt.active || s.finished()) continue;
        const bool streaming = w.ctx.status == 206 && w.ctx.content_range_valid &&
                               w.ctx.first_byte_ms != 0;
        views.push_back({i, s.length() - s.done, w.speed_ema,
                         streaming ? now - w.ctx.first_byte_ms : -1,
                         now - w.last_split_ms});
    }
    const auto choice = pick_split(views, best_conn_bps, split_policy);
    if (!choice) return false;

    const uint64_t victim_start = state.segments[choice->index].start;
    const uint64_t mid = state.segments[choice->index].next_offset() + choice->keep;
    const uint64_t old_end = state.segments[choice->index].end;
    if (!split_segment(state.segments, choice->index, mid)) return false;

    auto thief = std::make_unique<Worker>();
    thief->ctx.impl = this;
    thief->ctx.seg_start = mid;
    thief->last_split_ms = now;
    workers.emplace(mid, std::move(thief));
    workers.at(victim_start)->last_split_ms = now;
    if (event_log) event_log("split: segment @" + std::to_string(victim_start) +
                             " keeps " + std::to_string(choice->keep) + " B, new segment @" +
                             std::to_string(mid) + ".." + std::to_string(old_end));
    return true;
}
```

Di `run()`:

```cpp
// replaces: AdaptiveConcurrency(impl.one_by_one ? 1 : static_cast<int>(count))
impl.initial_limit = impl.one_by_one ? 1
                   : (impl.max_connections > 0 ? impl.max_connections
                                               : static_cast<int>(impl.state.segments.size()));
impl.concurrency = AdaptiveConcurrency(impl.initial_limit);

// right after the "Start eligible segments" block, before "Finished everything?":
if (impl.dynamic_split && !impl.one_by_one && active < parallel_limit &&
    !impl.has_pending() && !impl.narrowed()) {
    impl.try_split(now, best_conn_bps);   // the new segment is started by the start loop next iteration
}
```

Blok "No active handles: either waiting for a retry or dead" menelusuri `workers`, bukan `runtime`.

### 4.5 `src/engine/download_engine.cpp` (jalur segmen)

Di dekat `SegmentDownloader downloader(options, state, fd);`:

```cpp
downloader.set_max_connections(can_resume ? connections : 1);
downloader.set_dynamic_split(settings_.dynamic_split);
downloader.set_event_log([&](const std::string& m) { append_download_log(reporter.log_path, m); });
```

Jalur fallback `set_one_by_one(true)` tidak perlu diubah: dengan `initial_limit = 1`, split otomatis tidak terjadi. Tidak perlu mengubah `plan_segments`, `load_transfer_state`, atau `validate`.

### 4.6 Settings

- `src/store/settings.h`: `bool dynamic_split = true;` dengan komentar bahwa ini `[design]`.
- `src/store/settings.cpp`: parse key `dynamicsplit` **hanya** di section `[download]` (parser memakai key huruf kecil, ikuti pola `connectionsatonce`), dan tulis `DynamicSplit = 1/0` di blok `[download]` pada fungsi penyimpan.
- `tests/test_settings.cpp`: tambah test round-trip untuk key baru dan default `true` bila key tidak ada.
- `src/ui/settings_dialog.cpp`: **tidak diubah** kecuali K2 dijawab "ya".

### 4.7 `tests/http_test_server.h`

Tambahkan dua parameter query pada route `/file/<size>`:

- `slowfrom=<offset>`: hanya request dengan `Range` yang mulai tepat di `offset` itu yang diperlambat.
- `slowms=<ms>`: jeda per chunk 64 KiB untuk request yang diperlambat (default 60, sama dengan `slow=1` sekarang).

Request lain tetap berkecepatan penuh. Parse query secara manual di tempat `slow` dihitung (`query.find("slow=1")`), teruskan nilainya lewat parameter opsional di akhir signature `serve_file` dan `send_file_response` agar pemanggil lain tidak berubah. Jeda dipakai di tempat `if (slow) sleep_ms(60);` sekarang.

### 4.8 CMake

- `CMakeLists.txt`: tambah `src/engine/split_policy.cpp` ke `swiftdm_core`.
- `tests/CMakeLists.txt`: tambah target `test_split_policy` mengikuti pola `test_plan` (`add_executable`, `target_link_libraries(... swiftdm_core)` sesuai pola yang dipakai target itu, `add_test(NAME split_policy COMMAND test_split_policy)`).

---

## 5. Fase implementasi

| Fase | Isi | Kriteria selesai |
|---|---|---|
| **P1** | Refactor `Worker` + `CURLOPT_PRIVATE` (§4.4-A). **Tanpa** perubahan perilaku. | Build sukses, seluruh `ctest` hijau, `git diff` tidak menyentuh `src/ui/` maupun `docs/spec/` |
| **P2** | `split_segment` (§4.1); perubahan `write_cb`, `header_cb`, `finalize_segment`, `requested_end`, `split_stop` (§4.4-B); unit test `split_segment` di `tests/test_plan.cpp` | Test `split_segment` hijau; test lama hijau; belum ada pemanggil `try_split` |
| **P3** | `split_policy` (§4.2); kecepatan per segmen dan `try_split` di loop (§4.4-C); API baru di `http.h` (§4.3); `tests/test_split_policy.cpp` | Unit test policy hijau; dengan `dynamic_split` default `false` di kelas, perilaku lama utuh |
| **P4** | Engine (§4.5), settings (§4.6), `http_test_server.h` (§4.7), tes integrasi (§6.3), dokumentasi dan versi (§10) | Seluruh `ctest` hijau, termasuk tes straggler; dokumen spec dan CHANGELOG diperbarui |
| **P5 (opsional)** | Daur ulang koneksi macet (§12) | Hanya jika user menjawab K3 "ya" |

Saran pesan commit mengikuti gaya repo, misalnya `refactor(net): key segment workers by start offset`, `feat(engine): split slow segments dynamically`.

---

## 6. Rencana tes

Semua tes memakai helper di `tests/test_util.h` (`CHECK`, `CHECK_EQ`, `make_temp_dir`, `cleanup_dir`) dan server lokal; tidak ada tes GUI.

### 6.1 Unit: `split_segment` (di `tests/test_plan.cpp`)

- `test_split_segment_basic`: file 10 MiB, 4 segmen; pecah segmen 3 di titik tengah. Cek `size == 5`, kontiguitas, `[s.start, mid-1]` dan `[mid, old_end]`, dan `done` korban tidak berubah.
- `test_split_segment_preserves_done_bytes`: `TransferState::done_bytes()` sebelum dan sesudah sama.
- `test_split_segment_rejects_invalid_mid`: `mid <= next_offset()`, `mid > end`, dan `index` di luar rentang mengembalikan `false` tanpa mengubah vector.
- `test_split_segment_roundtrip`: lakukan 20 pemecahan acak deterministik (seed tetap) dengan `done` acak yang valid, lalu `save_transfer_state` dan `load_transfer_state` di direktori sementara. State yang dimuat harus identik; `load` yang berhasil membuktikan `validate()` lolos.

### 6.2 Unit: `pick_split` (`tests/test_split_policy.cpp`)

Satu fungsi test per kasus: segmen belum `streaming` diabaikan; `streaming_ms < warmup_ms` diabaikan; `since_split_ms < cooldown_ms` diabaikan; `remaining < min_remaining` diabaikan; `eta < min_eta_ms` mengembalikan `nullopt`; memilih ETA terbesar; seri memilih indeks terkecil; korban lambat mendapat `keep` mendekati `min_keep`; korban setara `ref` mendapat 0,5; invarian `0 < keep < remaining` dan `remaining - keep >= 1 MiB` untuk 1000 input acak deterministik.

### 6.3 Integrasi (`tests/test_http.cpp`, daftarkan di `main()`)

Skenario dasar: file 16 MiB (`16u << 20`), `req.connections = 4`, sehingga segmen ke-4 mulai di offset `12582912`. URL: `/file/16777216?slowfrom=12582912&slowms=240`, kira-kira 0,27 MB/s untuk segmen ke-4 saja. Tanpa split segmen itu butuh ~16 detik.

1. **`test_dynamic_split_straggler`**: `settings.dynamic_split = true`. Jalankan sampai selesai. Cek `Completed`, `file_matches(result.path, size)`, jumlah segmen yang pernah teramati di `info.segments.size()` melebihi 4, dan total waktu jauh di bawah baseline (batas longgar, mis. < 10 detik; perkiraan ~4,5 detik). Gunakan batas longgar karena CI sanitizer lebih lambat.
2. **`test_dynamic_split_disabled`**: `dynamic_split = false`. Hentikan (return `false` dari callback progres) setelah ~3,5 detik. Cek `Paused` dan jumlah segmen yang teramati selalu `== 4`. Tes ini sengaja tidak menunggu selesai agar singkat.
3. **`test_dynamic_split_resume`**: jalankan seperti (1), pause begitu `info.segments.size() > 4`, pastikan `segments.json` ada, lalu jalankan `engine.run(req)` lagi (pola `test_resume_after_pause`). Cek `Completed` dan isi file cocok. Ini membuktikan state hasil split bisa dilanjutkan.
4. **`test_dynamic_split_respects_host_limit`**: pakai route yang hanya mengizinkan satu koneksi ranged (lihat `serve_limited_connections` dan pola `test_connection_limit_refusal_503`) dengan `dynamic_split = true`. Cek `Completed`, isi cocok, dan jumlah segmen yang teramati tidak pernah melebihi jumlah rencana awal (karena `narrowed()` mematikan split).

### 6.4 Verifikasi tambahan

- Bangun dan jalankan seluruh `ctest` di bawah sanitizer lokal bila memungkinkan (`-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"`), dan satu kali `thread`.
- Setelah test straggler lulus, periksa log per-download: harus ada baris `split: ...` dengan `keeps` dan offset yang masuk akal.

---

## 7. Invarian (harus selalu benar)

| ID | Invarian |
|---|---|
| I1 | `state.segments` terurut, kontigu dari 0 sampai `file_size`, tanpa celah atau tumpang tindih (yaitu `validate()` lolos) di setiap titik di mana state bisa disimpan |
| I2 | Untuk setiap segmen: `done <= length()` |
| I3 | `done_bytes()` tidak berubah oleh operasi split |
| I4 | Koneksi aktif <= `concurrency.limit()` <= `max_connections` (bila diset) |
| I5 | Tidak ada byte yang ditulis di luar `[seg.start, seg.end]` milik segmen pada saat penulisan; tidak ada dua segmen yang menulis offset yang sama |
| I6 | `header_cb` membandingkan `Content-Range` dengan range yang diminta request itu sendiri |
| I7 | Hanya segmen `streaming` (status 206, `Content-Range` valid, sudah ada byte body) yang bisa jadi korban |
| I8 | Split tidak terjadi saat `one_by_one`, `dynamic_split == false`, `narrowed()`, atau ada segmen pending |
| I9 | `state.segments` hanya dimutasi oleh thread `run()`, di antara iterasi loop, tidak pernah dari callback curl |

---

## 8. Kasus tepi

| Kasus | Penanganan |
|---|---|
| Split sebelum header respons tiba | Cegah dengan I7; `header_cb` memakai `requested_end` (D8) |
| Korban mengalami error dan di-retry setelah split | `start_segment` membaca `seg.end` baru, jadi range retry benar; `requested_end` diperbarui |
| Korban selesai persis saat split dipilih | Tidak mungkin: `try_split` hanya melihat `s.finished() == false`, dan semua `CURLMSG_DONE` sudah diproses sebelum fase "start eligible" iterasi berikutnya |
| Host menolak koneksi paralel (403/503) | `host_refused_` mematikan split; jalur `finalize_segment` yang ada tetap berlaku |
| Segmen thief ditolak (transien) | Masuk jalur deferred yang ada (`return 3`); segmen mengantre dan diulang |
| Pause, cancel, atau crash di tengah split | Mutasi atomik di satu thread (D4); state hasil split valid dan dapat di-resume |
| Resume dengan segmen > `connections` | Batas koneksi dari `set_max_connections` (R5); sisanya mengantre lewat start loop |
| `BandwidthLimit` aktif | Aman, tetapi tidak ada gain karena limiter bersifat agregat |
| Korban macet total (tidak ada callback, jadi tidak pernah abort sendiri) | Koneksinya baru berakhir lewat `LOW_SPEED_TIME` (timeout lalu retry). Penanganan lebih baik ada di P5 (§12) |
| File kecil | `min_remaining` (2 MiB) mencegah split tidak berguna |
| Progres UI | `done_bytes()` tidak berubah (I3); hanya jumlah segmen yang naik |

---

## 9. Risiko, hal yang belum diverifikasi, dan keputusan terbuka

### Belum diverifikasi (`UNKNOWN`)

1. **HTTP/2 multiplexing.** Tidak ada `CURLOPT_HTTP_VERSION`, `CURLOPT_PIPEWAIT`, atau `CURLMOPT_PIPELINING` di `src/net`, jadi libcurl memakai default. Bila build libcurl mendukung HTTP/2, segmen yang dimulai belakangan (thief) bisa ikut multiplex ke koneksi TCP yang sudah ada, bukan membuka koneksi baru. Bila throttle server bersifat per koneksi TCP, gain bisa hilang. **Ukur** dengan `CURLINFO_NUM_CONNECTS` atau `curl -v` pada host nyata. Bila terbukti terjadi, set `CURLOPT_FRESH_CONNECT` pada handle thief atau paksa HTTP/1.1 untuk transfer segmen.
2. **Pola nyata host pengguna.** Belum diketahui apakah lambatnya segmen terakhir berasal dari throttle per koneksi atau dari line yang penuh. Bandingkan kecepatan per segmen dengan kecepatan agregat sebelum menyimpulkan gain.
3. **Nilai ambang** (2 MiB, 4 s, 1,5 s, 2 s, 0,2 dan 0,5) adalah titik awal tanpa data lapangan. Tuning memakai log D12.
4. **Perilaku `write_cb` yang mengembalikan 0** sudah dipakai kode untuk `interrupted`, tetapi belum diuji untuk kasus `split_stop`. Tes straggler (§6.3-1) adalah buktinya.

### Risiko

- Fase 1 menyentuh ~15 tempat di `http_segment.cpp`. Ini bagian paling berisiko, makanya dipisah dan harus lulus seluruh test lama sebelum lanjut.
- Satu handshake TCP/TLS dan sedikit byte yang terbuang saat korban dihentikan adalah biaya per split. Ambang `min_remaining` dan `min_eta_ms` ada untuk menjaga agar biaya itu tidak melebihi manfaat.
- Test berbasis timing bisa rapuh di CI sanitizer. Gunakan batas longgar dan hindari `sleep` yang menentukan kebenaran.

### Keputusan terbuka (user yang menentukan; agent jangan memutuskan sendiri)

| ID | Pertanyaan | Default bila tidak dijawab |
|---|---|---|
| K1 | Satu rilis untuk seluruh fitur (fase P1-P4 di satu branch, versi dan CHANGELOG hanya di commit terakhir), atau naikkan versi per fase sesuai `AGENTS.md` §5? | **Dijawab: satu rilis `0.25.0`**, commit per fase, versi dinaikkan di commit terakhir |
| K2 | Tambahkan checkbox di dialog Settings sekarang? | **Dijawab: tidak.** Hanya key INI, `src/ui/` tidak berubah |
| K3 | Kerjakan P5 (daur ulang koneksi macet)? | **Dijawab: ditunda** |
| K4 | Bila HTTP/2 multiplexing terbukti, pakai `FRESH_CONNECT` untuk thief atau paksa HTTP/1.1? | Putuskan setelah ada ukuran (risiko 1) |

---

## 10. Dokumentasi dan versi

Semua dikerjakan di P4. Perubahan dokumentasi murni tidak menaikkan versi, tetapi fitur ini mengubah perilaku, jadi versi dan CHANGELOG ikut (sesuai K1).

1. `CMakeLists.txt`: `project(SwiftDM VERSION 0.25.0)` (satu-satunya sumber versi; jangan mengedit file generated).
2. `CHANGELOG.md`: tambahkan entri teratas (bahasa Inggris, ikuti gaya entri yang ada), contoh draf:

```markdown
## 0.25.0 — <date>

Splits the slowest running segment when a connection slot is free, so the last
connection no longer drags the end of a segmented download.

- When fewer connections are active than the configured limit and no segment is
  waiting to start, the active segment with the longest estimated remaining
  time is split: it keeps the front part and a new range request takes the
  tail. The number of active connections never exceeds the setting.
- `segments.json` format is unchanged; a resumed download with more segments
  than connections queues the extra ranges.
- A new `DynamicSplit` key in the `[download]` section turns it off.
- Each split is written to the per-download log.
```

3. `docs/spec/03-engine.md` §2: tambahkan butir `[design: 0.25.0]` yang menjelaskan pemecahan dinamis, batas koneksi, dan bahwa jumlah segmen bisa melebihi `connections` selama download.
4. `docs/spec/05-settings.md`: tambahkan `DynamicSplit = 1   ; [design]` di blok `[download]`.
5. `docs/spec/02-data-model.md`: catat bahwa `segments.json` boleh memuat lebih banyak segmen dari `connections`.
6. `docs/spec/06-parity-checklist.md`: tambah baris "Dynamic segment split" berstatus `Implemented ([design], not in the original)`.
7. `docs/development_status.md`: perbarui snapshot (tanggal dan versi kerja).
8. Untuk rilis (bukan bagian patch ini), periksa versi contoh di `packaging/README.md` dan `packaging/io.github.swiftdm.SwiftDM.metainfo.xml` sesuai `AGENTS.md` §5.5.

---

## 11. Uji manual UI (dikerjakan user, bukan agent)

Agent hanya boleh menyerahkan daftar ini ke user setelah P4:

1. Unduh file besar dari host yang membuat segmen terakhir lambat, dengan 8 koneksi.
2. Buka jendela progres. Label "Segments: N" boleh naik melebihi 8 di tengah unduhan; bar segmen menambah potongan baru tanpa glitch; tabel Connections menambah baris dan penomoran mengikuti urutan offset.
3. Pastikan persentase dan ukuran total tidak melompat saat split.
4. Pause saat jumlah segmen sudah lebih dari 8, lalu resume sampai selesai; file hasil harus utuh (bandingkan checksum bila tersedia).
5. Matikan `DynamicSplit = 0` di `swiftdm.ini` dan pastikan perilaku kembali seperti sebelumnya (segmen tetap sebanyak koneksi).
6. Periksa `~/.local/share/swiftdm/logs/<id>-<filename>.log` untuk baris `split:`; bagikan hasilnya bila ingin menyetel ambang.

---

## 12. Di luar cakupan (kandidat lanjutan)

**P5. Daur ulang koneksi macet.** Korban yang benar-benar macet (EMA mendekati 0, tidak ada callback) tidak pernah menghentikan dirinya sendiri setelah split. Rancangan: saat `try_split` memilih korban dengan `speed_ema` di bawah ambang macet (mis. 16 KiB/s), hapus handle korban (`curl_multi_remove_handle` + cleanup), kosongkan `rt.active`, kurangi `rt.attempts` supaya tidak menghabiskan jatah retry, dan biarkan start loop memulai ulang korban dengan range barunya. Efek samping yang baik: koneksi baru sering mendarat di node CDN yang lebih sehat. Kerjakan hanya bila K3 dijawab "ya" dan setelah data log menunjukkan korban macet memang sering terjadi.

**Antrean chunk kecil** (alternatif desain): 8 worker mengambil chunk 4-16 MB dari antrean. Lebih sederhana secara konsep, tetapi mengubah `plan_segments`, ukuran `segments.json`, dan tampilan UI. Tidak dipilih karena dynamic split cocok dengan desain yang sudah ada.