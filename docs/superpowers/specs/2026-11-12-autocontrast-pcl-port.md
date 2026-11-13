# AutoContrast → native PCL module: design

Date 2026-11-12 · repo `~/projects/astro-pi`, branch `ac/pcl-port` (from `origin/main` e86366c5) · plan: `docs/superpowers/plans/2026-11-12-autocontrast-pcl-port.md`.

Labels: **[V]** verified here (read the code, ran it, or read the header). **[I]** inferred, not proven. **[A]** an assumption made in autonomous design (no human in the loop); each one is listed in section 12 so Scott can overrule it.

---

## 1. Intent

Scott, 2026-11-12: "just port it". AutoContrast (`tools/autocontrast/`) becomes a native PCL module, `modules/autocontrast/` → `AutoContrast-pxm.so`. What ships has no sidecar, no Python and no PJSR, and the port loses no functionality. Rules behind it: `autocontrast-native-pcl-no-sidecar`, `astro-pi-no-more-scripts` (memory).

The design document `tools/autocontrast/autocontrast-design-v0.1.md` stays the source of truth for *what the tool is*. Its §2 (core theses) and §12 (non-goals) bind this port without exception:
- §2.1 the reference never pushes linear channel ratios; chroma guidance is gated by palette;
- §2.2 scale is measured in arcseconds, never pixels; every comparison is band-limited;
- §2.3 palette achievability is a hard gate (chroma weight is zeroed in code, never by configuration);
- §2.4 the reference is a direction; the optimizer stops when its own gains flatline and declines when nothing beats the input;
- §12 no pixel-space loss against the reference, no style transfer, no absolute VLM score, no silent degraded path.

Success: a user with a stretched image in PixInsight runs **AutoContrast**, and the module (1) finds the sky position, (2) finds or discovers a licensed professional reference covering it, (3) measures the fingerprint distance, (4) runs the guardrailed beam search over stock PixInsight processes, (5) replays the winning recipe at full resolution into a new window, and (6) writes the recipe, the guardrail log and the report. Every number it computes matches the Python reference to stated tolerances.

## 2. Goals, non-goals, anti-goals

**Goals**
- G1. Every capability of the Python package and the four PJSR scripts is available natively (Feature Parity Checklist, plan end).
- G2. Numerical parity with the Python reference at a pinned commit, proven by goldens generated from Python (dev-only tooling).
- G3. The open Phase 2 acceptance work rides along: §3.6 hybrid proxy validation (old Task 15), §10 exit criterion (old Task 14, `task-14-brief-v2.md`) and the spec'd linear-input refusal, none of which the Python reached.
- G4. Built only in the Rocky 9 container, portable (`verify-portable.sh`), thread-safe and reentrant, no OpenMP.
- G5. Network connections documented for CPD 6.1 and opt-in.

**Non-goals**
- Stage 0 (stretch selection for linear input) — not committed work (phase2 spec §1).
- Phase 3 VLM director and Phase 4 mothership — never built in Python [V: no Ollama/HTTP client exists outside the crawler, `grep urllib|http|ollama src/`], so there is nothing to port. Section 7 records this.
- A release, `updates.xri` changes, or a merge to `main` (coordinator instruction).
- Re-tuning any calibrated threshold (phase2 spec §3.7: 0.075/0.15 buckets, `epsilon`, `epsilon_improve`, guardrail limits, `max_attempts = 3 × top_k`, weights 0.40/0.25/0.20/0.15).

**Anti-goals** (would make the port a failure even if it "works")
- Any approximation that changes a decision the Python makes (a different proposed action, a different guardrail verdict, a different beam).
- A second plate solver beside Gaia Depth Grade's.
- Any path where the reference's colour pushes a palette-mismatched image.
- A degraded path that proceeds silently (an unsolved WCS, a band-limit failure, a guardrail that could not assess, an executor failure).

## 3. What exists today [V]

| Area | Python (LOC) | What it does |
|---|---|---|
| `fingerprint/` starlet, energy, bandlimit, color, metrics, palette, extract, distance | 1 006 | B3 à trous (scipy `convolve1d`, `mode="mirror"`), per-plane energy at `2**i × scale`, band-limit with log-scale `np.interp`, sRGB→CIELAB (D65), quantiles at p1…p99.9, chroma-weighted 16×16 a*/b* histogram, background block, palette classes and the chroma family gate, distance = weighted sum with **exact EMD** via `scipy.stats.wasserstein_distance_nd` |
| `optimize/` actions, recipe, propose, guardrails, beam, loop, session, executor(s) | 1 896 | bounded action menu (band-limited), recipe → stock PI process rendering, gap-ranked proposer (per-band deficits, magnitude by tertile buckets, one action per group), six guardrails, beam prune, convergence, fail-safe outcome, batched protocol + session JSON, `NumpyExecutor` (offline approximation) |
| `db/` records, store, skymath, wcs, ingest, seed | 744 | SQLite fingerprint store (JSON record + indexed columns, `tool_output` refused), haversine cone search, WCS tiers (FITS header via astropy, AVM via pyavm, injected blind solver, manual), ingest with downsample-corrected scale, curated seed catalog |
| `db/discover/` gallery, crawl, index, discover, `__main__` | 1 229 | Djangoplicity HTML parsing (labels, `var images` literal, credit, filters), polite crawler (1 req/s + jitter, 8000 cap, 256 MB cap, 429/5xx retry), SQLite gallery index, rank (palette, framing, centering, id) → fetch → verify gates G1–G6 → ingest |
| `io/loaders.py` | 383 | allow-listed raster/FITS loading; Pillow bicubic downsize; 16-bit TIFF via tifffile, 16-bit PNG via pypng; FITS min/max normalisation |
| `eval/` ranking, degrade | 406 | Phase 0 harness: pro/amateur separation, leave-one-out, degradation monotonicity, reference-is-local-minimum |
| `sidecar.py` | 352 | file-protocol ops: ping, fingerprint, distance, solve, analyze, optimize_begin/step |
| PJSR `autocontrast_analyze.js` (180), `autocontrast_optimize.js` (908), `sidecar_bridge_test.js` (99), `solve_spike.js` (151) | 1 338 | Analyze/Optimize drivers; the **PixInsight executor** (eight process builders, l.334–481), proxy build with PI `Resample`, reporting |
| tests | 6 600 (+ 6 HTML fixtures) | 384 offline + 7 live; offline run here: 383 passed, 1 failed (`test_raster_highdepth` 16-bit PNG: `pypng` missing from the venv — environmental, fixed in plan Task 1) |

Data [V]: `data/fingerprints.sqlite` (2 records), `data/gallery_index.sqlite` (61 entries: eso 26, esa_hubble 35), `data/discovery_cache/` (35 renders), `data/references/` (5 pros), `data/amateur/` (5), `data/seed_catalog.json` (tracked). Python reference env: Python 3.14, numpy 2.5.3, scipy 1.18.1, astropy 8.0.1, Pillow 12.3.0 (libjpeg-turbo, libtiff 4.7.1, zlib-ng), pyavm.

## 4. Approaches considered

| | Approach | Trade-off | Verdict |
|---|---|---|---|
| A | **PCL-free C++ core (`ac_core`) ported line for line, thin PCL module layer, goldens from Python** (the Gaia Depth Grade house template) | Most code; parity is provable in ctest without PixInsight; the core is reusable by a dev CLI | **Chosen** |
| B | Lean on PCL's own primitives (ATrousWaveletTransform, Histogram, GaussianFilter, FileFormat readers) for everything | Less code, but PCL's boundary rules, float32 storage and PI's JPEG decoder differ from scipy/numpy/Pillow, so parity fails at the first threshold; and none of it runs in ctest | Rejected (fails G2) |
| C | Keep Python as an embedded interpreter | Violates "no Python in what ships" | Rejected |

Within A, the executor is not a choice: §12 requires the output to be a sequence of stock PixInsight processes, so the module runs those processes through `pcl::ProcessInstance`, in process, on hidden windows — the same builders the PJSR executor used, now in C++.

## 5. Architecture

```
modules/autocontrast/
  src/core/      ac_core: PCL-free, double precision, Python frame (row-major, y = 0 top row)
    num/         numpy/scipy-exact primitives
    fingerprint/ starlet, energy, bandlimit, color, metrics, palette, extract, distance, emd
    optimize/    actions, recipe, propose, guardrails, beam, loop, session, approx_executor (test double)
    io/          raster decode (libjpeg-turbo, libpng, libtiff), Pillow-exact resample, XMP/AVM
    db/          records, json, store (SQLite), skymath, wcs (TAN geometry + tiers), ingest, seed
    discover/    gallery parsing, crawl, index, discover (over an abstract Fetcher)
    eval/        ranking, degrade (Phase 0 harness)
  src/module/    PCL glue: module, processes, parameters, instances, interfaces, ImageBridge,
                 PiExecutor (ProcessInstance builders), PclFetcher (NetworkTransfer), NetworkConsent,
                 target WCS adapter (AstrometricMetadata), solver adapter
  src/tool/      ac-tool: dev CLI over ac_core (fingerprint, distance, rank, degrade; no network) — built, never shipped
  test/          Catch2 unit/parity tests, goldens (from Python), headless probes (.scp), GUI smoke
```

### 5.1 Data flow — `AutoContrast` Optimize

1. Target view → `ImageBridge` → `ac::Image` (float64, H×W×3, [0,1]; mono promoted to 3 channels as `load_raster` does).
2. **Linear-input refusal** (phase2 spec §1, §6; never built in Python): a loud error naming the problem and telling the user to stretch first (definition in plan Task 17; calibration choice recorded there).
3. **WCS** (§5.2 tiers, in order): (1) the view's astrometric solution (`pcl::AstrometricMetadata`) — the in-PI equivalent of the FITS-header tier; (2) the AVM tag in the view's source file (XMP, raster files only); (3) blind solve with the shared native solver (section 9); (4) the manual annotation parameters (all four values required together). Unsolved → loud error with every tier's note.
4. **Palette**: declared parameter wins; else derived from the view's `FILTER` keywords; a view with no FITS keywords at all (a raster) → `unknown` (section 6.4).
5. **Reference**: store cone search; on a miss, discovery (network, opt-in) → the nearest match (Python's `min(separation)`).
6. **Proxy**: longest side ≤ `maxDim` (1600) with the Pillow-exact float bicubic resampler; scale = native × (native longest / proxy longest). One resampler for proxy, references and full-res validation — this removes the 9 % Pillow-vs-PI-`Resample` divergence recorded in `autocontrast-phase2-live-pixinsight-findings`, by construction.
7. **Search**: `ac::loop` (beam, proposer, guardrails, convergence) over an `Executor` interface; production executor = `PiExecutor`: for each candidate, a hidden 32-bit float window holding the parent, one stock process via `ProcessInstance::ExecuteOn`, pixels read back. Candidates live in memory as float32 (no 16-bit TIFF round trip).
8. **§3.6 hybrid validation**: every 5 iterations, replay best-so-far at full resolution, downsample with the same resampler, re-measure; divergence beyond `fullResTolerance` is logged loudly and the run flagged (never silently trusted, never used to discard).
9. **Outcome**: improved → replay the recipe at full resolution into a new window `<id>_autocontrast`; declined → no new pixels, the report says the input was left untouched. Always: console report, `report.txt`, `recipe.json` (same keys as the PJSR's), `session.json` (written after every iteration; inspectable).

### 5.2 Threading and reentrancy
- PixInsight processes execute on the main thread only (inside `ExecuteOn`/`ExecuteGlobal`).
- Measurement (starlet, metrics, guardrails) uses `ac::parallel_for` (copied semantics of `nukex::parallel_for_dynamic`; no OpenMP). Per-pixel work is independent, so results are bit-identical at any thread count. **Every reduction is deterministic:** fixed chunk boundaries independent of the thread count, partial sums combined in chunk order. A test runs each measurement with 1 and N threads and requires identical bits.
- No mutable statics; every function takes its state explicitly. `pcl::View`/`ImageWindow` handles are never held past a window close (memory `pcl-view-handle-use-after-free`).

## 6. Algorithm mapping (Python → C++)

All parity tolerances below are against goldens produced by the pinned Python commit (plan Task 1 `MANIFEST.json`).

### 6.1 Primitives (`ac_core/num`)
| Python | C++ | Parity |
|---|---|---|
| `np.percentile(x, q)` (linear) | nth_element + numpy 2.x `_lerp` formula verbatim (`a + d·t`, and `b − d·(1−t)` where `t ≥ 0.5`) | bit-exact |
| `np.median` | mean of the two middle order statistics | bit-exact |
| `np.interp` | clamped linear interpolation, numpy's slope form | ≤ 1 ulp |
| `np.histogram` uniform bins | numpy's fast path verbatim: `(x − lo)·norm` index, then the two edge-comparison corrections, last bin right-inclusive, out-of-range dropped | identical counts/weights |
| `np.histogram2d` weighted | `searchsorted(edges, x, 'right') − 1`, right edge inclusive; weights summed in input order | identical bins; sums ≤ 1e-12 rel |
| `scipy.ndimage.convolve1d(mode="mirror")` | own: `d c b \| a b c d \| c b a` reflection repeated for kernels wider than the axis, taps summed in scipy's order | ≤ 1e-14 abs |
| `scipy.ndimage.gaussian_filter` (reflect, truncate 4.0, radius `int(4σ+0.5)`) | own separable | ≤ 1e-12 abs (used by the approx executor and `eval.degrade` only) |
| `scipy.ndimage.label` (default cross structure) + `find_objects` | own two-pass union-find, 4-connectivity, raster label order | identical labels and boxes |
| `np.arctan2`, `np.degrees`, `% 360`, `np.cbrt`, `np.hypot`, `np.log1p` | libm equivalents | ≤ 1 ulp; decision parity checked end to end |

### 6.2 Starlet — **own implementation**
PCL's `ATrousWaveletTransform` works on float32 `Image`s with PCL's boundary handling, and the B3 kernel dilation and plane count must match `starlet.py` exactly; any boundary difference moves the coarse planes and with them every band deficit. Own double-precision separable transform: kernel `[1,4,6,4,1]/16` dilated by `2**scale`, axis 0 then axis 1, `mode="mirror"`, `planes[s] = current − smoothed`. Parity ≤ 1e-12 abs per plane; reconstruction identity tested.

### 6.3 CIELAB — **own, after checking reuse** [V, checked 2026-11-12]
- `color.py` is: sRGB piecewise linearisation (0.04045 / 12.92 / 2.4), the 7-digit `_RGB_TO_XYZ` matrix, white (0.95047, 1.0, 1.08883), `δ = 6/29`, `f(t) = cbrt(t)` above `δ³`, else `t/(3δ²) + 4/29`.
- `modules/nukex/src/lib/compose/src/color_composer.cpp` has the same matrix and white point but the **rounded CIE constants** `t > 0.008856 ? cbrt(t) : 7.787·t + 16/116` (l.22–23). The exact values are δ³ = 0.0088564517 and 1/(3δ²) = 7.7870370. Below the knee — exactly the dark sky background whose quantiles (p1, p5, p10) and floor/peak ratio the fingerprint measures — L* differs in the 4th–5th digit. Not reusable for parity; changing NukeX's constants would alter NukeX's shipped output, so it is left alone.
- `modules/planetary/src/stack/Mineral.cpp` has no CIELAB conversion (its colour work is in RGB; its only "label" is 4-connected component labelling).
- So `ac_core/fingerprint/color` is written fresh (≈ 30 lines), a direct port of `color.py`. Parity ≤ 1e-12 abs on L*, a*, b*.

### 6.4 Metrics, extract, palette, distance
- Line-for-line ports. `extract` shares one starlet decomposition between the energy spectrum and the structure/gradient ratio, as Python does.
- `FingerprintData` JSON is **byte-compatible** with Python's `to_dict()` + `json.dumps` field names, so stores written by either side are readable by the other (section 6.8).
- Palette from a PI view: `palette_class_from_filters` over every `FILTER` keyword value (stripped). A view with no FITS keywords → `unknown` (the Python rule for non-FITS files); a view with keywords but no `FILTER` → `RGB` (the Python rule for a FITS header without the keyword) [A1].
- Distance: option (b) weight redistribution, spectrum dropped when band-limiting is unusable, chroma gated by `palette_chroma_compatible` in code.
- Parity: every fingerprint component ≤ 1e-9 relative; D ≤ 1e-9 absolute on synthetic and on the 10 Phase 0 renders.

### 6.5 Multi-dimensional EMD — **exact network simplex**
- `wasserstein_distance_nd(coords, coords, u, v)` on the 16×16 grid is the optimal-transport LP between two 256-point distributions with Euclidean ground cost; scipy normalises each weight vector to sum 1 and solves it with HiGHS (`linprog`) [V source].
- C++: an **exact network simplex** for the uncapacitated transportation problem (block-search pivoting, strongly feasible spanning-tree basis to prevent cycling under degeneracy, double supplies), written for `ac_core` (no third-party code). Zero-mass bins are kept as nodes (supply 0), so the graph is fixed at 256 + 256.
- Correctness is proven two ways, not only against scipy: (1) a **dual certificate** in every unit test — reduced costs `c_ij − u_i − v_j ≥ −1e-12` on all arcs and `= 0` on basic arcs, primal feasibility to 1e-12; (2) parity with scipy goldens ≤ 1e-9 absolute on the normalised distance (HiGHS solves to its own feasibility tolerance; if a golden differs by more and the certificate holds, the golden's tolerance is the culprit and the task reports it with the certificate rather than loosening the test).
- Neutral images (either histogram all zero) → 0, as Python.
- **Measured in Task 6 (2026-11-13):** scipy solves the *dual* LP through `milp` at its default 1e-7 tolerance, so its EMD is itself inexact — on 2 of 18 golden pairs it is off by 1.97e-9 and 9.24e-9 normalised (5.6e-7 and 2.6e-6 raw; 5.3e-8 and 2.2e-7 relative) while the network simplex's dual certificate holds and HiGHS on the primal with 1e-10 tolerances agrees with the simplex to 1.5e-14. Ruling: the oracle for *exactness* is a tight-tolerance reference golden (≤ 1e-12); parity with scipy is asserted at about twice the worst measured scipy error (≤ 5e-6 raw, ≤ 2e-8 normalised); every other quantity keeps ≤ 1e-9 vs Python. The resulting D difference (≤ 0.20 × 9.24e-9 ≈ 1.8e-9) is six orders of magnitude below every decision threshold (`epsilon`, `epsilon_improve` = 1e-3; gap buckets 0.075/0.15), so no Python decision can change.

### 6.6 Tonal quantiles, proposer, beam, guardrails
- Line-for-line ports of `actions.py`, `recipe.py`, `propose.py`, `beam.py`, `loop.py` (`advance`, `ingest_candidate`, `_close_iteration`, `_check_convergence`, `outcome`), `guardrails.py`, `session.py` (save only — see 6.10).
- `_STRENGTH`, `LEVELS`, `ONCE_ONLY`, `PROCESS_FOR_KIND`, `GAP_GENTLE_MAX = 0.075`, `GAP_MODERATE_MAX = 0.15`, `BeamConfig` defaults (3, 3, 20, 3, 1e-3, 1e-3, `max_attempts = 3·top_k`), `GuardrailLimits` defaults, `HUE_CHROMA_FLOOR = 1.0`, `NUM_HUE_BINS = 360` — copied verbatim, cited by Python file and line.
- Group ordering in `propose_actions` sorts the Python tuple `(−gap, scale_or_−1, kind)` exactly (float compare, then string compare).
- `Action.key` formats `scale_arcsec` with `%.3f` (Python `f"{x:.3f}"`, round-half-even on the binary value — same as C `printf`).
- Loop parity: Python `advance`/`run_to_convergence` with `NumpyExecutor` on synthetic fixtures and on `flatten(eso1103a, 0.6)` → C++ with `ApproxExecutor` (a test-only port of `NumpyExecutor`) must give identical recipes, iteration count, convergence reason, guardrail log (names, iterations, action keys) and distances ≤ 1e-9.

### 6.7 WCS (FITS and AVM)
- **TAN geometry** (`_geometry`): centre = pixel→world at astropy pixel `(nx/2, ny/2)`, radius = separation to astropy pixel `(0, 0)`, scale = mean of `proj_plane_pixel_scales` (the column norms of the CD matrix, or `|CDELTi|` with PC). Ported for linear TAN (AVM is always linear TAN with CDELT/CROTA or CD).
- **In PI (target view):** the solution comes from `pcl::AstrometricMetadata`. Mapping astropy pixel `(x, y)` (0-based, FITS bottom-up) to PI image coordinates is `(x + 0.5, H − y − 0.5)` **for a FITS file's own header** [I — the GDG port found PI saves FITS top-down (`gaia-depth-grade-wcs-yflip`, GDG Correction C1), so the frame is proven in plan Task 11 by probe, comparing astropy on the same file]. Parity: centre ≤ 1e-6 deg, radius and scale ≤ 1e-6 relative on a plate-solved FITS (`/mnt/qnap/astro_data/9_16_2023/M42/Stacked14_*.fit`; its header WCS is RA 83.8202, Dec −5.3823, 61.58′, 1.0108″/px [V memory]). With SIP distortion PI's spline solution and astropy's SIP may differ by a fraction of a pixel at the corner; the tolerance is set from the measured difference in Task 11, never assumed.
- **AVM:** extract the XMP packet (JPEG APP1 `http://ns.adobe.com/xap/1.0/`, PNG `iTXt XML:com.adobe.xmp`, TIFF tag 700), parse with `pcl::XMLDocument`, and reproduce **pyavm's strict validation**: a tag pyavm rejects must be rejected (memory: *never force-parse AVM*; eso1103a's multi-valued `Spatial.Notes` is rejected, force-parsing gives corrupt coordinates 81.97, −3.74). Golden = pyavm's accept/reject and WCS for all 40 cached and reference renders; C++ must agree on every file.

### 6.8 SQLite fingerprint DB and gallery index
- SQLite = the repo's vendored amalgamation `third_party/sqlite` (3.53.4, public domain), built static `-fPIC` with hidden symbols — the same copy PI Copilot links [V `modules/pi-copilot/CMakeLists.txt:73`]. (NukeX fetches its own amalgamation; the vendored one needs no network at build time.)
- Schemas are **identical** to `store.py` and `index.py` (tables, columns, indexes), so the existing `data/fingerprints.sqlite` and `data/gallery_index.sqlite` open unchanged; a test opens copies of them.
- Record JSON via nlohmann/json (FetchContent, pinned commit — PI Copilot's exact declaration). Float formatting: shortest round-trip (`%.17g` fallback), so Python `json.loads` reads identical doubles.

### 6.9 TIFF/PNG/JPEG I/O
- References are display-referred rasters downloaded from the galleries; Python decodes them with Pillow (libjpeg-turbo), tifffile and pypng, then resizes with Pillow bicubic. Parity therefore needs the **same decoders and the same resampler**, not PI's format modules (PI's JPEG reader may use a different libjpeg and its FileFormat API does not exist in ctest).
- Static `-fPIC` libraries fetched and hash-pinned in CMake (the NukeX/PI Copilot FetchContent pattern): **libjpeg-turbo** (decoder only, `WITH_SIMD=OFF` unless the parity test shows SIMD is required — libjpeg-turbo documents identical ISLOW output [I]), **libpng**, **libtiff** (deflate/LZW/packbits; no JPEG-in-TIFF unless a cached render needs it). zlib from PCL's `zlib-pxi`.
- **Pillow-exact resampling** ported from Pillow's `Resample.c` (HPND licence; notice added): bicubic `a = −0.5`, support scaled by the reduction factor, 8-bit path with Pillow's fixed-point coefficients and clipping, float (`"F"`) path in double accumulation. The horizontal pass streams decoded scanlines, so an 18000 × 18000 render peaks at ~200 MB instead of ~1 GB, bit-identically [I, proven by parity].
- Allow-list identical to `supported_suffixes()`; anything else is refused with the same message. FITS/XISF *files* (user ingest only) open through PixInsight (`ImageWindow::Open`) and are min/max-normalised as `load_image` does.
- Parity: decoded 8-bit arrays bit-identical to Pillow on every cached/reference JPEG and PNG; resized arrays bit-identical (8-bit) or ≤ 1 float32 ulp (F path); 16-bit TIFF/PNG round-trip fixtures from `test_raster_highdepth.py`.

### 6.10 Session, recipe, report
- Batched protocol (`begin_batch`/`step_batch`, instruction ids, cursors, `pending`) existed only to cross the process boundary; the module runs `advance` in process. Memory note `autocontrast-project-status-native-pcl` lists it, the `loop.resume(path, proxy_path)` guard and the sidecar ops as **intentionally dropped (sidecar-only)** [V]. `session.json` is still written after every iteration in Python's schema (inspectable after a crash).
- `recipe.json` keeps the PJSR's keys (`improved, source_path, proxy_path, reference_id, baseline_distance, distance, convergence_reason, iterations, recipe, pixinsight_steps, guardrail_log, executor_failures`).

### 6.11 Discovery crawler
- `gallery.py` parsing ported with `std::regex` (ECMAScript) — every pattern rewritten from Python `re` and tested against the six captured HTML fixtures plus synthetic edge cases (`-0°` declinations, arcsec FoV, copyright credits, malformed trailing `<tr>`).
- `crawl.py` politeness constants verbatim: 1 s minimum interval + 0–0.25 s jitter, 8000 requests per run, 256 MB per image, 3 retries, 429 honours `Retry-After` (≤ 120 s), 5xx and transport errors back off `2**attempt` s, `DriftError` above 50 % unparseable detail pages, 7-day sync safety window.
- The crawl/discover orchestration runs over an abstract `Fetcher` (tested with a scripted fake), and `PclFetcher` is the only network code.

## 7. Network connections (CPD 6.1)

`urllib` is used in exactly one place [V]: `PoliteFetcher` (`db/discover/crawl.py`), for two purposes:

| # | Purpose | Hosts | Requests | Sent | Received |
|---|---|---|---|---|---|
| N1 | Gallery index sync (crawl listing + detail pages) | `https://esahubble.org`, `https://www.eso.org` | HTTP GET only | URL, `User-Agent` | HTML |
| N2 | Reference render download (discovery miss path) | `https://cdn.esahubble.org`, `https://cdn.eso.org` | HTTP GET only | URL, `User-Agent` | JPEG/PNG/TIFF, ≤ 256 MB |

- **No image data, no user data, no telemetry is ever sent.** There is no POST.
- There is **no VLM/API call** to port: Phase 3 (Ollama director) and Phase 4 (mothership) were never implemented [V]. If they are built later, they get their own consent version (section 7.2) — nothing in this design pre-builds them.
- Transport: `pcl::NetworkTransfer`, HTTPS only, `SetSSL(true, true, true, true)`, a host allow-list checked before every request (the four hosts above; the CDN host must also match the image-URL pattern), the GDG `TapClient` receiver pattern (`Control` subclass, main thread). Redirects: memory `pi-externalprocess-gotchas` says NetworkTransfer does not follow them; a 3xx with a `Location` inside the allow-list is followed by hand (max 3), anything else is an error [I, probed live in plan Task 14].
- User-Agent: `AutoContrast/<version> (PixInsight module; +https://github.com/scarter4work/astro-pi)` [A2 — the Python UA carried Scott's email and the retired repo URL].

### 7.2 Consent (modelled on PI Copilot `DataConsent`)
- Network use is **opt-in**: the `allowNetwork` parameter of both processes defaults to `false` [A3 — Python's `allow_discovery` defaulted to true; CPD requires opt-in]. With it off, Analyze/Optimize use only the local store and say so when it misses ("no stored reference covers this field; enable network discovery to search the ESA/Hubble and ESO galleries").
- In the GUI, ticking "Allow network" opens a versioned disclosure dialog (`kConsentVersion = 1`) naming the four hosts, stating that only GET requests with a User-Agent are sent and that no image or user data leaves the machine; the agreement is stored with PCL `Settings` (`AutoContrast/NetworkConsentVersion`, `…/GrantedAt`), withdrawable in the interface. A process icon or a headless run with `allowNetwork = true` is the user's explicit act; the module still refuses (loud error) when the stored consent version is older than `kConsentVersion` and the run is interactive.
- The module documentation (`doc/tools/AutoContrast/`) carries a "Network connections" section with the table above, and `package-description.xml` mentions it.

## 8. Processes, parameters and identifiers (stable once shipped)

Module id **`AutoContrast`**; file `AutoContrast-pxm.so`; category **`scarter4work`** (GDG's choice). Two processes:

**`AutoContrast`** — view process (ExecuteOn the target view; `ExecuteGlobal` refused with a message). Command-line capable (`CanProcessCommandLines`) for headless use.

| Group | Parameter ids (type, default) |
|---|---|
| Mode | `mode` (enum `Analyze`/`Optimize`, Optimize) |
| Library | `storePath`, `indexPath`, `cacheDir`, `workDir` (string; defaults under `$XDG_DATA_HOME` or `~/.local/share/AutoContrast/` and `$XDG_CACHE_HOME`/`~/.cache/AutoContrast/`; `workDir` default `~/.local/share/AutoContrast/runs/<UTC stamp>`) |
| Network | `allowNetwork` (bool, false) |
| Reference | `referenceId` (string, empty = automatic cone search), `searchRadiusArcmin` (double, 0 = the field's radius) |
| Measurement | `maxDim` (int32, 1600), `nScales` (int32, 7), `psfFWHMArcsec` (double, 2.0), `paletteClass` (enum Auto/LRGB/RGB/SHO/HOO/HaRGB/HaOIII-RGB/L-only/unknown, Auto) |
| WCS | `blindSolve` (enum Automatic/Always/Disabled, Automatic — the GDG `BlindMode`), `manualWCS` (bool, false), `manualRA`, `manualDec` (deg), `manualFovRadiusArcmin`, `manualPixelScaleArcsec` |
| Weights | `weightSpectrum` 0.40, `weightTonal` 0.25, `weightChroma` 0.20, `weightBackground` 0.15 |
| Beam | `beamWidth` 3, `topK` 3, `iterationCap` 20, `convergenceWindow` 3, `epsilon` 1e-3, `epsilonImprove` 1e-3, `maxAttempts` (0 = 3·topK) |
| Guardrails | `noiseGrowth` 0.15, `shadowClipFraction` 1e-4, `highlightClipFraction` 1e-4, `starCountDrop` 0.05, `starFWHMGrowth` 0.20, `starEccGrowth` 0.20, `hueInventionMass` 1e-3, `hueToleranceDeg` 20, `channelRatioDrift` 0.10 |
| Validation | `fullResCheckEvery` (int32, 5; 0 = off), `fullResTolerance` (double, 0.02 [A4]) |
| Output | `replayFullResolution` (bool, true), `saveCandidates` (bool, false) |
| Read-only outputs | `outBaselineDistance`, `outDistance`, `outImproved`, `outIterations`, `outReferenceId`, `outConvergenceReason`, `outRecipeJSON` |

**`AutoContrastReferences`** — global process (reference library). `operation` enum: `SyncGalleries` (`galleries` string list, `maxPages`, `refresh`), `SeedCatalog` (`catalogPath`, `imageDir`), `IngestFile` (`filePath`, `paletteClass` required, `sourceType` professional_render/user_render — `tool_output` is rejected, `license`, `attribution`, `sourceUrl`, manual WCS fields, `psfFWHMArcsec`, `nScales`, `maxDim`), `List` (console table of store and index). Shares `storePath`, `indexPath`, `cacheDir`, `allowNetwork` with `AutoContrast`. Command-line capable.

**Test-only `AutoContrastProbe`** process, compiled only with `-DAC_BUILD_PROBES=ON`; `build-modules.sh` fails if it appears in the shipped `.so` (GDG Correction C6 pattern).

## 9. Plate solving — reuse, not a second solver

- Python's tier 3 was an injected callback that production never supplied (`sidecar._op_analyze` passes no `blind_solver`) [V]; `solve_spike.js` was a spike. So tier 3 is new capability in practice, and the design (§5.2) asks for it.
- Gaia Depth Grade already ships a native port of ImageSolver 6.6 on `gdg/phaseE-solver` (`cddc1ba0`, plan Tasks 14–15: `Solver`, `SolverAlign`, `SolverBlind`, `SolverDetect`, core `SolveMath`). AutoContrast **reuses it**: the plan's solver task factors it into `modules/shared/platesolve/` (a static library both modules link, namespace kept), on `ac/pcl-port`, once the solver commits are on `main` (A11), by merging `main` into `ac/pcl-port`. No commit ever goes to a `gdg/*` branch. **Merge dependency:** `ac/pcl-port` cannot merge to `main` before `gdg/phaseE-solver`'s solver commits are on `main`; the GDG agent re-points its build at the shared library (or the AutoContrast task does it in the factoring commit) — recorded in the plan.
- For references, tier 3 is tried before the manual tier (Python order: header/AVM → blind → manual), so discovery's G2 gate gains an independent cross-check for renders without AVM — exactly what `solve_spike.js` was probing.
- The solver needs a local Gaia DR3 and, for blind solving, the astrometric index configured in PI's Gaia process. Without them tier 3 reports "not available" and falls through (never a silent pass).

## 10. Constraints (binding)

- Rocky 9 container build only (`tools/build-env/build-modules.sh`), PCL 2.13.0 (C++20), `verify-portable.sh` gate, minimum PixInsight 1.9.6 (own platform block when it ships).
- Thread-safe and reentrant; no OpenMP; deterministic reductions (5.2).
- New dependencies: static `-fPIC` archives (libjpeg-turbo, libpng, libtiff — FetchContent, SHA-256 pinned) or vendored header-only code (`include/` + `LICENSE` only: nlohmann/json via FetchContent as PI Copilot does; Catch2 = NukeX's copy). THIRD_PARTY_NOTICES lists every one, plus the PCL licence acknowledgement and the Pillow (HPND) notice for the ported resampler.
- Every PixInsight launch goes through `tools/pi-headless.sh`; in-PI tests drive the processes' command lines from a generated one-line `.scp` (no PJSR, no Python in tests that run PixInsight — GDG decision D12); `tools/check-pi-launches.py` must stay green.
- RAM: build dirs and large outputs under the worktree or `~/projects`, never `/tmp` (tmpfs); host builds `-j8`; one container build at a time.
- No shipped Python/PJSR; Python stays as dev-only golden tooling under `tools/autocontrast/` (not shipped, as today).

## 11. Testing strategy

1. **Primitive parity** (ctest, no PI): each `num` function vs numpy/scipy goldens, plus thread-count determinism.
2. **Stage parity** (ctest): each stage fed the golden *inputs of that stage* (no compounding): starlet planes, energy, band-limit, Lab, metrics, fingerprint, EMD (+ certificate), distance, menu, proposals, guardrail verdicts, loop runs with `ApproxExecutor`, loader decode/resize, AVM accept/reject + WCS, gallery parsing, crawl/discover with a scripted fetcher, store/index round trips including Python-written DBs.
3. **Real-data parity** (ctest, skipped with code 77 only when the NAS or `data/` is absent — never silently): the 10 Phase 0 renders' fingerprints and D, the Phase 0 harness reports (leave-one-out, local-minimum), the M42 stack's header WCS.
4. **In PI, headless** (`.scp` + command line, slot 95/96 like GDG/Planetary): process ids load; Analyze on a solved fixture; Optimize with real processes on a 400 px fixture (declines at the floor; improves `flatten(eso1103a, 0.6)`); executor failure paths (LHE radius < 16 px raises → logged, run continues; StarXTerminator absent or failing → logged); §3.6 full-res check; linear-input refusal; network consent refusal; a live crawl/download smoke (runs by default, as Python's live tests did; fails, never skips, when the sites are down).
5. **GUI smoke** on a private Xvfb (both interfaces, the consent dialog).
6. **§10 exit criterion** (old Task 14, brief v2) natively: improves a flat image; declines on an image at the floor (a render against its own fingerprint).

## 12. Assumptions made in autonomous design

| # | Assumption | Why | Overrule cost |
|---|---|---|---|
| A1 | Palette of a PI view: no FITS keywords → `unknown`; keywords without `FILTER` → `RGB` | nearest equivalent of Python's file-suffix rule | one function |
| A2 | User-Agent drops Scott's email and the retired repo URL | a shipped module should not broadcast a personal address from every user's machine | one constant |
| A3 | Network discovery is opt-in (`allowNetwork = false`), with a versioned consent | CPD 6.1/6.2 posture as in PI Copilot; Python defaulted to on | default flip |
| A4 | `fullResTolerance = 0.02` absolute on D | §3.6 never specified it; 0.02 ≈ 10 % of the measured D ≈ 0.23 of a real print; flagged as a calibration question, never used to discard | parameter default |
| A5 | Cross-run session resume dropped (session JSON still written) | memory lists the resume guard as intentionally dropped sidecar plumbing; no user path ever resumed | small: load + continue |
| A6 | Optimize replays the winning recipe at full resolution into a new window by default | phase2 spec §8 deliverable ("a new image window with the result") and §3.6 "final replay"; the interim PJSR left this manual | parameter default |
| A7 | One process `AutoContrast` with a `mode` enum instead of two (Analyze/Optimize) | fewer permanent ids; both share every parameter | ids are permanent — decide before first release |
| A8 | `star_split` keeps Python/PJSR semantics (StarXTerminator `stars=false`, result is the starless image, no recombination) | parity; it is discarded by the star guardrail whenever stars vanish. The recombination ("layer routing") the code comments mention was never built — surfaced as a design question, not silently changed | design change, needs Scott |
| A9 | Shared numerics with GDG (`percentile`, `gaussian_filter`) are **not** factored out now; only the solver is | GDG's numerics are on unmerged branches still moving; ~150 duplicated lines vs a cross-branch dependency | later refactor |
| A10 | First-run data: the package ships a **prebuilt gallery index** (metadata only — positions, FoV, palette, licence, credit, URLs; no images, no fingerprints), copied to the user's data directory on first use; `SyncGalleries` refreshes it incrementally. Built by the module's own `--sync` at release time. Every user crawling 8 000 pages at 1 req/s would be a poor neighbour to two free archives, and an empty index makes discovery fail immediately (`discover.py:299-304`) | release-time packaging (Task 22), not needed to build or test the port | ship nothing; users sync |
| A11 | The solver move into `modules/shared/platesolve/` happens only once GDG's solver commits are on `main` (they are not yet: `cddc1ba0` is on `gdg/phaseE-solver` and `gdg/phaseH-nebula`, and GDG work continues on top). Until then tier 3 is reached only through `ac::BlindSolverFn` (section 9), so every other task builds and tests without it; Task 19 is last in Phase E | moving files under active GDG work guarantees rename conflicts for whoever merges second | none |

## 13. Risks

- **Executor identity.** The C++ builders must set exactly the parameters the PJSR set (`autocontrast_optimize.js` l.334–481). Black point uses PJSR `Histogram(view.image).normalizedClipLow(round(0.01·totalCount))`; `pcl::Histogram`'s channel and resolution defaults must be confirmed by probe before relying on them (Task 16).
- **Starlet cost.** 1600 px proxy × 7 scales: the measured Python cost is 1.53 s per extraction; the C++ target is ≤ 0.3 s single-threaded. A dilated kernel at scale 6 is 257 taps wide (direct convolution), so the separable passes must be cache-friendly (transpose blocks) — performance never changes the arithmetic order that parity depends on.
- **EMD degeneracy.** Transportation problems with many zero bins are highly degenerate; the network simplex uses a strongly feasible basis, and the certificate test catches any non-optimal stop.
- **Network.** Gallery markup drift (DriftError stays loud), CDN redirects, 503s (the live suite fails rather than skipping).
- **Solver dependency.** If GDG's solver has not reached `main` when the rest is done, `ac/pcl-port` carries the merge and cannot land first.
