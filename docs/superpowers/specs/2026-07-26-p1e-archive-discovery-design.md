# P1e — Archive Discovery Design

**Date:** 2026-07-26
**Status:** approved, ready for implementation planning
**Scope:** closes the Phase 1 remainder — the §5.2 "not-found path" — so a fingerprint-DB
miss can acquire a professional reference unattended.

---

## 1. Problem

Design §5.2 assumes the not-found path can "query archives (MAST, ESASky, ESO) for a public
release image covering the position." **That path does not exist as written.** Verified
2026-07-11 and re-verified 2026-07-26:

- MAST / `astroquery.esa.hubble` cone-search returns **science observations** (linear FITS),
  not press-release renders.
- ESASky `list_maps()` likewise exposes science missions.
- The ESA/Hubble and ESO public galleries — where the professional *renders* actually live —
  expose no JSON API. `?format=json` returns `text/html`; `/api/v1/` returns 404.
- The ESA/Hubble advanced-search form has **no RA/Dec fields at all**. Its searchable fields
  are `subject_name`, `title`, `description`, `credit`, `facility`, `instrument`, `category`,
  `fov`, `type`, `id`, `release_id`, `minimum_size`, `colours`, `ranking`, and publication
  dates. ESO's form is identical plus a free-text `q`.

Ingesting a linear science FITS as `source_type=professional_render` would be a §2.1
violation: a linear master has no presentation layer, fingerprints as maximally flat, and
would poison the reference set with an **anti-target**, driving user images toward flatness.
That is the fabrication failure mode the guardrails exist to prevent. It is not an option.

So the missing capability is purely **discovery by sky position**.

## 2. Decisions taken

| Decision | Choice | Rationale |
|---|---|---|
| Phase 1 exit criterion's "unattended" | **Kept literally** — the miss path must auto-fetch with no human in the loop | Rules out curated-catalog-only |
| Approach | **Local positional index** | Only option where discovery is a genuine cone search (§5.1) |
| Provenance of auto-discovered records | **`professional_render` immediately**, consensus-eligible | Position gate is verified; simplest, and §5.4 consensus gets data fastest |
| Gallery scope | **ESA/Hubble + ESO** | Both proven; identical schema, one adapter; both CC BY 4.0 (§5.5); robots-permissible |

### Why not search by name

A SIMBAD `position → name → subject_name search → verify` hybrid was considered and
rejected. §5.1 is explicit: key on cone search, **never** target name, because "catalog
naming is a swamp." Reconnaissance confirmed the swamp concretely — for the same object,
ESA/Hubble publishes `Name: Messier 42`, ESO publishes `Name: M 42`, and the search query
that actually returns results is `Orion Nebula`. Every such mismatch is a silent false miss.
It also fails for fields whose render is framed or named differently than the catalog object
(mosaic panels, off-center framings) even when a covering render exists.

A lazy variant (name-hint on cold regions, cached into the index) was also rejected:
coverage would depend on query history, degrading "unattended" from a guarantee to a
probability, and making the same input yield different results depending on cache state.

### Consequence of immediate consensus eligibility

With no quarantine stage, the position-verification gate (§6) is the **only** barrier
between a parser regression and a poisoned consensus pool. The gate is therefore treated as
safety-critical: fail-closed, and tested per-rule rather than only end-to-end.

## 3. What the galleries actually publish

Both galleries' detail pages publish, as text, everything the ingest pipeline needs.
Verified against the two references already in the curated catalog:

| Field | `heic0601a` (ESA/Hubble) | `eso1103a` (ESO) | Matches hand-built catalog |
|---|---|---|---|
| `Position (RA)` | `5 35 9.73` → 83.7905° | `5 35 17.32` → 83.82217° | exact |
| `Position (Dec)` | `-5° 24' 50.32"` → −5.4140° | `-5° 23' 27.55"` → −5.39099° | exact |
| `Field of view` | `30.03 × 30.03 arcmin` → r = 21.24′ | `35.49 × 34.10 arcmin` → r = 24.61′ | 21.23′ / 24.61′ |
| Colours & filters | B 435, V 555, Hα 658, I 775, z 850 nm | U 340, B 451, V 539, R 651, Hα 658 nm | both → RGB |
| CDN image | `cdn.esahubble.org/archives/images/large/<id>.jpg` | `cdn.eso.org/images/large/<id>.jpg` | — |

Two consequences worth stating:

1. **The published position is AVM-derived.** It matches the catalog values (which came from
   the embedded AVM tag) to four decimals. So a cheap HTML fetch yields a position
   trustworthy enough to filter the cone *before* downloading a 324-megapixel JPEG. Verify
   first, fetch only survivors.
2. **It rescues the AVM-unparseable case.** `eso1103a`'s AVM tag is rejected by pyavm (a
   multi-valued `Spatial.Notes`), and force-parsing yields corrupt coordinates
   (81.97, −3.74). Last session that was resolved by hand-reading ESO's page. Reading the
   same published metadata automatically gives a legitimate path for such renders, rather
   than requiring manual annotation or — never — force-parsing.

### Verified access details

- Results are server-rendered, but at **different paths per gallery**: ESA/Hubble at
  `/images/archive/search/page/N/?…`, ESO at `/public/images/archive/search/?…`. A bare
  search with no criteria renders empty on both; at least one criterion is required.
- `published_since_{year,month,day}` works and returns a result count — this is both the
  "match everything" trick (`published_since_year=1990`) and the incremental-sync mechanism.
- ESA/Hubble holds **5513** images total at roughly 50 per page → about 111 listing requests
  for a full enumeration; the deep-sky category restriction below cuts this substantially.
- `category` values are numeric IDs, not slugs.
- ESO's `robots.txt` does not disallow `/public/images/archive/search/`; esahubble.org
  serves no `robots.txt`. The path is permissible.

## 4. Architecture

New subpackage `src/autocontrast/db/discover/`, matching the `:mod:`.discover`` seam that
`seed.py`'s docstring already forward-references. Four modules, split on a pure/impure
boundary so that all parsing is testable without network access:

| Module | Purpose | Network |
|---|---|---|
| `gallery.py` | Djangoplicity adapter: build search/detail URLs; parse listing → ids; parse detail → `GalleryEntry`. Pure functions over HTML strings. | no |
| `index.py` | `GalleryIndex` — SQLite position index; upsert + local cone search | no |
| `crawl.py` | Polite HTTP client; crawl and incremental sync orchestration | yes |
| `discover.py` | Miss-path orchestration: cone search → rank → fetch → verify → ingest | via injected fetcher |

Gallery differences (base URL, results path, license and attribution defaults) live in two
config records, not two code paths.

**Targeted refactor.** `store.py` holds a private `_separation_arcmin()`; `index.py` needs
identical spherical-distance math. Extract it to `db/skymath.py` and have both import it,
rather than duplicating a formula in two places that could silently diverge.

## 5. The index

A **separate SQLite file** (`data/gallery_index.sqlite`), not a table inside
`fingerprints.sqlite`. It is a regenerable cache derived from external sites: keeping it
separate means "delete the cache and re-crawl" can never endanger user fingerprints, and the
two version on different cadences.

```sql
CREATE TABLE gallery_index (
  id                TEXT NOT NULL,      -- 'heic0601a'
  gallery           TEXT NOT NULL,      -- 'esa_hubble' | 'eso'
  detail_url        TEXT NOT NULL,
  image_url         TEXT NOT NULL,      -- CDN 'large' JPEG
  ra_deg            REAL,               -- NULL when unpublished
  dec_deg           REAL,
  fov_w_arcmin      REAL,
  fov_h_arcmin      REAL,
  fov_radius_arcmin REAL,               -- half-diagonal, derived
  width_px          INTEGER,            -- published in the listing (see amendment)
  height_px         INTEGER,
  pixel_scale_arcsec REAL,              -- fov_w_arcmin * 60 / width_px
  object_name       TEXT,
  category          TEXT,
  entry_type        TEXT,               -- 'Observation' | 'Photographic' | 'Artwork'
  palette_class     TEXT NOT NULL,      -- derived from filters, else 'unknown'
  license           TEXT,               -- NULL when not establishable (§7 G5)
  attribution       TEXT,
  published_utc     TEXT,
  parsed_ok         INTEGER NOT NULL,   -- absent data vs. broken parsing
  PRIMARY KEY (gallery, id)
);
CREATE INDEX ix_gallery_dec ON gallery_index(dec_deg);
```

**Amendment (2026-07-26, during implementation planning).** Live verification showed the
listing pages are not rendered markup but an inline
`var images = [ {id, title, width, height, url}, … ];` JavaScript data literal. Two
consequences, both improvements on this spec as first written:

1. Parsing that literal is more drift-resistant than scraping markup — it is
   machine-oriented and survives visual redesigns.
2. **Pixel dimensions are published**, so `pixel_scale_arcsec` is derivable from metadata
   alone: verified 0.1001″/px for `heic0601a` (30.03′ × 60 / 18000 px) and 0.238″/px for
   `eso1103a` (35.49′ × 60 / 8948 px), both matching the curated catalog. Gate G4 is
   therefore satisfiable **without downloading the image**; this spec originally assumed
   dimensions came from the downloaded file.

A third amendment: sync is driven by a `sync_state (gallery, last_synced_utc)` table rather
than by the maximum indexed `published_utc`. That avoids depending on release-date parsing,
and the current time is injected so the window is testable. `published_utc` is still captured.

Entries whose position is unpublished are stored with `ra_deg IS NULL` — so the crawler
remembers not to re-fetch them — but are never returned by cone search. `opo0205c` (ESA/Hubble
publishes neither coordinates nor field of view for it) lands here naturally, reproducing the
skip decision made by hand in the curated catalog.

`fov_radius_arcmin` is the half-diagonal `√(w² + h²) / 2`, matching the existing
`WcsResult.fov_radius_arcmin` convention. Verified: 30.03 × 30.03 → 21.24′ against the
catalog's 21.23′; 35.49 × 34.10 → 24.61′ exactly.

`palette_class` is derived from the published Colours & filters table by wavelength/band, not
guessed. An absent or unrecognized table yields `'unknown'`, consistent with
`palette_class_from_filters` (§4.3) — guessing `RGB` would let a mismatched reference push
channel ratios (§2.1).

### Crawl and sync

Paginate the listing under a wide `published_since` filter, restricted to the deep-sky
categories (Nebulae, Galaxies, Star Clusters, Stars, Cosmology, Quasars & Black Holes);
Solar System, spacecraft, and illustration entries carry no useful deep-sky position.

- **Resumable:** already-indexed `(gallery, id)` pairs are skipped unless explicitly refreshing.
- **Sync:** re-run with `published_since` set to the maximum indexed `published_utc` minus a
  **7-day** safety window, covering late edits and any publication-date backdating.
- **Politeness:** a single shared client — 1 req/s with jitter, descriptive User-Agent
  carrying a contact address, exponential backoff on 5xx, honors `429 Retry-After`, and a
  per-run request cap of **8000** (enough for a full cold crawl of both galleries with
  headroom; exceeding it raises).
- **Loud failure on site drift:** if **more than 50%** of detail pages parsed from a single
  listing page fail to yield a position *and* fail to yield a "no position published"
  determination — that is, the parse itself broke rather than the data being absent — the
  crawl **aborts with an error** rather than indexing partial rows. A Djangoplicity redesign
  must be a noisy failure, not a quietly-empty index (§12). Legitimately position-free
  entries (illustrations) do not count toward the threshold.

## 6. The miss path

```python
def acquire_reference(store, index, *, ra_deg, dec_deg, search_radius_arcmin,
                      palette_class, fetcher, cache_dir, blind_solver=None,
                      top_k=1) -> DiscoveryOutcome
```

This call is the Phase 1 exit criterion expressed as one function: look up → on miss
discover → fetch → fingerprint → store. `discover_reference()` beneath it performs discovery
alone, so the two are separately testable. The fetcher is injected, so tests never touch the
network.

`palette_class` here is the **user image's** palette (as derived by `palette_class_from_filters`
from its FITS `FILTER` keyword), used for §2.3 compatibility ranking against each candidate's
own indexed `palette_class`. It is never written to a discovered record — a discovered
record's palette always comes from that render's own published filters.

1. **Local cone search** over the index — footprint overlap
   (`separation ≤ query_radius + entry_fov_radius`), NULL positions excluded. Offline,
   instant, and no names anywhere in the matching path.
2. **Rank candidates deterministically.** Palette-compatible entries first — §2.3 flags
   rather than filters, so incompatible entries remain as lower-ranked fallbacks. Then
   framing aptness `|log(entry_fov_radius / query_radius)|`, then how centered the query sits
   within the reference, then `id` as a stable tie-break.
3. **Fetch** the top candidate's CDN `large` JPEG into `cache_dir`, skipping if already
   cached, under a hard **256 MB** cap that fails loudly rather than streaming indefinitely.
   (The Hubble Orion mosaic — 18000 × 18000 px — is the largest known case and sits well
   inside this.)
4. **Verify** (§7). On rejection, fall through to the next candidate.
5. **Ingest** through the existing `ingest_reference_auto()` — no new ingest path — as
   `professional_render`, with `{source_url, license, attribution, wcs_source}` provenance.
6. **Return** an outcome listing every candidate considered *and the reason each was
   rejected*, so degraded paths surface (§12).

`top_k` defaults to 1, matching §5.4: "seed each `(position_cell, palette_class)` with one
best professional reference," with ensemble robustness arriving later as references
accumulate.

The ranking policy in step 2 is the one genuinely judgment-laden piece — "which professional
render is the best style target for this field" is domain knowledge, not mechanics. The
ordering above is a starting proposal; `rank_candidates()` is to be scaffolded with signature
and comments for the policy body to be authored deliberately.

## 7. Verification gate

Fail-closed. Any check failing rejects the candidate and advances to the next.

| Gate | Check | On failure |
|---|---|---|
| G1 | Indexed position overlaps the query cone | not a candidate |
| G2 | Independent WCS cross-check (below) | reject |
| G3 | *Solved* position still within the query cone — the solve is authoritative | reject |
| G4 | Pixel scale available (§2.2 forbids pixel-space comparison) | reject |
| G5 | License establishable **and** attribution non-empty (§5.5) | reject |
| G6 | Palette derived from published filters, else `'unknown'` — never guessed | — |

**G2.** Run `acquire_wcs(path, manual=published_position)`:

- `wcs_source == 'avm'` → require agreement with the indexed published position within
  `max(0.25 × fov_radius_arcmin, 1.0)` arcmin. On disagreement, **reject** — do not prefer
  either source. Two independent sources disagreeing means one is wrong, and which one is
  unknowable from here.
- `wcs_source == 'manual'` (AVM absent or unparseable, so `acquire_wcs` fell back to the
  published annotation) → accept, but record `wcs_source='manual'` so the stored record shows
  the position was never independently cross-checked.
- `unsolved` → reject.

G2 has a concrete regression target: force-parsing `eso1103a`'s AVM yields (81.97, −3.74)
against its published (83.82, −5.39) — 2.3° apart, roughly 5× tolerance. The gate must reject
exactly the corruption already documented for that file.

**G4 on the manual path.** Derive scale as `fov_w_arcmin × 60 / width_px` — the same quantity
computed by hand for `eso1103a` (0.238″/px). Per the amendment in §5, `width_px` comes from
the listing's published dimensions, so no download is required. `ingest_reference_auto`
already refuses to proceed without a scale.

**G5 amendment (2026-07-26) — the per-gallery license default is not universal.** ESA/Hubble
hosts third-party **copyrighted** renders: `opo0205c`'s credit reads *"Copyright © Anglo-
Australian Observatory. Photograph by David Malin"*. And neither site publishes a per-image
machine-readable license — the page's `copyright` container is site boilerplate, and no CC BY
string appears anywhere in the markup. The only signal is the credit text itself.

So §5.5's table ("ESA/Hubble → CC BY 4.0") describes the *gallery's own* output, not
everything it hosts. The rule is therefore:

- Credit does **not** assert copyright → apply the gallery default license.
- Credit **does** assert copyright (matches `copyright`, `©`, `(c)`, `all rights reserved`)
  → leave the license **unestablished** (`NULL`) and **reject** the candidate at G5.

Attaching a false CC BY 4.0 to a record would be a §5.5 violation that travels with the
fingerprint permanently and surfaces in any UI displaying the reference. Erring toward
under-ingesting is the correct direction for a licensing question.

`source_type=tool_output` cannot arise on this path, since only fetched gallery renders are
ingested; `store.py`'s §5.3 enforcement remains the backstop regardless.

### What raises versus what reports

An unusable candidate never raises — it becomes a reason string in `DiscoveryOutcome`, per
§12's requirement that every degraded path logs and surfaces. Genuine faults do raise
loudly: network budget exhausted, database error, or the crawl's parse-failure threshold
breached.

## 8. Testing

Test-driven, per project norm. The lesson from commit `12e7041` is applied directly: the bug
class that slipped past 138 passing tests was a **fixture-diversity** gap, not a coverage
gap — every fixture had been 2D mono FITS. So the awkward cases are fixtures from the start.

Checked-in HTML fixtures under `tests/fixtures/gallery/`, captured from live pages, let all
parser tests run with zero network:

| Fixture | Why it exists |
|---|---|
| `hubble_listing_orion.html` | 35 results via the `/page/1/` path |
| `hubble_detail_heic0601a.html` | position, FoV, and filters all present — happy path |
| `eso_detail_eso1103a.html` | published position with **unparseable AVM** |
| `hubble_detail_opo0205c.html` | **no** published position or FoV → NULL row, must be skipped |
| `eso_listing_orion.html` | ESO's non-paginated results path |
| `hubble_detail_heic0211i.html` | `Type: Artwork` — artist's impression, no sky position at all |

Beyond parsing:

- **Index tests** on synthetic rows: just-touching footprints, NULL exclusion, both galleries
  co-resident, and RA-wrap near 0°/360° — `ix_gallery_dec` prefilters declination only, so
  wrap needs explicit coverage.
- **Gate tests** for each of G1–G6 rejecting; the `eso1103a` corrupt-AVM regression; and an
  agreeing-AVM acceptance.
- **Crawl tests** for pagination, resume-skips-indexed, the `published_since` sync window, and
  the drift threshold aborting.
- **Full miss-path test** with an injected fixture-serving fetcher over a temp store and temp
  index, asserting the stored record carries license, attribution, and `wcs_source` — the
  Phase 1 exit criterion as an automated test.
- **One opt-in live test**, marked and deselected by default (`-m live`), hitting the two real
  detail pages and asserting position and FoV still parse. Detects upstream drift on demand
  without making the suite network-dependent or flaky.

## 9. Design-document amendments implied

§5.2 step 1 ("Query archives (MAST, ESASky, ESO) for a public release image covering the
position") is factually wrong and should be rewritten to describe the local positional index.
The rest of §5.2 stands unchanged — in particular step 3's expectation that a meaningful
fraction of the best renders fail all WCS tiers, which the `opo0205c` NULL-row case confirms.

## 10. Out of scope

- Additional galleries (NASA/Webb, NOIRLab). Both are Djangoplicity, so each is a config
  entry plus a robots check and license mapping — deferred, not designed against.
- §5.4 consensus computation. Phase 4.
- Any use of MAST/ESASky science observations as renders. Permanently excluded (§2.1).
