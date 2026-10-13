# NukeX

Distribution-fitted image stacking and auto-stretch for PixInsight, with
filter-aware colour science: dual-narrowband OSC data is decomposed into
emission lines through the camera's measured quantum efficiency and composed
with a calibrated palette.

Rather than averaging or sigma-clipping, NukeX fits a distribution to every
pixel's samples across the stack and estimates the true signal from the fitted
model. Each pixel carries its own record — Welford accumulators, a histogram,
the fitted distribution and its robust statistics, per channel — and the
selection is made from that record.

## Install (PixInsight 1.8.9+, Linux x64)

Resources → Updates → Manage Repositories → Add:

    https://raw.githubusercontent.com/scarter4work/astro-pi/main/repository/

then Check for Updates and restart. The package installs `bin/NukeX-pxm.so`
and its documentation, `doc/tools/NukeX/` (this information, plus the
third-party notices), under the PixInsight base directory; the camera (QE)
database is built into the module.

## Use

Process → NukeX. Add light frames (and optional flats), pick a stretch — Auto
is recommended — and Execute.

Outputs:

| window | contents |
|---|---|
| `NukeX_stacked` | the stack, linear |
| `NukeX_composed` | 3-channel sRGB, when derived emission-line slots exist |
| `NukeX_stretched` | the stretched result |
| `NukeX_noise` | per-pixel noise estimate |

NukeX reads `FILTER`, `BAYERPAT` and `INSTRUME` from the FITS headers to decide
how each frame is routed. An unrecognised dual-narrowband filter on Bayer data
stops the batch rather than guessing; an unrecognised mono filter warns and
stacks as luminance. To teach it a camera or filter it does not know, see
[docs/qe_overrides_format.md](docs/qe_overrides_format.md).

## Network connections

NukeX makes one kind of network connection: the **camera-database update
check**. Nothing else in the module uses the network; frames are read locally
(the FITS library is built without network support, and NukeX opens only the
files you select).

- **What:** HTTPS GET requests to
  `https://raw.githubusercontent.com/scarter4work/astro-pi/main/repository/`.
  A check fetches `qe_manifest.json` and `qe_manifest.json.sig`; an
  installation fetches `qe_database.json` and `qe_database.json.sig`.
- **When:** automatically the first time the NukeX window is opened in a
  PixInsight session, at most once every 7 days; and whenever you press
  **Check now**. Never at PixInsight startup or module load.
- **Consent:** when a newer, verified database exists NukeX asks *"Install it
  now?"*. Nothing beyond the manifest is downloaded, and nothing is installed,
  unless you answer Yes. A declined version is not offered again by automatic
  checks.
- **What is sent:** nothing but the requests themselves -- no identifier, usage
  data or image data. GitHub sees your IP address and standard request headers.
- **Security:** TLS with certificate and host verification is mandatory (plain
  HTTP is refused); responses are capped at 8 MiB; 20 s connection timeout.
  The manifest and database are Ed25519-signed and verified against a public
  key compiled into the module; the database must also match the SHA-512 in
  the signed manifest. Older databases (rollback) and newer formats are
  refused. Nothing is written unless every check passes, and the replacement
  is atomic. Offline is silent.
- **Turning it off:** untick **Check for camera database updates**; only
  **Check now** then contacts the server. Delete
  `~/.config/nukex4/qe_database.json` to return to the built-in database.

## Files written outside the PixInsight installation

| location | files | purpose |
|---|---|---|
| `~/.config/nukex4/` (mode 0700; `/tmp/nukex4/` when `HOME` is unset) | `qe_update_state.json` | update-check setting, interval (default 7 days), last check time/result, installed and declined versions |
| | `qe_database.json` | downloaded camera database -- only after you accept an update; used while newer than the built-in one |
| | `filter_aliases.json` | filter names you taught NukeX in the unknown-filter dialog |
| | `phase8_user.sqlite` (+ `-wal`, `-shm`) | your stretch ratings: random run id, time, stretch and filter class, image statistics, stretch parameters, ratings -- no file names, paths or header text |
| | `phase8_user_model.json` | stretch-parameter model trained from your ratings (Auto stretch) |
| | `phase8_user.sqlite.corrupt.<time>` | a damaged rating database set aside (only on corruption) |
| frame cache: the `cacheDirectory` parameter, default `$XDG_CACHE_HOME/nukex4/cache`, else `~/.cache/nukex4/cache`, else `/var/tmp` | `nukex_cache_XXXXXX` | aligned frame data for the running stack (about 13 GB for 33 frames of a 24 MP colour camera); deleted when the run ends, but left behind if PixInsight crashes mid-run -- safe to delete while PixInsight is not running |
| | `nukex_cube_XXXXXX` | only when the working data does not fit in memory; unlinked as soon as it is created, so it never outlives the run |
| `/tmp` | `nukex_progress.log` | progress log of the most recent run (restarted each run) |
| | `nukex_heartbeat.txt` | current phase and elapsed time, rewritten every 10 s during a run |

Short-lived `.tmp`/`.stage` files appear beside the `~/.config/nukex4/` files
while they are replaced. NukeX also stores one PixInsight setting,
`NukeX/Phase8/RatingPopupSuppressed`, through PixInsight's settings interface.
None of this data is transmitted. NukeX saves no images: results open as
image windows for you to save.

## External programs

None -- NukeX launches no program or process. For GPU acceleration the
OpenCL loader built into NukeX reads `/etc/OpenCL/vendors/` (or the standard
`OCL_ICD_VENDORS` / `OCL_ICD_FILENAMES` overrides) and loads your GPU vendor's
installed OpenCL driver library; without a usable GPU NukeX runs on the CPU.

## Known limitations

- **One mono filter per batch.** Stack L, R, G and B separately and combine
  the results. A batch carrying more than one mono filter is refused with an
  explanation rather than producing empty channels.
- **Very long sessions lose frames at the ends.** Alignment is against a
  single reference frame, so frames far from it in time can fail once
  tracking drift accumulates. On a seven-hour set, everything within about
  90 minutes of the reference aligned and the outermost 21 of 72 frames did
  not.
- **A 24 megapixel colour stack wants more than 32 GB of RAM** to stay out of
  swap. It completes on less; it swaps while doing so.

## Build from source

    cmake -S . -B build && cmake --build build -j && ctest --test-dir build

Requires PCL at `~/PCL` (or pass `-DPCLDIR=`), system Eigen 3, glog and Ceres,
and OpenCL headers. GPU work falls back to CPU when no device is available.

See [CHANGELOG.md](CHANGELOG.md) for release notes.

## Licence

MIT. Copyright (c) 2026 Scott Carter.

This product is based on software from the PixInsight project, developed by
Pleiades Astrophoto and its contributors (https://pixinsight.com/).

The shipped module also contains CFITSIO, Ceres Solver, Eigen (MPL-2.0), the
Khronos OpenCL ICD Loader, JSON for Modern C++, SQLite and TweetNaCl. Their
notices and the full PixInsight Class Library License ship with the module in
[doc/NukeX/THIRD_PARTY_NOTICES.txt](doc/NukeX/THIRD_PARTY_NOTICES.txt);
the user documentation is [doc/NukeX/NukeX.html](doc/NukeX/NukeX.html).
