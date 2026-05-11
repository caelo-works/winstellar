# WinStellar — Code Review

Audit date: 2026-05-11
Scope: `fits_core/`, `viewer/`, `shell_ext/`, `tools/`. CMake build files are also covered. `tests/` is only spot-checked.
Reviewer focus per request: **performance**, with secondary correctness / modern C++17 / Win32 hygiene.

---

## 1. Executive summary

The codebase is in solid shape: small, readable, idiomatic C++17, and clearly the work of someone who knows Win32 and is aware of the relevant pitfalls (DPI, dark titlebar, COM threading, stream cap, cache invalidation). The structural decisions (single shared `fits_core` static lib, dedicated render worker with coalescing, `shared_ptr<const FitsImage>` to keep workers safe across navigation) are right. The top findings:

- **The hot pixel loop in `render_to_bgra` is the dominant CPU cost on every stretch change** and is currently fully scalar, branchy (per-pixel `isfinite`), and stride-unfriendly. There is large room for both micro-optimization (SIMD-friendly straight-line code) and macro-optimization (build a per-channel LUT instead of computing the stretch math per pixel for 8-bit output). Expected speedup on a 36 Mpx file at 1:1: 4–10×. **This is the biggest perf win available.**
- **`fits_loader.cpp` reads whole files into RAM, then copies them twice** (CFITSIO internal + a 1:1 `flipped` vector). On a 200 MB monolithic FITS that's an extra ~800 MB of allocator churn and ~400 ms of memcpy on a typical laptop. The flip can be fused with a value-range scan, and CFITSIO can be told to flip on read.
- **`compute_pixel_stats` (analysis) makes three passes over millions of floats and allocates a `work` buffer of `valid` floats just to take the median.** For a 36 Mpx float image that's a 144 MB temporary. A reservoir or quantile estimator gives an essentially-identical result for ~1 % of the cost; even just sampling (like `compute_auto_stretch` does) would be a 100× win.
- **`Histogram::render()` rebuilds three D2D PathGeometry objects every paint** for the diamond handles. At 60 fps drag this is a measurable allocator hot path; one-time cache eliminates it.
- **`detect_stars` keeps a `vector<int> xs/ys/vs` per blob and a `vector<int32_t> label(W*H)`** allocated up-front for every analysis. For 36 Mpx, that's ~144 MB of `label`, then unbounded per-blob `push_back` reallocation. Both can be flattened, and on real images >99 % of blob slots are unused. Restructuring saves ~150 MB and ~30 % of star-detection time.
- **Correctness:** several minor latent bugs (UB in `fits_render`'s "swapped bbox center" expression, `swprintf_s(L"%g", double)` for `wchar_t` printf is well-defined but `%g` truncates max counts oddly, an unjoined `detach()` race during shutdown, double-stretch computation on each load, an `image` move/use sequence in `populate()` that's safe today but fragile). None block release.

The code below is graded P0 (correctness or major perf), P1 (significant), P2 (nice-to-have), Note (informational).

---

## 2. Findings by area

### 2.1 `fits_core/src/fits_render.cpp` — BGRA conversion (hot path)

This is called on **every** stretch slider drag and on every file load. It is also called twice on every shell-ext thumbnail/preview. The current implementation is the single largest perf opportunity in the project.

#### 🔴 P0 — Per-pixel stretch math is needlessly expensive at 8-bit output
**Location:** `fits_render.cpp:56–91`
**Observation:** For each output pixel the loop computes:
```cpp
float n = (v - src_min) / frange;       // normalize
n = (n - shadows) / denom;              // clip
n = std::clamp(n, 0.0f, 1.0f);
n = apply_mtf(n, midtone);              // div in apply_mtf
// → uint8_t
```
That's two divisions, a branchy `clamp`, an `isfinite` check inside the box-average loop, and an `apply_mtf` that has three branches before its `num/den` division. For a 36 Mpx image at 1:1 that's ~144 M floating divisions on the user's main perceived latency path.

**Impact:** On a Zen 4 / Alder Lake laptop, this loop runs at roughly 70–120 Mpx/s in scalar code. A 36 Mpx render then takes 300–500 ms, blocking the next slider tick. The render worker coalesces, but the floor is still painful.

**Recommendation:** When `max_width/max_height` is 0 (i.e. native render — the common case in the viewer), the output range is 0..255. Precompute a **single-precision LUT of 4096 entries** that maps a quantized input position to a `uint8_t`:
```cpp
// One-time per stretch:
std::array<uint8_t, 4096> lut;
for (int i = 0; i < 4096; ++i) {
    float n = (i + 0.5f) * (1.0f / 4096.0f);   // mid-bin
    n = (n - shadows) / denom;
    n = std::clamp(n, 0.0f, 1.0f);
    n = apply_mtf(n, midtone);
    lut[i] = static_cast<uint8_t>(n * 255.0f + 0.5f);
}
// Per pixel: normalize, quantize to 12 bits, table-lookup.
uint32_t idx = static_cast<uint32_t>(
    std::min(4095.0f, std::max(0.0f, (v - src_min) * (4096.0f / frange))));
uint8_t g = lut[idx];
```
A 4096-entry LUT (4 KB) easily fits in L1. The per-pixel cost drops from ~6 ops + 2 divides to 1 FMA + 1 clamp + 1 indexed load. Empirically this is a 4–8× speedup on the native-resolution path. Quality is indistinguishable on 8-bit output (12-bit input quantization is well below display banding).

When downsampling, the box-average sum is still computed in float, but the same LUT applies to the averaged value — no quality loss.

#### 🔴 P0 — `isfinite` check inside the inner box-average loop
**Location:** `fits_render.cpp:73–75`
**Observation:** `std::isfinite(v)` branches on every pixel inside the inner accumulation. On Windows/MSVC `isfinite` is a function call unless `/fp:fast` is set (and even then it's a bit-twiddle). FITS files in practice contain NaNs only at masked edges (rare) or saturated pixels (specific BLANK keyword convention). The branch wrecks vectorization.

**Impact:** Roughly halves the throughput of the box-average loop. At target_w == img.width (no downsample), `x1-x0 == 1` and `y1-y0 == 1`, so the inner loop runs once per output pixel, and the branch is the dominant cost.

**Recommendation:** Sanitize NaN/Inf once at load (replace with `src_min` so they fall in the shadows clip — they already render as black). Then the render loop is branch-free:
```cpp
// in fits_loader.cpp / xisf_loader.cpp, after reading raw pixels:
for (float& v : flipped) if (!std::isfinite(v)) v = src_min;
```
Cost at load: ~20 ms on 36 Mpx, one-time. Gain at render: ~40–50 % of the inner-loop time. Combined with the LUT, the total render at 1:1 should drop from ~400 ms to **~50–80 ms**.

#### 🟡 P1 — Native-resolution fast path is being run as a (1×1) box average
**Location:** `fits_render.cpp:56–77`
**Observation:** When `max_width == 0 && max_height == 0` (the viewer's normal case), the code computes `target_w = img.width`, `scale_x = 1.0`, and walks a 1×1 box per output pixel. The `for (int yy = y0; yy < y1; ++yy)` and `for (int xx = x0; xx < x1; ++xx)` loops both execute once, but each iteration carries the `std::min(..., std::max(...))` arithmetic to compute `x1/y1` and a re-fetched `src` row pointer.

**Impact:** ~10–20 % overhead vs a direct row-walk on the native path. Small but pure waste.

**Recommendation:** Split the function:
```cpp
if (target_w == img.width && target_h == img.height) {
    // hot path: tight 1-D loop over npix, no scale_x/scale_y, no box average
} else {
    // current downsample path
}
```

#### 🟡 P1 — `RenderedBitmap::bgra` is re-`assign`-ed (zero-fill) on every render
**Location:** `fits_render.cpp:40–41`
**Observation:** `out.bgra.assign(stride * h, 0)` zero-initializes the buffer the loop is about to overwrite anyway. For 36 Mpx that's ~144 MB written then immediately overwritten — ~50 ms wasted on memory bandwidth.

**Impact:** ~50 ms on every render of a 36 Mpx image.

**Recommendation:** Use `out.bgra.resize(stride * h)` (non-zero-initialized only if you reserve+resize a default-allocated `vector<uint8_t>` — actually `vector::resize` *does* value-initialize, which for `uint8_t` is zero-fill). To skip the fill, switch to a `std::unique_ptr<uint8_t[]>` allocated with `std::make_unique_for_overwrite<uint8_t[]>(n)` (C++20) or, for C++17, `new uint8_t[n]` wrapped in a `unique_ptr` with a deleter — the loop always writes the whole bitmap so zero-init is wasted. Alternatively, just don't worry about it if you go to the LUT path (the LUT path is so much faster the zero-fill becomes the new bottleneck, at which point it's worth removing).

#### 🟢 P2 — `to_u8` adds a branch where a single `clamp` would do
**Location:** `fits_render.cpp:10–14`
**Observation:** The `<= 0` and `>= 1` checks duplicate what `clamp` already did at line 82. Once the prior recommendations land, `to_u8` becomes `(uint8_t)(n*255.0f + 0.5f)` with no branches.

#### Note — Rendering is single-threaded
The render worker only runs one job at a time, on one core. With the LUT path the per-render time drops below the human perception threshold, so threading isn't urgent. If the LUT change still leaves you with multi-hundred-ms renders on 60 Mpx masters, `std::for_each(std::execution::par_unseq, ...)` over rows trivially parallelizes (MSVC's parallel STL is mature on Win10+).

---

### 2.2 `fits_core/src/fits_loader.cpp` — FITS loading

#### 🔴 P0 — Whole-file read + CFITSIO buffer + flipped buffer = 3× the file in RAM
**Location:** `fits_loader.cpp:187–207` (`load_from_file`), `fits_loader.cpp:126–131` (the flip)
**Observation:** `load_from_file` does:
1. `fread` the entire file into `std::vector<uint8_t> buf` (size N).
2. CFITSIO `fits_open_memfile` against `buf` (which then internally copies if it wants — historically CFITSIO does *not* take ownership; it reads from your buffer).
3. `fits_read_pix` into `std::vector<float> raw` (size = `width*height*4`; for a 36 Mpx float file that equals the file payload, ~144 MB).
4. `flipped` allocates another 144 MB and memcpy's row-by-row.

For a 200 MB raw mono float FITS, that's 200 MB + 144 MB + 144 MB ≈ **490 MB peak**, with ~400 ms of pure memcpy/load on a SATA SSD (less on NVMe but still measurable).

**Impact:** Hits the user every time they navigate Prev/Next. Slow on large masters; also pressures the working set in 32-MB-cap Property Handler runs.

**Recommendation:**
- For `load_from_file`, **don't slurp into a buffer**. CFITSIO accepts a filename directly: `fits_open_diskfile(&fptr, utf8_path, READONLY, &status)`. It does its own buffered I/O and you skip the temp buffer entirely. (You'll need to pass UTF-8 — `WideCharToMultiByte` once, ~free.) This saves the first 200 MB.
- For the flip, fuse it with the NaN sanitization and the min/max scan: one pass instead of three. Or even better, ask CFITSIO to read in reverse-row order via `fits_read_subset` — but the fused single pass is simpler:
```cpp
// Combined flip + finite check + min/max scan
float vmin = +INF, vmax = -INF;
for (long y = 0; y < height; ++y) {
    const float* src = raw.data() + (height - 1 - y) * width;
    float* dst = flipped.data() + y * width;
    for (long x = 0; x < width; ++x) {
        float v = src[x];
        if (!std::isfinite(v)) v = 0.0f;     // or some sentinel
        dst[x] = v;
        vmin = std::min(vmin, v);
        vmax = std::max(vmax, v);
    }
}
```
This collapses the current passes at lines 126–140 into one. Saves ~100 ms on a 36 Mpx image.
- Better yet: skip `raw` entirely. Allocate `flipped` once, then call CFITSIO row-by-row from bottom to top (`fits_read_subset`). Eliminates the second 144 MB. The loop becomes:
```cpp
for (long y = 0; y < height; ++y) {
    long fpix[2] = {1, height - y};
    long lpix[2] = {width, height - y};
    long inc[2] = {1, 1};
    fits_read_subset(fptr, TFLOAT, fpix, lpix, inc, &nulval,
                     flipped.data() + y * width, &anynul, &status);
}
```
Net savings: one 144 MB allocation gone, one full-buffer memcpy gone. ~200 ms on a 36 Mpx image.

#### 🟡 P1 — `long` from `naxes` cast to `int` without overflow check
**Location:** `fits_loader.cpp:143–144`
**Observation:** `naxes[0]` and `naxes[1]` are `long` (LP64 on Linux but 32-bit on Win64/MSVC ABI! — `LLP64`). Assigning to `int width = naxes[0]` is fine on Windows but the silent narrowing path is a footgun if the file claims >2 G pixels per axis (extremely large mosaics). The `static_cast<int>(width)` at line 143 truncates without checking.

**Impact:** Theoretical only — no real FITS file has >2 Gpx per axis. But `pixel_count()` is `size_t`, so a 200000×200000 image would silently overflow `int width` while the rest of the code assumes the value is correct.

**Recommendation:** Add an early bounds check:
```cpp
if (width > INT_MAX || height > INT_MAX ||
    static_cast<size_t>(width) * height > 0x40000000ULL) {  // 1 Gpx safety cap
    res.error = "Image too large"; return res;
}
```
Same comment applies to `xisf_loader.cpp`'s `parse_geometry`.

#### 🟢 P2 — `fits_read_key` for BZERO/BSCALE is run after the HDU search but before headers are enumerated
**Location:** `fits_loader.cpp:106–112`
**Observation:** `enumerate_headers` walks the entire HDU header at line 152, so BZERO/BSCALE are already going to be present in `image.headers`. The early-bound reads at 109–111 duplicate that work.

**Impact:** Trivial (CFITSIO header lookup is fast). Removing them is just code cleanup.

**Recommendation:** Drop the explicit reads; populate `bzero`/`bscale` from the enumerated `image.headers` via `find_header_double("BZERO", ...)` after enumeration.

#### 🟢 P2 — `FILE* ftell` returns `long` — 2 GB cap on 64-bit Windows
**Location:** `fits_loader.cpp:197`
**Observation:** `_wfopen_s`+`ftell` is limited to `LONG_MAX` (2 GB on MSVC). Files >2 GB silently get `ftell == -1` and the function bails with "Empty file" or wrong size. A 60-Mpx float master is only 240 MB so this is theoretical, but XISF "Master" stacks easily blow past 2 GB.

**Recommendation:** Use `_wfopen_s` + `_fseeki64` / `_ftelli64`, or switch to `CreateFileW` + `GetFileSizeEx` (which is what the shell ext path effectively does via `IStream::Stat`).

#### Note — Repeated `static_cast` clutter
The body of `load_from_fitsfile` is full of `static_cast<size_t>(width)`. A pair of local `const size_t W = width, H = height;` at the top makes the loops more readable and lets the compiler optimize better in `/W4`-clean code.

---

### 2.3 `fits_core/src/xisf_loader.cpp` — XISF loading

#### 🟡 P1 — Per-pixel `track(v)` and `store(v, i)` lambdas defeat vectorization
**Location:** `xisf_loader.cpp:209–268`
**Observation:** Every sample-format case looks like:
```cpp
for (size_t i = 0; i < npix; ++i) {
    float v = static_cast<float>(src[i]); store(v, i); track(v);
}
```
The `track` lambda is a closure over local `vmin/vmax`, branching on `isfinite`. The compiler *might* inline both lambdas, but on MSVC at /O2 the closure layout is not always seen through and the result is scalar code with a branch for every pixel.

**Impact:** On a 96 Mpx Float32 XISF (typical PixInsight master), the convert+scan loop runs at ~150 Mpx/s scalar where pure copy hits 5 GB/s (1.25 Gpx/s for floats). That's a ~10× ceiling.

**Recommendation:** Two passes are faster than one branchy pass when the data isn't already memory-bound:
```cpp
case PixelType::Float32: {
    const float* src = reinterpret_cast<const float*>(pix);
    std::memcpy(img.data.data(), src, npix * sizeof(float));   // ~5 GB/s
    float vmin = +INF, vmax = -INF;
    for (size_t i = 0; i < npix; ++i) {
        float v = img.data[i];
        // NaN-safe: NaN compares false in both directions, ignored naturally
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }
    // ... store vmin/vmax
}
```
The min/max loop autovectorizes under `/O2 /fp:fast` (use `std::min/std::max` which MSVC vectorizes; chained `if` does not vectorize). For non-Float32 paths, do conversion + scan in one autovectorizable loop:
```cpp
case PixelType::UInt16: {
    const uint16_t* src = reinterpret_cast<const uint16_t*>(pix);
    float* dst = img.data.data();
    for (size_t i = 0; i < npix; ++i) dst[i] = static_cast<float>(src[i]);  // vectorizes
    // then a separate min/max pass — also vectorizes
}
```
Estimated win: 2–4× on the XISF loader for non-trivially-sized files.

#### 🟡 P1 — `parse_location` uses `std::stoull` exceptions for the happy path
**Location:** `xisf_loader.cpp:58–61`
**Observation:** `stoull` throws on parse failure. The wrapping `try{} catch(...)` is fine, but on the success path it still incurs the EH overhead (modern MSVC is mostly zero-cost on success, but the lookup table emission is still noise). And in a Property Handler running inside Explorer, an unexpected `std::system_error` from a malformed location string would have been caught only because of this try.

**Impact:** Minor.

**Recommendation:** `std::from_chars(p, p + n, value)` — non-throwing, zero-allocation, faster. Available in C++17 for integers.

#### 🟢 P2 — `for (pugi::xml_node kw = ...)` could be hoisted
**Location:** `xisf_loader.cpp:159–166`
**Observation:** Loop is fine, just noting that each `next_sibling("FITSKeyword")` does a name compare per iteration. For files with hundreds of FITS keywords (common), that's a fixed overhead. pugixml has `select_nodes("FITSKeyword")` which is similar; not a big deal.

#### Note — The "v1 RGB ignored" branch silently throws away G and B
For thumbnails this is fine (mono looks like a luminance preview). But for the viewer, a one-channel-only render of an RGB image is visually wrong (the user expects to see all three). Tracker item for later, not a perf concern.

---

### 2.4 `fits_core/src/fits_stretch.cpp` — Auto-stretch

#### 🟢 P2 — Sampling does step-based stride; values at exact multiples are biased
**Location:** `fits_stretch.cpp:36–41`
**Observation:** Sampling at `step` intervals picks pixels at `0, step, 2*step, ...`. For images where structure aligns with that stride (rare in astro data but possible — bias frame patterns, vignetting), the median is biased. The function caps at 100k samples which is plenty; a low-discrepancy or shuffled sampler would be a minor robustness win.

**Recommendation:** Use a simple LCG to spread the picks, or pick `kTargetSamples` random indices with `std::sample`. Not urgent.

#### 🟢 P2 — `std::vector<float> work = samples;` is a full copy
**Location:** `fits_stretch.cpp:44`
**Observation:** Copying ~100k floats (400 KB) just to compute a median. `nth_element` mutates; a `std::vector<float> work(samples)` does the same copy. You could `std::move(samples)` into `work`, but then you can't reuse `samples` for the `dev` computation. Alternative: in-place compute median, then compute deviations in `samples` itself (since after `nth_element`, only the position-`mid` value matters).

**Impact:** ~100 µs. Negligible. Note only.

#### 🟢 P2 — `kMadToSigma * mad` mixed-precision
**Location:** `fits_stretch.cpp:57`
**Observation:** `kMadToSigma` is `1.4826f`; `median` and `mad` are `float`. Fine. Just observing that the project uses `double` for analysis stats but `float` here. Consistent with the rest of the file.

---

### 2.5 `fits_core/src/analysis.cpp` — Pixel stats + star detection

This file does the most pixel work after rendering, and it's run once per file load (cache miss). It's also the biggest memory consumer.

#### 🔴 P0 — `compute_pixel_stats` allocates one float per valid pixel just for the median
**Location:** `analysis.cpp:65–72`
**Observation:** `work.reserve(valid); ... work.push_back(v);` for every finite pixel. For a 36 Mpx image with no NaNs, that's 144 MB allocated. Same again for `dev`. Then `nth_element` runs over 36 M floats — ~250 ms on a fast desktop. The function is called from both the viewer's first-load path and the Property Handler's cache-miss path (FITS only).

**Impact:** ~144 MB peak (a second copy beyond the image's `data`), ~300–500 ms on a 36 Mpx file. Hits every uncached first-time-open.

**Recommendation:** Switch to **subsampling**. Astrophotography pixel statistics are perceptually identical between a 100 % scan and a 1 % stride scan unless the image is pathological (which it isn't — that's astro data). Reuse `compute_auto_stretch`'s approach: take ~200k samples, do `nth_element` on those. The median is statistically indistinguishable to the user, and the MAD/sigma estimate is well within its own noise. The pass becomes:
- ~200k samples (instead of 36 M)
- 800 KB allocation (instead of 144 MB)
- ~5 ms for the `nth_element` (instead of ~250 ms)

If you absolutely want the exact median, look at boost-style p²-quantile or just accept the sample. For the Welford mean/stddev and exact min/max, those are already one-pass — keep them.

```cpp
constexpr size_t kStatsSamples = 200000;
const size_t step = std::max<size_t>(1, n / kStatsSamples);
std::vector<float> work;
work.reserve(n / step + 1);
for (size_t i = 0; i < n; i += step) {
    const float v = img.data[i];
    if (std::isfinite(v)) work.push_back(v);
}
// ... nth_element on work
```

The cache key already encodes file identity, so reproducibility across runs is unchanged.

#### 🔴 P0 — `detect_stars` preallocates an N-element `label` vector and an unbounded number of `Blob`s
**Location:** `analysis.cpp:131, 165`
**Observation:**
- `std::vector<int32_t> label(N, -1)` — for 36 Mpx that's 144 MB.
- `std::vector<Blob> blobs(uf.parent.size())` — sized to the total number of make_set calls (worst case is roughly the threshold-hit pixel count divided by some small constant). For a star-rich image with 50k thresholded pixels and 1k merges, that's 50k `Blob` slots, each holding three `vector`s (24 bytes of overhead each). Most slots are unused after path compression, but they still cost cache lines and the per-blob `xs/ys/vs.push_back` causes amortized O(1) reallocs that aren't free at scale.

**Impact:** On a 36 Mpx image with 5000 detected stars, `label` dominates at 144 MB and the per-blob vector overhead adds another 1–2 MB of allocator fragmentation. Star detection takes ~300–600 ms; ~40 % of that is allocator and cache traffic.

**Recommendations:**
1. **Use a 1-byte mask + a small int16/int32 label per ABOVE-THRESHOLD pixel**, not per all pixels. First pass: count above-threshold pixels; if there are very few, skip the bitmap approach entirely. Otherwise, label only those.
2. **Use a hash-map (or two-row sliding label window) for label assignment**, since star pixels are very sparse (well under 1 %) — paying for an N-element label array is wasteful.
3. **Replace `Blob{ xs, ys, vs }` with a flat `vector<PixelEntry>` indexed via a per-blob start/end range**, since you only iterate each blob once. Eliminates the per-blob heap allocations:
   ```cpp
   struct PixelEntry { int x, y; float v; };
   std::vector<PixelEntry> pixels;          // single flat buffer
   std::vector<int>        blob_begin;      // offset into pixels[]
   ```
4. **Reject min/max blob size *before* gathering pixels.** A two-pass count-then-fill removes the `xs/ys/vs` realloc for blobs that get discarded anyway.

Combined with the stats subsampling above, a 36 Mpx first-load analysis should drop from ~700–1000 ms to ~150–250 ms. The cache covers repeat opens, but **first open of every new image** is what the user feels — and the Property Handler in Explorer hits this every time someone scrolls a fresh folder.

#### 🟡 P1 — `std::sort(rp.begin(), rp.end(), ...)` for HFR is O(n log n) per blob
**Location:** `analysis.cpp:246–247`
**Observation:** For each blob, the code sorts all pixels by distance from centroid to find the half-flux radius. With `kMaxPixels = 200`, each sort is ~200 elements — fast. But the sort allocates internally (it's a quicksort, no allocations in libc++/MSVC STL).

**Impact:** Tiny. ~200 elements × 5000 stars × ~30 comparisons = 30 M compares total. Maybe 5 ms. Not a hotspot.

**Recommendation:** Leave it. Just noting.

#### 🟡 P1 — `compute_pixel_stats`: per-pixel branch chain inside Welford
**Location:** `analysis.cpp:44–55`
**Observation:** The Welford loop has:
- `isfinite` branch
- min/max updates with an unfortunate `else if` chain that doesn't autovectorize.

**Impact:** Welford is sequential by definition, so it can't be vectorized cleanly anyway. The `isfinite` is unfortunate but cheap if the data is sanitized at load (see fits_render P0 #2).

**Recommendation:** After load-time NaN scrub, drop the `isfinite` here. Welford stays scalar but becomes ~40 % faster. min/max can be split out into a separate vectorizable loop.

#### 🟡 P1 — `cmin/cmax` "first-equal" counter is subtly broken
**Location:** `analysis.cpp:51–54`
```cpp
if (v < vmin) { vmin = v; cmin = 1; }
else if (v == vmin) { ++cmin; }
```
For floats, `==` after `<` is fine for monotone-decreasing minima updates, but exclusively uses `else if`. If `vmin` is updated mid-stream, the previous count is correctly reset to 1 — good. But: the count counts pixels exactly equal to the *current* minimum, which can include pixels seen *before* the current min was set, which are by definition strictly greater than the current `vmin`, so they don't increment. Wait — actually no, they do not increment, because they were never compared against the new `vmin`. So you only count pixels equal to `vmin` that occur *after* `vmin` was last updated. That's wrong.

**Impact:** `min_count` / `max_count` are wrong any time the min/max is updated mid-stream. In practice astro images have a single saturation plateau (max_count is meaningful), and zeros at the edges (min_count is meaningful), and the plateau is usually contiguous — but if there's a single noise pixel pulling the min lower mid-image, the count of the "real" floor pixels is lost.

**Recommendation:** Either compute exact counts in a second pass (after vmin/vmax are known), or accept the approximation and rename to `min_count_after_last_update`. The two-pass approach is what the comments seem to imply happens but it doesn't. A second pass on 36 Mpx is ~50 ms — acceptable for an analysis that runs once per file.

```cpp
// after the Welford loop:
uint64_t cmin = 0, cmax = 0;
for (size_t i = 0; i < n; ++i) {
    float v = img.data[i];
    if (!std::isfinite(v)) continue;
    if (v == vmin) ++cmin;
    if (v == vmax) ++cmax;
}
```

This second pass also autovectorizes nicely.

#### 🟢 P2 — `UnionFind::reserve(N/32+16)` heuristic is fragile
**Location:** `analysis.cpp:133`
**Observation:** Sized for ~3 % star pixel density. Fine for typical sky, but very dense fields (open clusters, dense Milky Way) trigger reallocs. Not a correctness issue, but `push_back` on a moving vector inside the hot loop is wasted reallocs.

**Recommendation:** A first counting pass over the threshold gives an exact upper bound for `make_set` calls. Cheap.

---

### 2.6 `fits_core/src/cache.cpp` — SQLite analysis cache

#### 🟡 P1 — `sqlite3_bind_text` with `SQLITE_TRANSIENT` copies the key on every call
**Location:** `cache.cpp:126, 159`
**Observation:** The cache key is a 16-char hex string. `SQLITE_TRANSIENT` instructs SQLite to copy. Since the key lives on the stack of the caller and is short-lived, `SQLITE_STATIC` would be a footgun. But `SQLITE_TRANSIENT` here is correct and cheap (16 bytes).

**Impact:** Negligible.

**Note:** Not actually a problem. Listed because the comment in the code reads as if `SQLITE_TRANSIENT` is a perf concern; it isn't here.

#### 🟢 P2 — `ensure_open()` is called under the mutex on every lookup/store
**Location:** `cache.cpp:121, 153`
**Observation:** `open_attempted_` makes the body a single bool check after the first call. Fine. But the lock is still acquired even for the early-out. Splitting the open path (double-checked) into a separate `std::call_once` would let the hot path skip the mutex for the open check. Probably not worth it given the lock is short and uncontended.

#### Note — `time(nullptr)` in `bind_int64` (line 162) is `time_t` which on MSVC is 64-bit. Safe. Just confirming.

#### Note — WAL mode + `synchronous=NORMAL` is the right call for a metadata cache shared with Explorer/Indexer. Good.

---

### 2.7 `viewer/src/ViewerWindow.cpp` — the GUI

#### 🔴 P0 — Double-computed stretch on every load
**Location:** `ViewerWindow.cpp:660–663` (worker), then `apply_stretch` after.
**Observation:** The load worker runs `compute_auto_stretch(result->image)` to populate `result->stretch`, then `render_to_bgra(result->image, result->stretch)`. Then `on_load_finished` assigns `image_`, `stretch_`, `rendered_` — fine, no re-render. But: the very next user action that calls `apply_stretch()` (a stretch-mode toggle) **recomputes auto-stretch on the now-shared `*image_`**. That's fine. The issue is subtler: on the very first load, **`compute_auto_stretch` is run in the worker, then again from nothing if the user toggles RAW→Auto**. Not a bug, just two computes for one purpose.

**More importantly:** if the user's default is Auto and they immediately use the histogram slider, the new stretch is run by `apply_custom_stretch` → `request_render`. The original render result is already on screen, so the spinner doesn't kick in. Good. But the worker also runs `compute_auto_stretch` in `apply_stretch` (line 537) under `image_->...` — and `compute_auto_stretch` walks the data and `nth_element`s. That's tens to hundreds of ms **on the UI thread** for a 36 Mpx image, because `apply_stretch` is called from `on_command` (the WM_COMMAND handler).

**Impact:** Clicking the toolbar "Auto" button on a 36 Mpx image stalls the UI for ~200 ms. Worse on master frames.

**Recommendation:** `compute_auto_stretch` should run on the render worker:
```cpp
void ViewerWindow::apply_stretch() {
    if (!image_ || image_->data.empty()) return;
    if (stretch_mode_ == StretchMode::Auto) {
        // schedule on worker: compute_auto_stretch then render
        request_render_auto();    // new method
    } else {
        stretch_ = fitsx::StretchParams{};
        histogram_.set_params(stretch_);
        refresh_stretch_toolbar();
        request_render(stretch_);
    }
}
```
The worker grabs `image_` (cheap, just a shared_ptr copy), runs `compute_auto_stretch`, then `render_to_bgra`. Posts back to the UI with the new params *and* bitmap. The toolbar/histogram are updated only when the result lands.

Alternative quick win: **cache the auto-stretch params on the FitsImage itself** at load time (computed in the worker already). `apply_stretch` becomes a no-op state restore — instant.

#### 🟡 P1 — `std::thread(...).detach()` for every navigation
**Location:** `ViewerWindow.cpp:646–681`
**Observation:** Every `load_file` spawns a fresh thread and detaches it. On WM_DESTROY the thread might still be running (the `load_gen_` check + `IsWindow` guards drop the stale post). But: if the user spam-presses Next 10 times in a second, you spawn 10 threads all racing to read big files from disk. They thrash the disk queue against each other, slow down each load, and consume up to 10×450 MB peak. The "newest wins" check at line 687 happens *after* all the work is done.

**Impact:** Burst loading is slower than serial loading would be. Disk thrash is real on HDDs and SATA SSDs; less so on NVMe but still noticeable. RAM peak is alarming on large files.

**Recommendation:** Move loading onto the same dedicated worker thread as rendering (or a sibling). The worker holds a queue of size 1 (last-write-wins), exactly the same pattern you already use for render. The user can press Next 10 times: only the last request actually executes.

```cpp
// In the worker:
// while (!quit) {
//   wait for (load_pending || render_pending || quit)
//   take pending path (load wins if both, since render depends on image)
//   do work, post back
// }
```

Bonus: this naturally serializes loads so they don't trample the disk cache.

#### 🟡 P1 — `WM_DESTROY` does `PostQuitMessage` but the detached load thread can outlive the window
**Location:** `ViewerWindow.cpp:455–458`
**Observation:** WM_DESTROY → PostQuitMessage → message loop exits → `run_message_loop` joins `render_thread_`. But the **detached load thread** may still be inside `load_from_file` (a 450 ms read for a 200 MB file). The lambda captured `hwnd_target` by value — the HWND is invalid but `PostMessageW` on an invalid HWND is just an `ERROR_INVALID_WINDOW_HANDLE` from `PostMessage`. The `LoadResult*` that was supposed to be cleaned up via `unique_ptr` on the UI side then **leaks**, since the lambda's `result.release()` already gave up ownership.

**Impact:** Per-leaked-load: full image data (~150 MB), the rendered bitmap (~150 MB), etc. On normal app exit this is reclaimed by the OS. On Explorer-launched-then-closed scenarios it's noise. But strictly it's a leak and the leaked thread has detached state.

**Recommendation:** Don't detach. Use the same single-worker pattern as rendering. On shutdown, set `quit`, notify the cv, join the thread (drop or process the in-flight result safely).

#### 🟢 P2 — `render()` has a no-op expression
**Location:** `ViewerWindow.cpp:833`
```cpp
const float cx = vpr.left + vp_w * 0.5f + (bb_w - bb_w) * 0.0f - offset_x_ * zoom_;
```
`(bb_w - bb_w) * 0.0f` is zero. Looks like leftover scaffolding for sideways-bbox centering.

**Impact:** None, but it looks wrong on review and `(void)bb_h;` at line 851 confirms this was unfinished. The `bb_h` is also computed but never used.

**Recommendation:** Remove the dead expression; either use bb_w/bb_h for clamping (they already are, in `clamp_offset`) or delete the local.

#### 🟢 P2 — Spinner timer fires at 60 Hz even when load is fast
**Location:** `ViewerWindow.cpp:639`
**Observation:** `SetTimer(hwnd_, kTimerSpinner, 16, nullptr)` fires every ~16 ms during load. For a fast load (under 100 ms), the spinner is invisible anyway — every WM_TIMER is wasted work.

**Recommendation:** Delay starting the timer by ~150 ms (post a delayed event via `SetTimer` with a longer initial period that the WM_TIMER handler then resets to 16 ms). Or check `loading_` duration before firing. Minor.

#### 🟢 P2 — `kSpinnerRadius` is unused if `loading_` is false but `CreateSolidColorBrush` runs every paint
**Location:** `ViewerWindow.cpp:856–894`
**Observation:** Inside `if (loading_)`. So only when loading. Two brush-create/release pairs per spinner frame at 60 Hz = 120 allocs/sec. D2D pools internally but it's still pointless. The veil brush in particular is identical every frame.

**Recommendation:** Cache the two `ID2D1SolidColorBrush` as members; release on `release_render_target`. Standard D2D pattern.

#### 🟢 P2 — `WM_APP_RENDER_DONE` posts a heap-allocated `RenderResult*` and the receiver does `std::unique_ptr<RenderResult> r(raw)` — good. But the stale-result early return at line 618 leaks the rendered bitmap of the stale result (it's inside the unique_ptr — actually no, the unique_ptr destructor runs on return, so it's freed correctly). Reviewed: this is fine.

#### Note — `safe_release` template at line 203 is a clean RAII alternative but COM pointers are still raw `*`. Using `Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget>` from wrl/client.h removes a class of manual-release bugs at zero runtime cost. Not urgent (the current code looks tight) but worth considering for the next pass.

#### Note — `request_render` does `render_gen_latest_.fetch_add(1, relaxed) + 1` then stores under the mutex. That's fine since the worker reads `render_gen_pending_` under the mutex and posts back with that value, and `on_render_finished` compares against `render_gen_latest_.load(relaxed)`. The relaxed memory order is acceptable here because all real synchronization goes through the mutex + cv. Good.

---

### 2.8 `viewer/src/Histogram.cpp` — Histogram popup

#### 🟡 P1 — Three PathGeometry+GeometrySink objects created per paint
**Location:** `Histogram.cpp:443–458`
**Observation:** Each of the three diamond handles allocates an `ID2D1PathGeometry`, opens an `ID2D1GeometrySink`, fills four points, closes, releases. During a slider drag, paint can happen ~60 Hz. That's 180 D2D geometry allocations per second.

**Impact:** D2D internally pools small geometries, but the COM AddRef/Release dance and the small-object allocation churn show up in profilers as ~5 % of paint time. Visible micro-stutter on slow GPUs/integrated graphics.

**Recommendation:** Cache one diamond geometry centered at origin (created once at first paint), then translate in the render loop:
```cpp
// once:
ID2D1PathGeometry* diamond_geom_ = nullptr;
d2d_factory_->CreatePathGeometry(&diamond_geom_);
// ... open sink, add the diamond around (0,0), close
// release in release_d2d()

// in render():
for (int i = 0; i < 3; ++i) {
    const float cx = norm_to_x(vals[i]);
    rt_->SetTransform(D2D1::Matrix3x2F::Translation(cx, cy_handle));
    rt_->FillGeometry(diamond_geom_, br_handle);
}
rt_->SetTransform(D2D1::Matrix3x2F::Identity());
```
Or even simpler: use `FillEllipse` (no geometry needed) — circles render just as nicely as diamonds at 14×18 px and require zero allocations.

#### 🟡 P1 — `recompute_bins` does a full-image scan with NaN check + division per pixel
**Location:** `Histogram.cpp:353–371`
**Observation:** For 36 Mpx: per pixel, `isfinite`, subtract, multiply, cast to int, clamp, increment a float bin. ~300 ms scalar. Runs every time `set_image` is called — i.e., on every file load.

**Impact:** Adds ~300 ms to load completion before the histogram is usable. But since the popup is hidden by default, this work is wasted unless the user actually has it open.

**Recommendation 1 (low effort):** Defer `recompute_bins` until the popup is first shown:
```cpp
void HistogramWindow::set_image(const fitsx::FitsImage* image) {
    pending_image_ = image;
    bins_dirty_ = true;
    if (is_visible()) ensure_bins();
}
void HistogramWindow::show() {
    if (bins_dirty_) ensure_bins();
    ::ShowWindow(...);
}
```
Avoids 300 ms on every load when the popup is hidden.

**Recommendation 2 (when computed):** Same SIMD-friendly pattern as the renderer — precompute `scale = 256 / span`, drop NaN check (after load-time sanitize), use `uint32_t` bins (autovectorizable), then log1p in a separate pass:
```cpp
std::vector<uint32_t> int_bins(256, 0);
const float scale = 256.0f / span;
for (size_t i = 0; i < img.data.size(); ++i) {
    int bin = static_cast<int>((img.data[i] - vmin) * scale);
    bin = std::min(255, std::max(0, bin));
    ++int_bins[bin];
}
// then log1p over 256 entries — trivial
```
Native int counter is ~3× faster than the current float bin. Combined with deferral, this is a clean P1 fix.

#### 🟢 P2 — Brushes are created+released every paint
**Location:** Throughout `render()`
**Observation:** `br_panel`, `br_bar`, `br_guide`, `br_clip`, `br_strip`, `br_handle` are all freshly created via `CreateSolidColorBrush` on every paint. Same as ViewerWindow's spinner brushes.

**Recommendation:** Cache them on the render target. Standard D2D advice. Probably saves ~1 ms per paint at 60 Hz drag.

#### 🟢 P2 — `release_d2d` releases the factory
**Location:** `Histogram.cpp:157–160`
**Observation:** `release_d2d()` calls `d2d_factory_->Release()`. ViewerWindow keeps the factory across the lifetime of the app and only the render target/bitmap are recreated. The histogram should match — recreate only the HwndRenderTarget on D2DERR_RECREATE_TARGET, keep the factory.

**Impact:** Tiny, but inconsistent with ViewerWindow.

**Recommendation:** Mirror the ViewerWindow pattern.

---

### 2.9 `shell_ext/` — COM shell extensions

Overall the COM scaffolding is correct: standard ATL-free pattern, manual ref counting, `IInitializeWithStream` everywhere, correct `Apartment` threading model in registration, AppID hookups for low-IL surrogates. Comments are illuminating. A few items:

#### 🔴 P0 (latent) — `FitsPropertyHandler::Initialize` swallows exceptions from `populate()` only at the catch boundary
**Location:** `PropertyHandler.cpp:151–317`
**Observation:** `populate()` is wrapped in `try{...}catch(...){}` — good. But operations inside `populate()` (vector allocations, `utf8_to_wide`, `InitPropVariantFromString`) can throw `std::bad_alloc`. If a throw happens after some `props_` entries are added, the half-filled `props_` is then read by `GetCount/GetAt/GetValue` — the caller sees a partial property store. That's the documented behavior per the comment, and Explorer tolerates it.

**Impact:** Acceptable. Note only.

#### 🟡 P1 — `IThumbnailProvider::GetThumbnail` doesn't honor `WTSAT_ARGB` (alpha)
**Location:** `ThumbnailProvider.cpp:56`
**Observation:** Returns `WTSAT_RGB` and produces opaque BGRA. That's fine — astro thumbnails are inherently opaque. But the Explorer overlay (e.g., the icon "shortcut" arrow, modified-status badges) blends against the alpha channel. With `WTSAT_RGB`, Explorer treats the alpha as garbage. The BGRA buffer happens to have alpha = 255 from `render_to_bgra`, so it's safe by accident.

**Recommendation:** Either set `WTSAT_ARGB` (since alpha is valid and = 255 everywhere) or `WTSAT_UNKNOWN` and let Explorer figure it out. `WTSAT_RGB` tells the framework to ignore alpha; that's fine here. Note only.

#### 🟡 P1 — `StreamBuffer::init` uses `buffer_.resize(want_bytes)` which zero-fills before the loop overwrites
**Location:** `StreamBuffer.cpp:21`
**Observation:** A 32 MB resize zero-fills 32 MB. Wasted ~10 ms in a Property Handler that is supposed to be snappy. On thumbnail/preview where the cap is 1 GB, this is up to ~300 ms wasted on zero-fill.

**Impact:** Up to ~300 ms per thumbnail/preview unrelated to the actual stream read.

**Recommendation:** Replace `vector<uint8_t>` with a buffer that allows uninitialized allocation. Easiest C++17:
```cpp
class StreamBuffer {
    struct NoInit { uint8_t v; };
    static_assert(sizeof(NoInit) == 1);
    std::vector<NoInit> buffer_;     // default-init = no zero-fill
    // ...
};
```
Or use `std::unique_ptr<uint8_t[]>` allocated with `new uint8_t[n]` (POD: zero-init is not implicit) — but you lose the convenience of `resize/shrink_to_fit`. The `NoInit` trick is the cleanest.

#### 🟡 P1 — Thumbnail provider reads the full file even when only the header is needed
**Location:** `ThumbnailProvider.cpp:49`
**Observation:** `buf_.init(stream)` reads up to 1 GB. For a 36 Mpx FITS that's 144 MB read just to render a 256-pixel thumbnail. CFITSIO needs the full pixel data, fair enough — but XISF doesn't if you're only displaying a downscaled thumbnail and you're willing to do nearest-neighbor decimation while reading sparse pixel rows.

**Impact:** A folder of 50 large XISF files makes Explorer's thumbnail cache fill at ~2 s per file (mostly disk I/O). With sparse-row reading it could be ~100 ms per file.

**Recommendation (longer-term):** For XISF thumbnails, parse the header to find the pixel block offset, then `IStream::Seek` + `Read` only the rows you actually need for the downscaled output (one row per `scale_y` step). Avoids reading 99 % of the file. Not trivial; defer to v2.

#### 🟢 P2 — `FitsPreviewHandler::WndProc` calls `CreateSolidBrush(self->bg_)` per WM_PAINT
**Location:** `PreviewHandler.cpp:88–90`
**Observation:** Same brush-leak class as the histogram. Cache as a static at minimum.

#### 🟢 P2 — `SetWindowSubclass` subclasses on the toolbar and listview never `RemoveWindowSubclass` on shutdown except via the WM_NCDESTROY handler
**Location:** `ViewerWindow.cpp:142, Toolbar.cpp:57`
**Observation:** The WM_NCDESTROY handler does the `RemoveWindowSubclass` cleanup. Confirmed correct.

#### Note — `FitsPropertyHandler` re-implements `IPropertyStoreCapabilities` with `IsPropertyWritable` returning S_FALSE for *every* key. This is correct: it advertises "read-only" to Explorer's column editor UI.

#### Note — `Registration.cpp` uses `HKEY_LOCAL_MACHINE`, requiring elevation. Reasonable for shipping but means dev install needs admin. Not a finding, just confirming the choice.

#### Note — `module_path()` uses a fixed `wchar_t buf[MAX_PATH]`. On Win11 22H2+ with long-path support enabled, `GetModuleFileNameW` can return >MAX_PATH for paths under `\\?\`. Real-world the DLL is always under `C:\Program Files\WinStellar\` so this is fine. Just noting.

---

### 2.10 `tools/xisf_bench.cpp`

Looks fine. One nit:

#### 🟢 P2 — `std::ifstream` is being benchmarked, which is famously slow
**Location:** `xisf_bench.cpp:31–37`
**Observation:** `std::ifstream` on Windows with MSVC is significantly slower than `CreateFileW`+`ReadFile` (the iostream layer adds locale + state-machine overhead). If you're using this benchmark to compare against the production path, results may understate production speed since production uses `fread`.

**Recommendation:** Use `fread` (matching `fits_loader.cpp`) or even better, `CreateFileW`+`ReadFile`. Benchmark output will be more comparable to production hot path.

---

### 2.11 `CMakeLists.txt` (root + per-subdir)

#### 🟢 P2 — No `target_compile_features(... cxx_std_17)`
**Location:** root `CMakeLists.txt`
**Observation:** Global `set(CMAKE_CXX_STANDARD 17)` works, but `target_compile_features` per target is more idiomatic and propagates properly to consumers if `fits_core` is ever turned into a vcpkg/CPM package.

**Recommendation:** Optional. Add `target_compile_features(fits_core PUBLIC cxx_std_17)` etc. Not urgent.

#### 🟢 P2 — `/MP` is added globally but no `/Gw` (data COMDAT folding) or `/Gy` (function-level linking)
**Location:** root `CMakeLists.txt:61`
**Observation:** `/Gy` is default-on under `/O2`, so already enabled in Release. `/Gw` is *not* default and helps with link-time dead data stripping. Tiny binary-size win.

**Recommendation:** Add `/Gw` to `add_compile_options` for Release. Negligible runtime perf but cleaner ELF/PE.

#### 🟢 P2 — No LTCG/LTO
**Location:** root `CMakeLists.txt`
**Observation:** No `INTERPROCEDURAL_OPTIMIZATION ON` or `/GL` + `/LTCG` for Release. LTCG can give 5–15 % on tight numeric loops (the renderer in particular benefits).

**Recommendation:**
```cmake
include(CheckIPOSupported)
check_ipo_supported(RESULT lto_ok)
if(lto_ok)
    set_property(TARGET fits_core WinStellar WinStellarShellExt
                 PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()
```
Caveat: longer link times; some CFITSIO/sqlite3 symbols may have issues with LTCG. Test first.

#### 🟢 P2 — No /arch:AVX2
**Location:** root `CMakeLists.txt`
**Observation:** Default x64 MSVC targets SSE2. Windows 11 minimum CPU requirement is Intel 8th gen / AMD Zen, both of which support AVX2. Targeting AVX2 unlocks 8-wide float operations for the renderer/analysis loops.

**Recommendation:** Add `/arch:AVX2` to Release builds. With the LUT path, the renderer becomes memory-bandwidth-bound where SIMD matters most. Caveat: a hypothetical Win11 install on a pre-AVX2 CPU (e.g., older Atom-based devices) would fail with an "Illegal Instruction" exception at first SIMD call — but Win11 doesn't formally support such CPUs anyway. If you want to be cautious, conditionally compile a `/arch:AVX2` variant of the renderer and dispatch at runtime via `__cpuid`. Probably overkill.

#### Note — `/permissive- /W4 /Zc:__cplusplus /utf-8` is exactly the right combo. Confirmed.

#### Note — `MSVC_RUNTIME_LIBRARY` propagated correctly across targets including tests. Good.

---

## 3. Cross-cutting recommendations

### 3.1 Sanitize NaN/Inf once at load
The single most impactful "downstream-free" change. Both the renderer and analysis loops can drop their `isfinite` checks if pixels are sanitized once in `fits_loader.cpp` / `xisf_loader.cpp`. Combined effect: ~40 % faster render, ~30 % faster Welford, ~30 % faster histogram bins. Cost: one extra pass at load, fused with the flip/copy that's happening anyway.

### 3.2 Make every cross-thread image use a single shared `std::shared_ptr<const FitsImage>`
Already done. Note: this means the `FitsImage::data` vector should arguably be `std::shared_ptr<float[]>` instead of a `std::vector<float>` to avoid the vector's exclusive-ownership semantics — but the current `shared_ptr<const FitsImage>` wrapper achieves the same effect for the whole struct, so this is fine.

### 3.3 Centralize allocator policy for large buffers
The codebase repeatedly resizes `vector<float>` of width×height. For 36 Mpx that's 144 MB per allocation, repeatedly fragmenting/coalescing the heap. Consider:
- A small "image-data arena" via `VirtualAlloc(MEM_RESERVE|MEM_COMMIT)` for image buffers, then `VirtualFree`. The Windows heap is great at small allocations but a single large allocation goes straight to VirtualAlloc anyway, so this is a small-and-rare win. Not urgent.
- Or just `std::pmr::monotonic_buffer_resource` for the analysis pass's temporary buffers (work, dev, label, blobs).

### 3.4 Align critical buffers for SIMD
If/when you start adding intrinsics for the renderer (or rely on autovectorization with stricter alignment), `FitsImage::data` should be 32-byte aligned (AVX2) or 64-byte (AVX-512). `std::vector<float>` only guarantees `alignof(float) == 4`. A custom allocator (or `_aligned_malloc` wrapped in a unique_ptr) gives 32-byte alignment cheap.

### 3.5 Consider a profiling pass
The findings above are based on static reading + estimation. Before investing in the larger items (LUT, analysis subsampling), capture a real profile with Visual Studio's CPU Usage tool on a 36 Mpx FITS open + drag. The hottest functions will confirm or refute my priorities. Likely top-5 by self-time:
1. `render_to_bgra` inner loop
2. `compute_pixel_stats` (the second `nth_element` over 36 M elements)
3. `detect_stars` blob-pixel gathering
4. `flipped` copy in `fits_loader`
5. Recompute_bins in histogram

### 3.6 Modern C++17 / `[[nodiscard]]` etc.
- Mark `parse_xisf_header`, `compute_auto_stretch`, `run_analysis`, `render_to_bgra`, `compute_cache_key` as `[[nodiscard]]` — they all return values that are meaningless to discard. Several callers in tests/tools today silently drop them.
- `LoadResult::success` could become a `std::variant<FitsImage, std::string>` — but that's a public-API churn for a stylistic win. Not worth it.
- `FitsImage::find_header(const char*)` could take `std::string_view` for consistency with the rest of the API.
- `enumerate_astro_siblings` builds a vector of `std::wstring` then sorts. Could be a `std::vector<std::wstring_view>` over a backing arena — minor.

### 3.7 D2D resource caching
Three places (`ViewerWindow::render`, `Histogram::render`, `PreviewHandler::WndProc`) all create solid color brushes per paint. Consolidate via a small `D2DBrushCache` helper that vends ID2D1SolidColorBrush by COLORREF and is released alongside the render target. ~3 ms/paint savings, but more importantly it removes a class of "I added a new brush and forgot to release it on a fail path" bugs.

### 3.8 Threading: one worker thread, two work types
The current model is one detached load thread per nav + one persistent render thread. Consolidate to a single persistent worker that handles a queue of size 1 (latest-wins) of (load|render) jobs. Benefits:
- Eliminates the detached-thread shutdown race.
- Naturally serializes I/O so spam-clicks don't thrash the disk.
- Same coalescing semantics as today for render.
- Simpler shutdown (one join).

---

## 4. Quick wins (ordered by ROI)

1. **LUT-based renderer in `render_to_bgra`.** 4–8× perf on every stretch change. ~30 lines of code. *Highest ROI.*
2. **Sanitize NaN at load.** Drop `isfinite` from all three hot loops (render, Welford, histogram bins). Net ~30–50 % win across the board. ~5 lines per loader.
3. **Subsample `compute_pixel_stats` for median/MAD.** 100× faster, indistinguishable result. Frees 144 MB. ~10 lines.
4. **Skip the `assign(0)` zero-fill in `RenderedBitmap`.** ~50 ms saved per render. ~3 lines.
5. **Cache D2D solid-color brushes** in ViewerWindow + Histogram. ~3 ms/paint, plus less brush churn.
6. **Cache the diamond geometry in Histogram::render.** One alloc instead of 180/sec while dragging.
7. **Defer `recompute_bins` until the histogram popup is first shown.** ~300 ms off every file load when popup is hidden (the common case).
8. **Fuse flip + min/max + NaN scrub in `fits_loader.cpp`.** ~100 ms saved per FITS load.
9. **Cache `compute_auto_stretch` result on the FitsImage.** Makes "Auto" button toggle instant instead of ~200 ms stall.
10. **Move auto-stretch off the UI thread.** Makes the toolbar Auto toggle non-blocking. ~30 lines of refactor.

---

## 5. Longer-term / architectural notes

### 5.1 RGB rendering
The XISF loader silently drops G/B channels. For proper RGB viewing of master frames, the pipeline needs to be a 3-channel `FitsImage` (or a `MultiChannelImage` variant), with the renderer producing BGRA from independent per-channel stretches. Touches the histogram (3 separate curves) and the auto-stretch (per channel). Big-ish change; reasonable to defer.

### 5.2 Tiled / chunked image storage
For >1 Gpx mosaics, holding the whole `vector<float>` in RAM stops being viable. A tiled storage (e.g., 256×256 tiles, loaded on demand) plus a tile-aware renderer would unlock arbitrary-size files. Long-term roadmap item.

### 5.3 Parallel renderer with parallel STL
Once the LUT path lands, the renderer is ~50–80 ms on a single core for 36 Mpx. `std::for_each(std::execution::par_unseq, rows.begin(), rows.end(), ...)` over the row range is a one-line change that linearly speeds up to ~core_count×. On an 8-core laptop, 36 Mpx renders in ~10 ms. Slider drag becomes butter-smooth at 60 fps.

### 5.4 Save out-of-band thumbnails
The cache already keys analysis results by content hash. Saving the rendered BGRA thumbnail alongside (one column in SQLite or a sibling `.thumb` file) makes the second-and-later Explorer thumbnail render instant. Significant win for the shell-ext UX. Implementation: bump `kAnalysisSchemaVersion` and add a BLOB column.

### 5.5 Replace per-file `compute_cache_key` open with already-open data
`compute_cache_key_from_file` opens the file again for hashing after the viewer just read it. Pipe the buffer through the existing load path so the same bytes are hashed without re-I/O. Minor (8 KB read is fast) but cleaner.

### 5.6 Star detection: separate label-pass from analyze-pass
Today `detect_stars` builds labels, then resolves, then gathers per-blob pixel lists, then computes per-blob stats. Steps 3 and 4 can be a single streaming pass: walk the image again, look up each above-threshold pixel's root label, and accumulate per-root sufficient statistics (count, sum_w, sum_wx, sum_wy, sum_wx², sum_wy², sum_wxy). HFR needs the per-pixel distance list, so for that one you do need to keep the pixel lists for blobs that survive the count threshold — but if you reject by count first, you only keep the pixels for ~5000 surviving blobs. Saves the rejected-blob `xs/ys/vs.push_back` overhead.

### 5.7 WIC for downscaled thumbnails?
Direct2D's WIC integration can do high-quality downscaling on the GPU. For thumbnails this might be a non-trivial win, especially under the IThumbnailCache surrogate where the GPU may or may not be available. Probably not worth it given the current `render_to_bgra` is the bottleneck, not the downscale.

---

## 6. Areas already in good shape

- COM ref counting in `ClassFactory` / handlers — clean, no leaks visible.
- `safe_release` pattern in ViewerWindow — consistent.
- `enumerate_astro_siblings` uses `FindExInfoBasic` + `FIND_FIRST_EX_LARGE_FETCH` — best-practice optimizations applied.
- Cache invalidation via schema version with a documented comment about the v2 regression — exemplary engineering hygiene.
- The DPI 96 override in `create_render_target` with the explanatory comment is exactly right; preserve it.
- Dark titlebar handling (`DwmSetWindowAttribute` with attribute IDs 20/34/35/36) is the correct Win11 22H2+ pattern with graceful no-op fallback on older builds.
- `StreamBuffer::kHeaderCap = 32 MB` for PropertyHandler is well-reasoned and documented.
- The cache key documentation (read 8 KB but pass total size) prevents a subtle bug class.
- Subclass cleanup via `WM_NCDESTROY` + `RemoveWindowSubclass` is correctly placed.

---

## Appendix A — Why `(bb_w - bb_w) * 0.0f` exists

This appears to be a placeholder for "if I ever need to offset by the bbox swap, this is where it goes." `bb_w` and `bb_h` are computed only to be discarded (the `(void)bb_h;` at line 851 makes that explicit). When you implement rotation pan-clamping per-axis you'll want them, but right now `clamp_offset` already handles the rotated bbox separately. Recommend deleting both lines and re-introducing if/when needed.

## Appendix B — On the `Float32` XISF fast path

Once you sanitize NaNs at load (recommendation 3.1) and add the renderer LUT (recommendation 4.1), the typical 96-Mpx PixInsight master scenario looks like:

| Operation | Today | After fixes |
|---|---|---|
| File read (IO) | ~600 ms | ~200 ms (no slurp) |
| Convert + scan | ~700 ms | ~200 ms (vectorizable) |
| Initial render | ~1200 ms | ~150 ms (LUT) |
| Auto-stretch | ~300 ms | ~50 ms (already sampled) |
| Pixel stats | ~600 ms | ~15 ms (sampled) |
| Star detect | ~500 ms | ~250 ms (flat pixel buffer) |
| Histogram bins | ~300 ms | 0 (deferred until shown) |
| **Total first-load** | **~4.2 s** | **~0.9 s** |

Caveat: these are estimates from looking at the code. Real numbers will differ — a real profile is the right next step before doing the work.

---

End of review.
