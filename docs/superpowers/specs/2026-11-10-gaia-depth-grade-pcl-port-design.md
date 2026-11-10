# Gaia Depth Grade → pure PCL module: scoping report

Date 2026-11-10 · repo `~/projects/astro-pi`, branch `feat/nebula-3d-depth-reconstruction` · deadline 2026-12-12 (32 calendar days).

Scope (Scott, via coordinator): one native PCL module does everything the sidecar and the PJSR scripts do today. No Python, no PJSR/JS, no sidecar package. Catalogue default is PI's local Gaia DR3.

Labels: **[V]** verified here (read the code or header, or ran it). **[I]** inferred, not proven.

---

## 0. Baseline facts

- Python suite: **107 passed** in the project venv (`gaia-depth-grade/.venv`, `pytest -q`) [V]. With the system `python3`, 5 test modules fail to collect because `photutils` is missing. That is environmental, not a code defect [V].
- Python core: **1,916 LOC** in `src/gaia_depth_grade/` [V]. That is 1,274 shipped (non-recon), 640 recon (branch only) and 2 in `__init__`.
- PJSR: **1,036 LOC** [V]: `pi/gaia_depth_grade_lib.jsh` 439, `pi/GaiaDepthGradeDialog.js` 518, `pi/gaia_depth_grade.js` 79.
- Recon branch [V] (`git diff main...HEAD -- gaia-depth-grade/src`): it adds `recon/` (640 LOC) and additively carries `bp_rp` through `distances.py`, `local_gaia.py` and `match.py`.
  - Plan tasks 1–8 are committed: model, channels, ionization, bp_rp, ionizer, extinction, cavity terms, cavity assembly.
  - Tasks 9–13 are **not written**: render2d, the `reconstruct` CLI, GLB export, the orbit GIF, and the NAS validation.
  - Design: `docs/superpowers/specs/2026-11-05-nebula-3d-depth-reconstruction-design.md`. Plan: `docs/superpowers/plans/2026-11-05-nebula-3d-depth-reconstruction.md`.
- **Existing bug found [V].** The recon ionizer cannot get colour in offline mode:
  - `queryLocalGaiaDR3()` (`pi/gaia_depth_grade_lib.jsh` ~l.350) writes only `ra dec parallax G`, i.e. `r[0],r[1],r[2],r[5]`, never BP/RP.
  - So `pick_ionizer` always falls back to `gaia-bright` in offline mode.
  - The port fixes this for free: `GaiaStarData` carries `magBP`/`magRP` (section 2).
- **Dead code [V].** `detect.measure_fwhm` fills a `fwhm` column that nothing reads: `cache._COLUMNS` drops it, and `psf`/`render` never use it. Do not port it.

---

## 1. Python core inventory

"Ship" = on `main` and in sidecar 1.0.8. "Recon" = branch only. LOC from `wc -l` [V]. The library calls listed are the ones actually made in the code [V].

| Module | LOC | Ship/Recon | What it computes | Third-party calls actually used | PCL replacement |
|---|---|---|---|---|---|
| `cli.py` | 198 | Ship | argparse dispatch for grade/footprint/prepare/render/nebula/preview; FITS read/write with the honesty tag | `astropy.io.fits` open/PrimaryHDU, `np.moveaxis`, json | Gone. Replaced by process parameters, ExecuteOn and the interface. Honesty tag becomes a FITS keyword/property on the output window. |
| `pipeline.py` | 69 | Ship | two stages: prepare (detect→footprint→catalogue→match→QA) and render (strength→modulation→halo→render) | numpy mean | Same orchestration in C++, with in-memory prep state |
| `config.py` | 70 | Ship | frozen dataclasses for Gains/GradeConfig, TOML loader | `tomllib` | Process parameters (ProcessParameter subclasses) |
| `cache.py` | 33 | Ship | prep table to `prep.npz` + `qa.json` | `np.savez/load`, astropy Table | Gone. Prep result is held in the interface/instance. No temp files. |
| `wcs.py` | 35 | Ship | field footprint: centre RA/Dec plus max corner separation | `astropy.wcs.WCS.pixel_to_world`, `SkyCoord.separation` | `AstrometricMetadata::ImageToCelestial` [V header] plus a great-circle distance (a few lines) |
| `transform.py` | 41 | Ship | log10-distance percentile normalisation → strength in [-1,1], confidence from r_lo/r_hi | `np.percentile` (linear), clip, log10 | Own `percentile_linear` (numpy "linear" method) |
| `detect.py` | 43 | Ship | sigma-clipped background stats, then DAOStarFinder (fwhm 3, 6σ) → x, y, flux | `astropy.stats.sigma_clipped_stats(sigma=3)`, `photutils.detection.DAOStarFinder` 3.0.0 | **Port DAOFIND** (photutils is BSD-3 [V METADATA]; `detection/daofinder.py` is 1,037 lines incl. docs [V]). The biggest parity item; see section 4. |
| `match.py` | 61 | Ship (+6 on Recon) | project catalogue to pixels; nearest detection per catalogue star; each detection keeps its closest catalogue star within tolerance; carries bp_rp | `wcs.world_to_pixel_values`, `scipy.spatial.cKDTree.query(k=1)`, `np.median` | `AstrometricMetadata::CelestialToImage` [V header], `pcl::KDTree` or a grid hash. Tie-break order (catalogue order, strict `<`) must be kept. |
| `modulate.py` | 25 | Ship | strength × gains → brightness/size/contrast/saturation vectors | arithmetic | trivial |
| `display.py` | 60 | Ship | screen blend; STF autostretch (median/MAD/MTF); PNG write, crop, downscale | numpy median, PIL `Image.resize`/`save` | Blend and MTF in C++. Render the preview with `Bitmap::Render` [V header] or own code. Display only, so no bit-parity needed. |
| `distances.py` | 136 | Ship (+7 on Recon) | **online** Bailer-Jones: ADQL cone, async TAP job, 3 retries, ECSV cache | `astroquery.gaia.GaiaClass.launch_job_async`, astropy Table/ECSV | Drop (recommendation in section 2) |
| `local_gaia.py` | 150 | Ship (+32 on Recon) | offline: TSV parse; G cut; distance = 1000/parallax; σ_parallax synthesised from a G knot table; S/N cut | `np.interp` | Direct `pcl::GaiaDatabaseFile::Search` [V header]. The knot σ can be replaced by the real Gold/Silver/Bronze parallax-error flags [V header]. That is a behaviour change, so it needs a decision. |
| `psf.py` | 126 | Ship | empirical halo profile: radial-median stack of bright, unsaturated, isolated stars; Moffat fallback | `np.mgrid`, median, `nanmedian`, `np.minimum.accumulate`, `np.interp` | Own code, about 150 C++ LOC |
| `render.py` | 127 | Ship | per-star windowed render: Hann taper, brightness, halo (tinted, stretched), noise-cored unsharp, saturation | **`scipy.ndimage.gaussian_filter`** on each window (reflect boundary at the window edge), median/MAD | Own separable Gaussian with scipy semantics (truncate 4σ, reflect). Parity needs scipy's edge mode exactly. |
| `nebula.py` | 100 | Ship | Gaia depth budget; structural depth field; atmospheric brightness/colour; structural pop | **`gaussian_filter` with large σ** (`max(4, 0.03·min(h,w))`, about 65 px on a 3840×2160 master), percentiles | Own Gaussian. Large σ wants an FFT or recursive path, which must still match the reflect boundary. |
| `recon/model.py` | 103 | Recon | ChannelSet / IonizationMap / Ionizer / CavityModel / ReconConfig, shape validation | dataclasses | C++ structs |
| `recon/channels.py` | 84 | Recon | SHO / duoband unmix (min) / OSC proxy; `looks_linear` percentile heuristic | `np.percentile`, min/max | own |
| `recon/ionization.py` | 30 | Recon | smoothed OIII/(OIII+Ha) | `gaussian_filter(σ=2)` | own Gaussian |
| `recon/ionizer.py` | 65 | Recon | bluest luminous star (bp_rp − 0.4·log flux), fallbacks | numpy | own |
| `recon/extinction.py` | 66 | Recon | dust from structure (grey closing vs smooth); Balmer decrement with signal gate | `gaussian_filter(σ=1)`, **`scipy.ndimage.grey_closing(size=15×15)`**, percentiles, exp | Own flat 15×15 closing with scipy's reflect boundary. Separable max/min (van Herk) is exact for a flat square. `pcl::DilationFilter/ErosionFilter` exist [V header], but their boundary rule must be checked before relying on them. |
| `recon/cavity.py` | 291 | Recon | geometry bowl, ionization depth, EM relief (DoG with σ 1 and 4, 99.5 pct), footprint, confidence-weighted blend, linear emissivity | `gaussian_filter`, `np.percentile` (also over a stacked 3-channel array), hypot | own |

**Primitives the whole port depends on** (one shared, parity-tested `gdg::num` layer):
- `gaussian_filter` (scipy: truncate 4.0, `mode='reflect'`, kernel radius `int(4σ+0.5)`);
- `percentile` (numpy "linear");
- `median` (mean of the two middle values when n is even);
- MAD;
- `np.interp` (with left/right clamping);
- `grey_closing` (flat square, reflect);
- `sigma_clipped_stats` (astropy defaults: sigma=3, maxiters=5, median centre, std);
- DAOFIND;
- nearest-neighbour matching.

Everything else is element-wise arithmetic.

---

## 2. Network connections and their on-platform mapping

| # | Connection today | Where | On-platform mapping |
|---|---|---|---|
| N1 | **ESA Gaia archive** `https://gea.esac.esa.int/`. Reads status notices from `/tap-server/notification`. Submits an async ADQL job to `/tap-server/tap/async` (Gaia DR3 joined with `external.gaiaedr3_distance`, G<18). Polls the job, downloads the result, up to 3 attempts. | `distances.py` via astroquery, in **online mode only** (Offline unchecked, and always in the headless `gaia_depth_grade.js` `grade` path) [V code + `doc/GaiaDepthGrade/GaiaDepthGrade.html` §Network] | Recommend **drop** (below). If kept: `pcl::NetworkTransfer` (header exists [V]); UWS job POST, 303 redirect, poll, CSV fetch, with a CSV parser written in C++. |
| N2 | Plate solving through PI's `ImageSolver.js` with the **user's** ImageSolver settings. If those select an online catalogue, ImageSolver itself goes online (VizieR). | `solveAndSplit()` in `gaia_depth_grade_lib.jsh` [V]; the doc says so [V] | Disappears: the module does not solve (section 3, S2). Any network use then belongs to whichever solver the user runs. |
| — | Offline Gaia DR3 | PI `Gaia` process over the local `.xpsd` [V]; **no network** [V doc] | `pcl::GaiaDatabaseFile` in-process (section 3, S4). No network. |

With N1 dropped, the module makes **no network connections**. CPD 6.1 documentation then shrinks to one line.

### Online Bailer-Jones / ESA TAP mode: keep or drop? **Recommend drop.**

1. **The accuracy gap is coverage, not the distance method.**
   - Offline on Scott's install gives match_rate **0.41** (0.53 at parallax S/N ≥ 2); online gives **0.73**, on the same Cygnus master [memory note `gaia-depth-grade-offline-dr3`, 2026-06; not re-run here].
   - The note attributes the gap to the database: Scott's DR3 slot holds the **DR3SP subset** (G ≲ 17.6, 20 files in /opt/gia3). That attribution is the note's [I], not a measurement.
   - PI's full Gaia DR3 database covers the same G<18 cut that online uses. Installing it should close most of the gap with no network code [I; worth one measurement before release].
2. **The PCL path also gets real parallax-error classes.** `GaiaStarFlag::Gold/Silver/BronzeParx` [V header] replace the synthetic G→σ knot table. That narrows the remaining Bailer-Jones advantage (a proper prior for low-S/N parallax). With S/N ≥ 5 and G<18 the 1/parallax bias is small [I].
3. **Cost and fragility in C++.**
   - Async TAP needs a UWS job POST, a 303 redirect, polling, a result fetch and CSV parsing.
   - Memory note `pi-externalprocess-gotchas` records "NetworkTransfer no redirects" [memory; not re-verified]. If that holds, the 303 handling must be written by hand.
   - The archive has a history of 408/500 failures that forced retries (`distances.py` comments [V]).
   - Estimate 2–3 days plus tests that cannot run offline. That time comes straight off the critical path.
4. **Compliance surface.**
   - Online is a CPD 6.1 network connection that must be documented and justified.
   - It is also the only reason the module would need a cache directory on disk (the ECSV cache) and the private-dir hardening code in the `.jsh`.
   - Dropping it means no files written outside the image windows.
5. **Offline is already the default** in the dialog (`offlineCheck.checked = true` [V]). Only the headless `grade` path uses online.

To keep any Bailer-Jones capability later, add it as a separate opt-in after the deadline. Do not put it on the 12-12 critical path.

---

## 3. Each step the scripts do today: how it runs in PCL, and whether PCL supports it

| Step | Today | PCL implementation | Supported? |
|---|---|---|---|
| S1 Target pick / browse / dialog | `Dialog` + ViewList + OpenFileDialog | `ProcessInterface` with a view selector. `NumericControl`, `CheckBox` and `PushButton` for the controls. A preview `Control` painted with `Graphics::DrawBitmap`. Planetary's interfaces are the template (PlanetaryStackInterface.cpp 1,469 lines, SERViewerInterface.cpp 508 [V wc]). | Yes [V headers: ProcessInterface.h, Control.h, NumericControl.h, Graphics.h, Bitmap.h] |
| S2 Plate solve | `ImageSolver.js` (`solver.Init/SolveImage`), then `writeWcsKeywords` with a hand-made **Y-flip** for FITS export | Stock PI has **no native solver process**: ImageSolver is a script (`/opt/PixInsight/src/scripts/AdP/ImageSolver.js`, and no ImageSolver module in `/opt/PixInsight/bin` [V ls]). `Console::ExecuteScript()` could run it [V Console.h] but it is JS; it is root-thread only and "knows nothing about whether the script worked" [V doc text]. AstroResolver is a native solver process installed here, but it is third-party (DeepSkyColors) [V doc], not stock. **Recommendation:** precondition. The target must already carry an astrometric solution, read with `AstrometricMetadata(const ImageWindow&)` and checked with `IsValid()` [V header]; fail loudly otherwise ("plate-solve first with ImageSolver or another solver"). | Reading a solution: yes [V header]. Solving from C++ without JS: no stock path [V]. |
| S2a WCS Y-flip hack | Needed only because the data went through FITS to astropy | **Deleted.** `AstrometricMetadata` works in PI's own pixel frame and uses the full solution, including the spline distortion model [V: `SplineWorldTransformation` in WorldTransformation.h]. Python only carried a linear CD+TAN, and the 4 px tolerance was tuned to absorb its ~2 px scatter (`config.py` comment [V]), so matches should improve [I]. | Yes |
| S3 Star/starless split | `new StarXTerminator; stars=true; unscreen=true; executeOn(view)`, then capture the new window by diffing `ImageWindow.windows` | `ProcessInstance sxt( Process( "StarXTerminator" ) ); sxt.SetParameterValue( true, "stars" ); sxt.SetParameterValue( true, "unscreen" ); sxt.ExecuteOn( view );` then diff `ImageWindow::AllWindows()`. The process lookup and parameter API are verified [V Process.h:92, ProcessInstance.h:234/591, ImageWindow.h:3050]. The process id and parameter ids are taken from the PJSR property names [I]. RCAstroSuite-pxm.so is installed here [V ls]. **Also add a second input mode:** the user supplies their own stars + starless views (StarNet2 etc.). That removes the hard paid dependency and gives the parity tests a deterministic input. | API yes [V]; ids [I], to be confirmed by a headless probe |
| S4 Footprint + local Gaia cone | `footprint` subcommand → `new Gaia` configure + search → TSV of `ra dec parallax G` | Footprint from `AstrometricMetadata::ImageToCelestial` on the centre and corners [V header]. **Catalogue:** open each `.xpsd` with `pcl::GaiaDatabaseFile(path)` and call `Search(GaiaSearchData&)` (centerRA/Dec, radius, magnitude limits, flags) [V GaiaDatabaseFile.h:302,471]. That returns `GaiaStarData{ra, dec, parx, pmra, pmdec, magG, magBP, magRP, flags}` [V], so **BP/RP is now available** and fixes the bug noted in section 0. Fallback: invoke the `Gaia` process through `ProcessInstance` (configure/search) and read its `sources` table parameter with `ParameterValue(id,row)`. API [V], parameter ids and readable output [I]. | Yes for GaiaDatabaseFile [V] |
| S4a Finding the user's `.xpsd` paths | Regex over `~/.PixInsight/core-*-pxi.settings` (`DR3DatabaseFilePathNN`, `DR3SPDatabaseFilePathNN`) | The keys live in the `Gaia` module group of the core settings file [V grep]. `Settings::ReadGlobal("/Gaia/DR3DatabaseFilePath00")` exists [V Settings.h] but fails if the owner read-protects the key. PJSR `Settings.read` was observed **not** to work for these keys [memory]. Recommend: a module preference ("Gaia DR3 database files", file list), auto-filled on first use by the same settings-file discovery in C++, editable by the user. | Discovery by file read: yes. ReadGlobal: [I], probably not. |
| S5 Prepare (detect + match + QA) | sidecar `prepare` | In-process C++ (section 1). Run in a `pcl::Thread` / parallel_for with the StatusMonitor, cancellable. QA goes to the console and the status label. | Yes |
| S6 Preview (render + nebula + screen blend + autostretch + PNG + 1:1 inset) | sidecar `preview` → PNG → `new Bitmap(png)` | Render at preview scale in memory, build the `Bitmap` directly (`Bitmap::Render(ImageVariant…)` [V Bitmap.h:471] or per-pixel), paint the inset from a full-res crop. No files. | Yes |
| S7 Execute (full render + optional nebula + PixelMath screen blend into a new window) | sidecar `render` / `nebula` → FITS → `ImageWindow.open` → `PixelMath ~((~A)*(~B))` | Compute in C++ (`1-(1-a)(1-b)`, identical to `display.screen_blend` [V]), then create `ImageWindow(w,h,n,32,true,…)` [V ImageWindow.h:348] as `<id>_depthgraded`. PixelMath is not needed. If wanted for history transparency: `ProcessInstance(Process("PixelMath"))` with an `expression` parameter [I ids]. | Yes |
| S8 Sidecar bootstrap: version probe, private work dir, symlink/uid/0700 checks | `sidecarBin`, `gdgWorkDir`, `gdgPrivateDir` | **Deleted.** No child process and no temp files. | n/a |
| S9 Headless `gaia_depth_grade.js` (env-var driven, online, FITS out) | PJSR | The process's `ExecuteOn(view)` runs prepare + render with stored parameters. It works from process icons, ImageContainer and other modules. The headless test probe runs through `tools/pi-headless.sh` like Planetary's `run-probe.sh`. | Yes [I, same pattern as Planetary] |

PCL calls into other processes: StarXTerminator (third-party), optionally Gaia and PixelMath. Each one runs through `ProcessInstance::ExecuteOn/ExecuteGlobal`.

---

## 4. Module/process structure, PCL building blocks and reuse

### Recommended structure
New module **`modules/gaia-depth-grade/` → `GaiaDepthGrade-pxm.so`**, built in the Rocky 9 container like NukeX/Planetary and released by `./release.sh`. Two processes:

1. **`GaiaDepthGrade`** (view process, ships 12-12).
   - **Parameters:** gains (brightness, size, contrast, saturation, halo), p_low/p_high, base σ, nebula strength/atmos/structure, detect σ, match tolerance, G limit, parallax S/N, split mode (StarXTerminator | supplied stars+starless views) and the view ids.
   - **Interface:** Prepare → Preview (with inset) → Execute, mirroring the dialog.
   - **ExecuteOn:** the full pipeline, for headless use.
2. **`NebulaReconstruct`** (global process over channel views, recon).
   - **Inputs:** SHO, duoband or OSC channel views, plus the GaiaDepthGrade prep (or its own Gaia pass).
   - **Outputs:** z / emissivity / dust (and later the 2D render) as new windows. GLB export is a file writer, about 200 LOC of plain binary glTF; pygltflib is not needed [I].
   - **12-12 scope:** port plan tasks 1–8 with parity tests. Exposing it in the UI and tasks 9–13 come after, as Part 2.

**Code layout.** A PCL-light core library `gdg_core` (plain `float/double` buffers in, `std::vector` out) holds the numerical layer, so ctest runs without PixInsight against Python goldens, as Planetary's PCL-free reader does. `src/module/` holds the process, parameters and interface glue. The two processes share `gdg_core`.

### PCL building blocks
Source: a header survey by a subagent (signatures read) plus my own spot checks. All [V header] unless marked.

- **FFT:**
  - `FFT1D.h` / `FFT2D.h` (`FRealFFT2D` etc.), with `OptimizedLength`.
  - `FFTConvolution`, `Convolution` and `SeparableConvolution` as image transformations.
  - `GaussianFilter` / `KernelFilter`.
  - Use them for speed only. Not for golden parity, because the boundary rule ≠ scipy reflect [I].
- **Morphology:** `ErosionFilter`, `DilationFilter`, `MedianFilter`, `MorphologicalTransformation`, `SeparableMedianFilter`.
- **Astrometry:**
  - `AstrometricMetadata(const ImageWindow&)` with `IsValid`, `ImageToCelestial`, `CelestialToImage`, `ImageCenterToCelestial`.
  - `ImageWindow::HasAstrometricSolution()`.
  - `WCSKeywords`, `ProjectionFactory`, `SplineWorldTransformation`.
- **Spatial search:**
  - `KDTree<T>` does a **box search** (`Search(pt, epsilon)`), not k-nearest. Matching = box search at the tolerance, then the minimum Euclidean distance. That is equivalent to `cKDTree.query(k=1)` followed by the `d > tol` reject, apart from exact-tie ordering [I].
  - `QuadTree<T>` does range search.
- **Surfaces:** `SurfaceSpline`, `ShepardInterpolation`, `GridInterpolation`. The current code needs none of these.
- **Linear algebra:**
  - `Matrix`/`Vector`, `InPlaceGaussJordan`, `InPlaceSVD`, robust `LinearFit`.
  - No Cholesky/LU and no public Levenberg–Marquardt wrapper; cminpack's sources are in `~/PCL/src/3rdparty/cminpack`.
  - The current code fits nothing beyond DAOFIND's closed-form marginal fits, so none of this is needed [V code].
- **Statistics:**
  - `Math.h` has `Median`, `OrderStatistic`, `MAD`, `Sn`, `Qn` and the biweight estimators.
  - There is no numpy-style interpolated percentile; write it on top of `OrderStatistic`/nth_element.
  - `ImageStatistics` and `Histogram` also exist.
- **Stars:**
  - `StarDetector::DetectStars` returns `Star{pos, rect, flux, signal, mad, …}`.
  - `PSFFit` / `PSFEstimator` (Gaussian/Moffat/VariableShape).
  - Only relevant if decision 4 goes the StarDetector way.
- **Gaia:** `GaiaDatabaseFile(path).Search(GaiaSearchData&)` (section 3, S4).
- **Network:** `NetworkTransfer` (SetURL/SetSSL/Download/Upload). Unused if online mode is dropped.
- **Processes / UI / threads:**
  - `Process(classId)` + `ProcessInstance` (`SetParameterValue`, `ParameterValue`, `ExecuteOn`, `ExecuteGlobal`).
  - `ImageWindow`, `Bitmap::Render`, `ProcessInterface`, `Console`, `StatusMonitor`, `Thread`.

### Reuse from NukeX / Planetary [V, survey]
- **Threading:**
  - `nukex::parallel_for_dynamic` (`modules/nukex/src/lib/gpu/include/nukex/gpu/parallel_for.hpp`), which is the project-mandated pattern.
  - Planetary's `ParallelBands` (`modules/planetary/src/stack/Parallel.h`).
- **Robust statistics:** `modules/nukex/src/lib/fitting/include/nukex/fitting/robust_stats.hpp` (median_inplace, mad, biweight). These are float-only and the median definition must be checked against numpy, so use them for non-parity paths only.
- **FFT:** Planetary's PCL-free `planetary::stack::Fft` (`src/stack/Fft.h`, lengths with prime factors 2/3/5) works for the large-σ nebula Gaussian if PCL's FFT is awkward on raw buffers.
- **Architecture and tests:**
  - Planetary's split, a PCL-free engine library plus a thin `src/module/`, is the pattern to copy.
  - `planetary_add_test` reuses NukeX's Catch2 (`modules/nukex/third_party/catch2`).
  - Python check scripts are registered with `SKIP_RETURN_CODE 77`, which is how goldens can be checked in ctest.
  - `NewImageWindow.*` and `FitsCards.h` are reusable module helpers.
- **Wiring for a new module [V survey of scripts]:**
  - `tools/build-env/build-modules.sh`: cmake/build lines, a ctest line in `RUN_TESTS`, and the `.so` check loop.
  - `release.sh`: everything is hand-listed per module. That covers DOC/DOC_IDS, `PKG_DESC`, the `modver` version read, `module_tgz`, the `published_pkg_reusable` guard, `RELEASE_PKGS`, the sign and package steps, `write_pkg`, and the `--package-test` mode (argument count 8 plus module loops).
  - Plus removal of `GAIA_ZIP`, `SIDECAR_TGZ`, the `GaiaDepthGradeSidecar` `PKG_DESC` entry and step 2c (native script signing) [V release.sh l.154–173, 496–508, 551–559, 679–689].
  - Budget about half a day inside P1/P8.

### What has to be written new (not in PCL, or PCL's version is not parity-safe)
- scipy-semantics Gaussian. PCL's GaussianFilter/SeparableConvolution exist, but their boundary handling ≠ scipy `reflect` [I], so they are unsafe for golden parity.
- numpy percentile/median.
- astropy sigma clip.
- DAOFIND port.
- flat grey closing (reflect).
- halo profile and per-star render.
- nebula and recon math.
- `.npy` reader for tests.

---

## 5. Phased work breakdown and critical path

Sizes are focused working days for me driving, with Scott's reviews async [I].

| Phase | Content | Size | Depends |
|---|---|---|---|
| P0 Golden harness | Python script (in the existing venv) that dumps every stage for synthetic fixtures and a real master to `.npy` + JSON. Stages: sigma-clip stats, DAO table, footprint, catalogue, catalogue pixel coords, matches, strength, modulation, halo profile, rendered stars, nebula depth field, nebula output, each recon term. Freeze the Python commit hash in the manifest. | 1 | — |
| P1 Skeleton | Module dir, CMake, `build-modules.sh` + `verify-portable.sh` + `release.sh` entries, version header, package-description, empty process + interface, ctest + npy reader | 1.5 | — |
| P2 Numerics layer | Gaussian (direct + FFT for large σ, both reflect), percentile, median, MAD, interp, grey closing, sigma clip; parity unit tests | 3 | P0, P1 |
| P3 DAOFIND port | Kernel, threshold, local-max/min_separation (3.0 default changed [V daofinder.py:151]), sharpness/roundness filters, marginal-fit centroids, flux definition; parity on synthetic + real | 3–4 | P2 |
| P4 Astrometry + catalogue + match | AstrometricMetadata footprint and world↔pixel; GaiaDatabaseFile search + path discovery/preference; parallax table; matcher; QA | 2.5 | P1 (P3 for e2e) |
| P5 Render + nebula | transform, modulate, halo profile, per-star render, nebula, screen blend, autostretch | 3 | P2 |
| P6 Process + interface | Parameters, ExecuteOn, Prepare/Preview/Execute, inset, StarXTerminator `ProcessInstance` + supplied-views mode, solved-image precondition, threading/cancel/progress, console QA, honesty keyword | 4 | P3–P5 |
| P7 Recon port (tasks 1–8) | C++ port + parity tests; `NebulaReconstruct` process shell | 3 | P2 (off critical path) |
| P8 Release cut-over | Remove the sidecar + script packages from release.sh/updates.xri; module docs (`doc/tools/GaiaDepthGrade/`); CPD 6.1 network section ("none"); headless probe + GUI smoke scripts; signed release; Scott's repo-pull test | 2.5 | P6 |

**Critical path:** P0 → P2 → P3 → P6 → P8, with P4/P5 in parallel with P3. That is about **19–21 working days of 32 calendar days**, so roughly a week of slack. P7 runs in that slack (subagent-parallel); if it slips, it ships in a follow-up release without affecting the deadline. **Hard internal checkpoint:** a signed release candidate by **2026-12-05**, leaving a week for Scott's repo-pull test and fixes.

### Test strategy: proving parity
- **Tier 1, primitive parity (ctest, no PI).**
  - Each primitive is run against scipy/numpy goldens: random images, edge sizes (odd/even, σ near window size), NaN handling.
  - Tolerance: float64 path |Δ| ≤ 1e-9 relative. float32 storage ≤ 1 ulp of the stored value.
- **Tier 2, stage parity (ctest, no PI).**
  - Each stage is fed the **golden inputs of that stage** (not its own upstream outputs), so differences do not compound.
  - DAO: same count, x/y within 1e-6 px, flux rel 1e-9.
  - Match: given golden catalogue pixel coordinates, an identical match set.
  - Halo, render, nebula, recon terms: per-pixel max |Δ| ≤ 1e-6.
- **Tier 3, real data.**
  - The existing tests use **no real data** (no `/mnt/qnap` or `astro_data` reference in `tests/` or `tools/` [V]).
  - Real masters exist at `/mnt/qnap/astro_data/processing-archive-2026-10-03/master/` [V find]. Examples: `masterLight_…FILTER-HaO3_RGB.xisf`, `…FILTER-Full_RGB.xisf`, `…FILTER-S2O3_RGB.xisf`, all 3840×2160. Target names are not in the filenames.
  - Split each one **once** with StarXTerminator, then freeze the stars/starless pair and the catalogue as fixtures.
  - Python (golden) and C++ (supplied-views mode) run on identical inputs. Assert end-to-end per-pixel |Δ| ≤ 1e-5 and identical QA numbers.
- **Tier 3b, catalogue equivalence.** GaiaDatabaseFile search returns the same rows as the PI Gaia process TSV for the same cone.
- **Tier 3c, astrometry improvement (deliberately not parity).**
  - With the real `AstrometricMetadata` spline solution, the match set will differ from Python's linear-WCS one.
  - Assert match_rate(C++) ≥ match_rate(Python) and median offset ≤ Python's on the same master.
- **Tier 4, inside PI.** Headless probe through `tools/pi-headless.sh` (ExecuteOn over the fixtures, marker-file asserts), and a GUI smoke on a private Xvfb (Prepare/Preview/Execute), modelled on Planetary's `run-probe.sh` / `gui-smoke.sh`.
- **Tier 5.** Scott's repo-pull human test on the signed release.
- **Recon real-data validation (plan task 13).**
  - It needs **integrated** M8/M42 duoband and broadband masters. Only **subs** exist at `9_24_2023/M42`, `8_8_2023/M8` and `5_10_2023/M8` [V ls].
  - Integrating them is a prerequisite: about half a day with FastIntegration/NukeX.
  - This is outside the 12-12 critical path.

---

## 6. Risks and decisions

### Risks
- **R1 DAOFIND parity.** The largest single item. photutils 3.0 changed defaults (min_separation, the flux definition); match the exact installed version (3.0.0 [V]). Mitigation: a stage-isolated golden.
- **R2 Gaussian boundary/large σ.** Nebula σ ≈ 65 px on 3840×2160 (`0.03·2160`) [V formula]. A direct separable kernel of about 520 taps is fine multithreaded; FFT is optional. Parity requires scipy's reflect, so PCL's own filters are unsafe [I].
- **R3 StarXTerminator ids.** The process id and parameter ids are inferred from PJSR. Confirm with one headless probe early in P6. The supplied-views mode de-risks it.
- **R4 Gaia DB path discovery.** Parsing the core settings file is what ships today. A module preference is the robust fallback.
- **R5 Retiring the old packages on users' machines.**
  - Whether PI's updater removes `bin/gaia-depth-grade-sidecar` and the old `src/scripts/GaiaDepthGrade/*` when their packages leave `updates.xri` is unknown [I].
  - If it does not, users keep a stale sidecar and a Script-menu entry that fails at `sidecarBin()`.
- **R6 Unsolved images.** Users must plate-solve first. This is a UX change from today's automatic solve.
- **R7 Schedule.** About 20 days of work in 32 calendar days. The recon port is the release valve.

### Decisions for Scott (each with my recommendation)
1. **Drop the online Bailer-Jones/ESA TAP mode?** Yes. Go offline-only with zero network connections, and measure the full-DR3 match rate once (section 2).
2. **Plate solving: require a pre-solved image instead of solving from C++?** Yes. There is no stock native solver, and running ImageSolver.js through `Console::ExecuteScript` is still JS.
3. **Offline distance uncertainty: switch from the synthetic G-knot σ to Gaia's Gold/Silver/Bronze parallax-error flags?** Yes, but as a second step after parity is proven with the knot table, so the port and the behaviour change are tested separately.
4. **Detection: port DAOFIND exactly, or switch to `pcl::StarDetector`?** Port DAOFIND for 12-12, because it is the reference the thresholds and tolerance were tuned against. Evaluate StarDetector later.
5. **Add the "supply your own stars + starless views" mode alongside StarXTerminator?** Yes. It removes the hard paid dependency, enables StarNet2, and is how the parity tests get deterministic inputs.
6. **Recon in the 12-12 release?** Port tasks 1–8 in C++ with parity tests if slack allows, but expose no recon UI until Part 2. Build tasks 9–13 directly in C++ rather than finishing them in Python first.
7. **Freeze the Python recon branch now (no more Python recon commits) so it serves as the oracle?** Yes.
8. **Old package retirement (R5): verify the updater's removal behaviour with a throwaway package in a test slot before the cut-over?** Yes.
9. **Module/process ids `GaiaDepthGrade` / `NebulaReconstruct`, treated as stable once shipped?** Yes. The old script's `#feature-id` is a menu id, not a process id, so no clash is expected [I].

---

## Addendum (2026-11-10, later the same day): Scott's rulings. These supersede sections 2, 3 (S2) and 6 where they conflict.

**Ruling: no functionality is lost.** So:

### A1. Online Bailer-Jones mode stays, ported to `pcl::NetworkTransfer`
API [V `/opt/PixInsight/include/pcl/NetworkTransfer.h`]:
- available: `SetURL`, `SetSSL(useSSL, forceSSL, verifyPeer, verifyHost)`, `SetCustomHTTPHeaders`, `SetConnectionTimeout`, `POST(fields)`, `Download()`, `ResponseCode()`, `ContentType()`, `ErrorInformation()`;
- data arrives through `OnDownloadDataAvailable(handler, Control&)`, whose handler is a `Control` member function;
- there is no response-header accessor;
- it does not follow redirects [memory `pi-externalprocess-gotchas` #2: no CURLOPT_FOLLOWLOCATION].

The async UWS flow still works without either, as live probes of the ESA archive showed [V curl, 2026-11-10]:
- `POST /tap-server/tap/async` returns **303 with a `text/plain` body `Location: https://gea.esac.esa.int/tap-server/tap/async/<jobid>`**, so the job URL can be parsed from the body;
- `GET <job>/phase` returns `COMPLETED`;
- `GET <job>/results/result` returns **200 `text/csv` directly** (no redirect).

So the async path ports with no redirect support and no `curl` child process.

The status-notice fetch astroquery makes (`GAIA_MESSAGES = "notification?action=GetNotifications"`, astroquery `gaia/core.py:44`) currently returns **404** at both `/tap-server/notification` and `/notification` [V curl]. The port logs that the notice service is unavailable and carries on. Only the notices are skipped; the query still runs.

Documentation: the existing CPD 6.1 network section of `doc/GaiaDepthGrade/GaiaDepthGrade.html` carries over, with the endpoint list above.

### A2. The module still plate-solves for the user, natively
- **What PI 1.9.6 has [V].** The installed core is **PixInsight 1.9.6** (`doc/scripts/ImageSolver/ImageSolver.html` shows "PixInsight 1.9.6 / ImageSolver 6.6.0"). The 1.9.6 "native solver" is **`pcl::BlindAstrometricSolver`**, a **PCL class**, not a process.
  - Its header ships with PI at `/opt/PixInsight/include/pcl/BlindAstrometricSolver.h`, and its source at `/opt/PixInsight/src/pcl/BlindAstrometricSolver.cpp` (1,016 LOC), together with `AstrometricIndex.{h,cpp}`, `AstrometricIndexFile.{h,cpp}` and `AstrometricIndexGenerator.{h,cpp}`.
  - It has 0 calls into the core API (`api->`), so it compiles into our module [V grep].
  - PJSR exposes it as the `BlindAstrometricSolver` object [V `doc/pjsr/objects/`].
  - There is no solver process in `bin/` or `doc/tools/` [V], so `ProcessInstance` cannot reach one.
- **PCL versions [V].**

  | Source | PCL version | Has BlindAstrometricSolver? |
  |---|---|---|
  | PI 1.9.6 bundle (`/opt/PixInsight/src/pcl/Version.cpp`) | 2.13.0 build 1072, released 2026-10-28 | yes |
  | `~/PCL` (rewritten during this session at 15:05, presumably by the parallel agent) | 2.11.0 build 1070 | no |
  | gitlab `pixinsight/PCL` master (`5a39021`, 2026-09-28, "PixInsight 1.9.5 / PCL 2.11.0") | 2.11.0 | no |
  | Container pin `3a4af80` (2025-05-09) | older | no |

  So the **only** PCL source with the solver is the PI 1.9.6 bundle. It ships a buildable tree [V `src/pcl/linux/g++/Makefile`, `src/3rdparty/*`]. The new sources include headers that the older PCL lacks (`FileMapping.h`, a changed `StarDatabaseFile.h`) [V], so vendoring the solver files alone into the old PCL is not clean.
- **Index files.** The blind solver needs an **astrometric index** (XPSD) selected in the Gaia process preferences ("Astrometric Index File") [V BlindSolver doc]. **None is configured on this box** [V settings grep]. This box now has base `gdr3-1.0.0-*.xpsd` (38 files in /opt/gia3) [V]. That may already raise the offline match rate above the DR3SP-era 0.41 [I].
- **How ImageSolver 6.6.0 solves** [V `src/scripts/ImageSolver/ImageSolverEngine.js`]:
  - seeds from image metadata (`extractImageMetadata`: centre RA/Dec, focal length, pixel size; l.332+);
  - queries the Gaia catalogue;
  - matches with the **StarAlignment process** in `OutputMatrix` mode against a reference, reading `outputData` pairs (l.378–440);
  - detects stars with StarDetector;
  - falls back to `BlindAstrometricSolver` when there is no starting point (l.945);
  - fits a spline distortion model.
  - It is licensed under PCL License 2.0 [V header].
- **Option (a): native C++ solver. Recommended.** It reproduces ImageSolver's path from parts that are all on-platform:
  - seed from metadata;
  - catalogue from `GaiaDatabaseFile`, or the TAP client when the user has no local DR3;
  - `pcl::StarDetector`;
  - StarAlignment through `ProcessInstance` (OutputMatrix pairs);
  - an affine fit, then distortion through `SplineWorldTransformation` / `SurfaceSpline`, written with `AstrometricMetadata::Write`;
  - `BlindAstrometricSolver` (compiled from PCL 2.13) for images without a seed.

  Size: **6 days**, in a parallel track; critical-path impact about 1.5 days (plan below).
- **Option (b): run PI's stock ImageSolver from the module.**
  - `pcl::Console::ExecuteScript/ExecuteScriptOn` exist [V Console.h], and ImageSolver honours `Parameters.isViewTarget` / `non_interactive` [V ImageSolver.js l.3880–3897]. So a module **can** launch it.
  - The PCL documentation says such calls are root-thread only. They "know nothing about whether the script worked", and they can fail script authorisation [V Console.h doc text].
  - It is not our JS, but it is a module driving a script. That is close to the "bridge" pattern the PTeam already objected to [I].
  - Size: about 1 day.
  - Keep it **only** as a contingency if the solver gate (Task 15) fails, and only with Scott's explicit OK.
- **Minimum PI version: 1.9.6** (recommended).
  - The module is built against PCL 2.13.0 (needed for `BlindAstrometricSolver`).
  - The blind fallback needs 1.9.6's astrometric-index preference.
  - 1.9.6's ImageSolver already does blind fallback, so parity is defined against 1.9.6.
- **Repository range.** The update repository currently declares `1.8.0:2.0.0` for all packages [V `repository/updates.xri` l.8, 50]. The XRI spec makes packages available per `<platform>` element, and only when the running core is within its range [V `doc/docs/PIRepositoryReference`]. So the module goes in a **second `<platform os="linux" arch="x64" version="1.9.6:2.0.0">` block**, and the other packages keep `1.8.0:2.0.0`. That two blocks for the same os/arch are accepted is [I]. Task 2's probe checks it with a scratch repository.
- **Build.** The module is built against **PCL 2.13.0 from the PI 1.9.6 bundle**. The plan proposes a second PCL tree in the image (`/opt/pcl-2.13`, from a sha256-pinned tarball of `/opt/PixInsight/{include,src}`) used only by this module. NukeX and Planetary stay on their pin. Scott decides; the alternative is moving the whole image once gitlab publishes 2.13.

### A3. PCL 2.11/2.13 counterparts to the new 1.9.6 PJSR objects [V headers]
- **KDTree / QuadTree** now have `NearestNeighbor(pt, maxDistance, &distance)` and `NearestNeighbors(pt, k, …)` (KDTree.h:748/789, QuadTree.h:692/732). That gives an exact counterpart to `cKDTree.query(k=1)` for the matcher, and corrects the earlier "box search only" note, which described the 2.9.4 headers.
- **`LevenbergMarquardt.h`** (`LevenbergMarquardt::Fit`, `LevenbergMarquardtFunction`) is in 2.11 and 2.13. That corrects the earlier "no public LM" note. It is useful for the solver's affine/TAN refinement.
- **FFTRegistration.h** has changed. `Association.h`, `FeatureRegistration.h` and `Warp.h` are new in 2.13. None is needed for parity.

### A4. Deadline re-check
- The rulings add about **10 days** of work: the TAP client (2.5) and the solver (6), plus golden/probe/test overhead.
- The total is now about **37 working days serial**. **Serial execution does not fit 2026-12-12.**
- With three parallel tracks (stars pipeline; catalogue + solver; UI and recon later), the critical path is about **19 days**. A release candidate by **2026-12-01** leaves about 11 days for Scott's repo-pull test and fixes.
- **The deadline holds only with parallel execution**, plus the solver gate on 2026-11-21 (Task 15).

### A5. Which PCL has BlindAstrometricSolver? (evidence)
- **Only PCL 2.13.0** (build 1072, the PI 1.9.6 bundle).
  - It is absent from `~/PCL` 2.11.0 (gitlab master as of today) and from the whole gitlab tree at `5a39021` [V].
  - The bundle includes a complete buildable 2.13 source tree (`src/pcl/linux/g++/makefile-x64`, `-std=c++20`, plus `src/3rdparty/*`) [V].
  - So libPCL 2.13 can be built in the container even though upstream has not published it.
- **Using the solver from a module built against 2.11 or 2.9.4 would mean vendoring** `FileMapping`, a changed `StarDatabaseFile` and the index classes next to an older libPCL. That gives two layouts of the same classes in one binary (ODR) [I], so it is rejected.
- **Build plan:**
  - a second PCL tree `/opt/pcl-2.13` in the image;
  - C++20 for this module;
  - gcc-toolset-14 (already in the image);
  - `verify-portable.sh` floor unchanged (GLIBCXX 3.4.30), proven on the built `.so` in plan Task 2.
