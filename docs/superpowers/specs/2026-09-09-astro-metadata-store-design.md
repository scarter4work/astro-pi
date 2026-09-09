# Astro Metadata Store — Design

**Date:** 2026-09-09
**Status:** Design approved in brainstorming; not yet planned or implemented
**Scope:** Phase 1 only (build the store and earn trust in it). Phase 2 (physically
reorganizing the archive) is deliberately excluded and gets its own spec.

---

## 1. Problem

`/mnt/qnap/astro_data` holds **28,135 `.fit` files across 845 leaf directories and 248
top-level date directories**, accumulated across three naming eras (`10_1_2023`,
`07_02_2025`, `2026-09-08`) and at least seven cameras. Nothing on disk records what a
frame is of, whether it is any good, or which session it belongs to. Every question about
the archive is answered today by re-deriving it from directory names and filenames, both
of which are unreliable.

Three failures motivated this, all observed rather than hypothesised:

**Absence is ambiguous.** On 2026-09-09 a whole-tree audit found 39 IC 59 frames present on
the ASIAIR but missing from the NAS. That signature is identical whether the pull dropped
them or the operator culled them deliberately. An automated "repair" was queued and would
have silently re-added frames that had just been culled on purpose; it was killed by luck
of timing, not by design. The filesystem cannot express *why* a frame is absent.

**Names do not identify objects.** `M33` (6 dirs) and `Triangulum Galaxy` (7 dirs) are the
same galaxy. `Veil2`, `Pickering-triang` and `NGC6960` are the same complex. Worse, the
split happens *inside* single objects: both `M31` (13) and `M 31` (12), `M45` (6) and
`M 45` (6), `IC1848` (6) and `IC 1848` (4) exist as sibling directories. 30 directories are
named only `Lights`/`lights`, and 16 are `FOV Cross` — placeholders with no object at all.

**Filenames are not unique.** Of 28,135 files there are 25,027 distinct names, so **1,989
names collide**. Any index keyed on filename — including the audit method used to date — is
approximate.

## 2. Goal

A metadata store that, for every frame in the archive, records **what it is of, whether it
is good, and what it belongs to** — with enough provenance that its answers can eventually
be trusted to reorganise the archive by object.

The endgame layout, agreed during brainstorming and driving the schema:

```
by-object/<canonical>/<filter>/<project>/<panel>/
```

Phase 1 delivers this as **queries and views only**. Nothing on the NAS moves.

## 3. Decisions

| Decision | Choice | Rationale |
|---|---|---|
| Host | scott-server LXC | 48 cores, 95 GB RAM free, 0.26 ms wired RTT, ZFS snapshots |
| DB location | `/data/astro-metadata/store.sqlite` on local ZFS, **never** the NAS | SQLite/Postgres rely on POSIX byte-range locking; CIFS does not honour it reliably. This is a known corruption path. |
| DB engine | SQLite | ~28k rows, single writer (the scanner). Zero admin. Migration to Postgres is mechanical if concurrent writers ever appear. |
| Durability | Per-leaf-dir manifest JSON mirrored to NAS | 845 files, not 28,135. Labels survive DB loss and travel with a moved folder. |
| Read path | Local ZFS copy `/data/backups/qnap/astro_data` | Disk speed instead of CIFS for hashing/solving/measuring |
| Live delta | Read-only CIFS `/mnt/qnap-source` | Catches frames newer than the last backup cycle |
| Camera in path? | No — queryable field only | Filter names are already camera-specific (`HaO3`, `Lqef`, `S2O3`, `LPro`), so a camera path level would be almost entirely single-child directories |
| Canonical names | No-space Messier (`M42`), variants as aliases | Matches how the operator writes them. Changing this is a preference flip, not a rescan. |
| FITS header writes | **Never** | Mutating 28k originals is the one irreversible option. Two silent header-corruption modes are already documented (PixInsight renormalisation, XISF keyword loss). |
| Deletion by machine | **Never** | Culling moves frames to `rejected/`, matching existing practice. Reversal is one `mv`. |

## 4. Architecture

```
                 scott-server (LXC)
   ┌──────────────────────────────────────────────┐
   │  scanner  ──reads──►  /data/backups/qnap/    │  local ZFS, full archive copy
   │     │                  astro_data            │  (lags one backup cycle)
   │     │                                        │
   │     ├────────reads───►  /mnt/qnap-source     │  CIFS, READ-ONLY, live delta
   │     │                                        │
   │     └────writes────►  store.sqlite  (ZFS)    │  snapshotted
   └──────────────────────────────┬───────────────┘
                                  │ export
                                  ▼
              <leaf dir>/.astro-manifest.json  on the NAS
```

The scanner is the only writer. The read-only mount means the scanner physically cannot
damage the live archive; the ZFS copy means the expensive passes never touch the network.

Both source paths exist on the Proxmox host, not inside a container, so the LXC receives
them as **read-only bind mounts** (`pct set <id> -mp0 /data/backups/qnap,mp=/archive,ro=1`
and likewise for `/mnt/qnap-source`). Read-only is enforced at the mount, not by
convention in the scanner.

**Solver scratch space (implementation constraint).** `astap_cli` writes its `.wcs`/`.ini`
sidecars next to the input file, so it **cannot** be pointed at either source mount — the
CIFS mount is read-only by design and the ZFS backup must not be polluted. Every solve
copies its frame to local scratch first and cleans up afterwards. Verified 2026-09-09.

**ASTAP deployment: DONE (2026-09-09).** `astap_cli` (CLI-2026.06.29) plus all 1,476 D50
`.1476` files, 926 MB, installed to `/opt/astap` on scott-server and symlinked to
`/usr/local/bin/astap_cli`. The GTK GUI build was deliberately **not** copied — it hangs a
headless shell, so its absence is the safeguard. Functionally verified: a blind whole-sky
solve of a known IC 1848 frame reproduced the workstation's result exactly
(RA 02:51:27.0 +60°04′11″, 11/11 quads, identical solution matrix) in 11.7 s against the
workstation's 5.9 s — slower per frame on the older Xeon cores, but 48 of them are
available for parallel solving.

Note the 106 GB PixInsight Gaia DR3 in `/opt/gia3` is **not** usable here — PixInsight
`.xpsd`, ASTAP `.1476` and AstroIndexer `aixqdb` are three incompatible formats. D50 is
the correct and sufficient database for solving.

## 5. The three keys

Frames are identified by three different keys because they answer three different questions.

| Key | Derived from | Answers |
|---|---|---|
| **Content hash** (blake3) | raw bytes | Is this the same file? Resolves the 1,989 filename collisions, exact duplicates, integrity |
| **WCS solution** | pixel *geometry*, via ASTAP | Where was this pointed? Exact deep-sky identity |
| **Perceptual fingerprint** | downsampled pixel *appearance* | Which frames are the same field? Identifies the unsolvable frames |

**The WCS solution is the primary identity for deep-sky frames.** Verified 2026-09-09: a
blind whole-sky solve (`-r 180`, no positional hint) of an IC 1848 frame returned
RA 02h51m27.0s +60°04′11″ in **5.9 s**, matching 11 of 11 quads from 500 detected stars.
The frame's own `RA`/`DEC` header cards were **10.6′ off** the truth, which is why header
coordinates are used only for coarse clustering and never for identity.

The fingerprint is a difference hash (dHash) computed over a downsampled frame that has
first been percentile-normalised (clipped to its own 1st–99th percentile, then rescaled).
Normalisation is required, not cosmetic: raw astronomical frames vary enormously in
absolute level with exposure, gain and sky brightness, and an un-normalised hash would
cluster by *exposure* rather than by *field*. Similarity is Hamming distance, with the
threshold calibrated against known same-field and known different-field pairs during
implementation rather than guessed up front.

**The fingerprint makes the expensive step cheap.** Frames sharing a near-identical
fingerprint are the same field, so exactly one per cluster is solved and the result
propagates to the rest. This collapses ~23,952 solves to roughly the number of distinct
fields. It also removes a documented blind spot: sampling one frame per *directory* is
unsafe for generic `LIGHT` dirs that mix targets, but sampling one per *fingerprint
cluster* is safe everywhere, because the clustering comes from pixels rather than folders.

**The fingerprint proposes; the solve confirms.** An object is never filed on appearance
alone, so a mosaic panel is never mistaken for a different nebula.

**Solar-system frames are the exception, and there the fingerprint is the whole answer.**
Verified 2026-09-09: a Moon frame failed to plate-solve entirely, killed at 90 s against
5.9 s for the deep-sky blind solve — there is no star field to match. 405 frames are in
this category (Moon 269, Sirius 72, Saturn 22, Jupiter 16, Planetary 10, Neptune 8,
Vega 7, Mars 1). These are identified by fingerprint plus simple image statistics
(saturated fraction, size of the largest contiguous bright region), which separates
"large bright disc" from "small bright disc" from "point source" without ambiguity.

## 6. Pipeline

Nine passes. Each is independently resumable and idempotent, keyed on content hash.

1. **Inventory** — walk the ZFS copy, then the live CIFS delta. Record path, size, mtime,
   content hash. Exclude `_dedup_quarantine_*` and `@Recycle`.
2. **Classify frame type** — light / dark / flat / bias / derived / unknown, from filename
   convention cross-checked against the `IMAGETYP` header. Current counts: 23,952 light,
   1,205 flat, 400 bias, 341 dark, 949 derived (Siril `pp_light`/`r_pp_light`,
   `ASIVideoStack`, `AS_P*`), 7 autosave, 1,281 unclassified. Only lights get an object;
   calibration keys on camera, exposure and temperature instead.
3. **Read headers** — `RA`, `DEC`, `OBJECT`, `INSTRUME`, `FILTER`, `EXPTIME`, `DATE-OBS`,
   `FOCALLEN`, `XPIXSZ`, `NAXIS1/2`, `IMAGETYP`, `GAIN`, `CCD-TEMP`. Parse raw 2880-byte
   header blocks (80-char cards to the `END` terminator), reading ~3–30 KB per file rather
   than pulling pixel data.
4. **Fingerprint** — compute the perceptual key from a downsampled frame; cluster.
5. **Cluster by pointing** — group frames whose header centres fall within a tolerance
   scaled to each frame's own FOV (`206.265 × XPIXSZ / FOCALLEN` arcsec/px × `NAXIS`).
   Coarse only; refined by the solve.
6. **Solve** — `astap_cli` on one representative per fingerprint cluster, propagating the
   WCS to cluster members. Blind (`-r 180`) where no usable header hint exists.
7. **Name** — resolve each solved field centre against SIMBAD; cross-check against any
   `OBJECT` card and the directory name. Ambiguous fields are surfaced for confirmation
   rather than guessed.
8. **Measure quality** — `astap_cli -analyse` for star count and HFR, plus sky background.
   Background is measured because star counts saturate and will not by themselves catch a
   thin-cloud frame.
9. **Group** — sessions from capture instants using a dusk cutoff (frames after midnight
   belong to the night that started the session); adjacent nights on the same object and
   filter merge into one project.

## 7. Data model

```sql
frames
  content_hash TEXT PRIMARY KEY
  path, filename, size, mtime, first_seen, last_seen
  frame_type            -- light|dark|flat|bias|derived|unknown
  camera, filter, exptime, binning, gain, ccd_temp
  captured_at           -- DATE-OBS, falling back to the filename capture instant
  header_ra, header_dec, focallen, xpixsz, naxis1, naxis2
  fingerprint
  field_id              -- FK, nullable
  disposition           -- present|missing|quarantined
  disposition_reason, disposition_at, disposition_source

fields                  -- one distinct pointing
  id PRIMARY KEY
  solved_ra, solved_dec, fov_w, fov_h, rotation, scale_arcsec_px
  solve_source          -- astap|propagated|none
  solve_at, quads_matched, solve_attempts
  object_id             -- FK, nullable

objects
  id PRIMARY KEY
  canonical_name, object_type   -- deepsky|solar|star
  simbad_id, ra, dec

aliases
  object_id, alias, source      -- dirname|object_card|simbad|manual

identity_assertions            -- provenance for every identity claim
  field_id, object_id, source, confidence, asserted_at

quality
  content_hash, star_count, hfr, sky_background, measured_at

projects
  id PRIMARY KEY
  object_id, filter, kind       -- session|mosaic
  started_at, ended_at

frame_projects
  content_hash, project_id, panel   -- panel nullable, e.g. "1-1"
```

### Confidence model

Every identity claim records how it was made. This is what makes the store safe to
eventually act on.

| Source | Confidence | May drive a Phase 2 move? |
|---|---|---|
| `solved` — ASTAP solve matched to catalogue | high | yes |
| `propagated` — fingerprint sibling of a solved frame | high | yes |
| `manual` — operator confirmed | authoritative | yes |
| `object_card` — `OBJECT` header, consistent with coordinates | medium | no |
| `dirname` — directory name only | low | no |

A directory-derived label and a solved one are both "labels"; only one is ever allowed to
move a file.

## 8. Disposition and culling

**Auto-cull means auto-quarantine, never delete.** A frame breaching threshold is moved to
a sibling `rejected/` directory — matching existing practice — and the store records the
metric, the threshold, and the timestamp that moved it. Reversal is one `mv`. Machine
deletion is out of scope permanently, not just for Phase 1.

**Thresholds are derived per session, not globally.** Seeing varies night to night, so a
fixed HFR limit would cull a whole mediocre night and keep junk from an excellent one.
Each session's own distribution sets its limits, with an absolute floor for frames that
are unambiguously unusable.

**A frame that disappears between scans becomes `missing`, never deleted from the store.**
This is the direct fix for the motivating failure: the operator marks it `quarantined`
with a reason, and the archive then carries a permanent record that its absence was
deliberate. Once that exists, the whole-tree audit stops producing false alarms, because
absence and intent are finally distinguishable.

Historical culls — including the 39 IC 59 frames from the night of 2026-09-07 (17 Lqef at
indices 0006, 0021–0022, 0038–0046, 0052–0053, 0056, 0075–0077; 22 HaO3 at 0019–0040) —
are backfilled as deliberate at initialisation so the record is consistent from day one.

## 9. Error handling

Following the project rule that errors must be loud rather than papered over:

- **Unreadable or corrupt FITS** — recorded with the failure, never silently skipped.
- **Solve failure** — recorded as `solve_source = none` with a reason and an attempt
  counter, so genuinely unsolvable frames (the 405 solar-system ones) are not retried
  forever, and are not mistaken for "not yet processed".
- **NAS or ZFS unreachable mid-scan** — the pass is resumable; partial state is valid
  because every pass is idempotent on content hash.
- **Ambiguous identity** — surfaced for confirmation. Never guessed, never defaulted.
- **Count discrepancy** — the workstation and scott-server currently report 28,135 vs
  28,123 frames for the same archive. The inventory pass resolves this exactly by content
  hash and reports the difference rather than picking a number.

## 10. Testing

Test-driven, per the project workflow. Fixtures are small synthetic FITS files with known
headers, plus a handful of real frames.

- Blind solve of the known IC 1848 frame returns its known coordinates.
- The Moon frame is classified unsolvable and does **not** hang or crash the pass.
- Two different files sharing one filename both survive inventory as distinct rows.
- A path containing a space (`NGC 7635`) round-trips through every pass — this is a live
  bug class in existing scripts, where word-splitting made a failed comparison look
  identical to a passing one.
- Scanning twice produces no duplicate rows (idempotency).
- A frame removed between scans transitions to `missing`, and is never dropped.
- Manifest export and re-import reconstructs labels with the DB deleted.

## 11. Non-goals

- No file moves, renames, or deletions in Phase 1.
- No FITS header modification, ever.
- No machine deletion, ever.
- Not a replacement for AstroIndexer, which is third-party and keeps its own index.
- No web UI. CLI and queries only.
- No morphological/CNN object recognition. Star geometry via plate solving is exact where
  morphology is fuzzy, and survives changes of filter, stretch and mosaic panel.

## 12. Phase 2 (out of scope, recorded for context)

Physical reorganisation into `by-object/<canonical>/<filter>/<project>/<panel>/`, plus
parallel `calibration/<camera>/<type>/<exposure>/` and `derived/` trees. Gated on the store
having earned trust, performed per object rather than wholesale, and executed as
copy-verify-then-remove — never a bare `mv`. Only `solved`, `propagated` or `manual`
identities are eligible. This gets its own spec.
