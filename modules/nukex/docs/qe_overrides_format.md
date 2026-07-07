# QE Override File Format

NukeX ships a quantum-efficiency database (`share/qe_database.json`, 55 cameras /
87 filters as of this writing) that Phase B color science uses to build the
camera-response matrix for each (camera, filter) pair. If your camera isn't in
that list, you measured your own filter passband, or you just want to correct a
shipped value, you supply a second JSON file — the **QE override** — in the same
schema.

## How it's loaded

- There is **no baked-in default override path**. `qeOverridePath` on the
  NukeX process instance starts empty, which means "no override." You set it
  with the **QE override file picker** in the NukeX interface (`Browse…` next
  to the QE Override field; `Clear` resets it to none).
- `~/.nukex4/qe_overrides.json` is only a **suggested convention** — it shows
  up in the tool's own error-message hints (e.g. "Add it to
  `~/.nukex4/qe_overrides.json`") — but you can point the picker at any path.
- When a path is set, NukeX loads `share/qe_database.json` first, then loads
  your override file **on top of it**, in that order
  (`StackingEngine` constructor, `qe_database.cpp`). Both loads happen before
  the stack executes; a load failure is deferred and reported loudly the
  moment you run the process, not silently ignored.

## Schema

Identical schema to the shipped `share/qe_database.json`:

```json
{
  "schema_version": 1,
  "cameras": {
    "<camera-key>": {
      "sensor": "<sensor-model>",
      "type": "OSC | mono",
      "bayer": "RGGB | BGGR | GRBG | GBRG",
      "qe": {
        "<wavelength_nm>": { "R": 0.0, "G": 0.0, "B": 0.0, "mono_pk": 0.0 }
      },
      "confidence": "high | medium | low"
    }
  },
  "filters": {
    "<filter-key>": {
      "type": "<free-form label, e.g. dual-narrowband>",
      "lines": [
        { "name": "Ha", "wavelength_nm": 656.3, "fwhm_nm": 7.0 }
      ]
    }
  }
}
```

Notes on fields, confirmed against the loader (`src/lib/calibration/src/qe_database.cpp`):

- `schema_version` is **not read or enforced** by the loader. Keep it at `1`
  for consistency with the shipped file, but the parser never checks it.
- `qe` is keyed by integer wavelength in nanometers (as a JSON string key,
  e.g. `"656"`). Each wavelength maps to per-photosite QE values.
- Recognized photosite keys are `R`, `G`, `B`, `Gr`, `Gb` (`Gr`/`Gb` both fold
  into `G`). **Any other key** — including the shipped file's `mono_pk` —
  falls through to a single catch-all "mono peak" bucket used for mono
  sensors and as an interpolation fallback. This isn't validated: a typo
  like `"r"` (lowercase) silently lands in that same catch-all bucket instead
  of `R`, with no error.
- `confidence` accepts `high` / `medium` / `low`; anything else (including a
  missing field) silently becomes `unknown` — no rejection.
- `type` (camera and filter) is a **free-form string**, not an enforced enum.
  The shipped database itself uses `"OSC"` / `"mono"` for cameras and a mix
  of values for filters (`"dual-narrowband"`, `"tri-narrowband"`,
  `"quad-narrowband"`, `"narrowband-single"`, `"narrowband-multi"`,
  `"broadband-RGB"`, `"broadband-LPR"`, `"luminance"`). Pick whatever's
  accurate; the loader doesn't check it against a fixed list.
- Camera and filter keys are matched by **exact, case-sensitive string
  equality**. The shipped database uses lowercase keys (e.g. `asi2600mc`).
  Check `share/qe_database.json` for the exact spelling of your camera
  before writing an override key, or your override will silently add a new
  camera alongside the shipped one instead of replacing it.

## Override semantics — REPLACE, not merge (read this before editing)

This is the part that will bite you if you skim it.

The loader merges `cameras{}` and `filters{}` as **dictionaries keyed by
name**: an override entry whose key doesn't exist in the shipped DB is
simply added. But on a **key collision** (same camera name, or same filter
name), the override entry does not get merged field-by-field into the
shipped one — the loader builds a **brand-new, empty record** and populates
only the fields present in your override JSON for that key, then replaces
the shipped record with it wholesale:

```cpp
// qe_database.cpp, parse_and_merge()
CameraQE cam;                    // fresh, all fields default/empty
if (cam_json.contains("sensor")) cam.sensor = ...
if (cam_json.contains("qe")) ...  // only wavelengths present in *your* JSON
...
cameras_[name] = std::move(cam);  // shipped record for `name` is gone
```

The same applies to `filters{}`.

**Consequence: any field you omit is not "left as shipped" — it's reset to
empty/default.** If your override for a shipped camera specifies only one
wavelength's QE, you have just deleted every other wavelength's data for
that camera, along with `sensor`, `bayer`, and `confidence` if you didn't
repeat them too. There is no partial/deep merge at any level — not per
wavelength, not per photosite, not per field. The replace granularity is
the top-level map key (camera name or filter name) *only*.

**To override a single value on a shipped camera or filter, copy that
camera's or filter's complete record out of `share/qe_database.json` into
your override file, then edit only the value(s) you want to change.**
Anything you don't copy over is lost for that entry, not inherited.

This is demonstrated by the repo's own test fixture. The shipped
`ASI585MC` record (`test/fixtures/qe/minimal_db.json`) has QE at four
wavelengths (486, 501, 656, 672 nm). The test override
(`test/fixtures/qe/override.json`) only specifies 656 nm:

```json
{
  "schema_version": 1,
  "cameras": {
    "ASI585MC": {
      "sensor": "IMX585",
      "type": "OSC",
      "bayer": "RGGB",
      "qe": {
        "656": { "R": 0.99, "G": 0.99, "B": 0.99 }
      },
      "confidence": "high"
    },
    "Custom_Cam_1": {
      "sensor": "Custom",
      "type": "OSC",
      "bayer": "RGGB",
      "qe": {
        "656": { "R": 0.50, "G": 0.10, "B": 0.05 }
      },
      "confidence": "low"
    }
  }
}
```

After this loads, `ASI585MC` has QE data at 656 nm **only** — the 486/501/672 nm
points from the shipped file are gone, so any lookup at those wavelengths now
falls back to the single remaining 656 nm value (nearest-point fallback) instead
of the real shipped curve. `Custom_Cam_1` is a genuinely new camera and is
simply added.

## Validation / failure behavior

Confirmed from `parse_and_merge()`:

- **Malformed JSON** (syntax error) fails loud and cleanly: the load result
  reports `ok = false` with a message like `QE override is malformed at line
  N col M (parser: ...)`. This surfaces in the Process Console as `** QE
  database error: ...`.
- **Missing file** fails loud: `QE override missing or unreadable: <path>`.
- **Wrong JSON types for a field** (e.g. a `qe` wavelength key that isn't
  numeric, or `"sensor": 123` instead of a string) are **not** caught by the
  try/catch in `parse_and_merge` — that block only catches JSON *syntax*
  errors, not schema/type mismatches. A type mismatch throws an uncaught
  C++ exception instead of producing the friendly line/col message. Stick to
  the schema's types exactly (numeric-string wavelength keys, string values
  for `sensor`/`type`/`bayer`/`confidence`, numeric QE values).
- Unrecognized `confidence` values and unrecognized photosite keys are
  **not rejected** — see above, they silently fall back to `unknown` /
  the mono catch-all bucket instead of erroring.

## Worked example

Goal: add a camera missing from the shipped DB, and correct the confidence
rating on a shipped camera without losing its data.

```json
{
  "schema_version": 1,
  "cameras": {
    "my_custom_cmos": {
      "sensor": "IMX294",
      "type": "OSC",
      "bayer": "RGGB",
      "qe": {
        "486": { "R": 0.10, "G": 0.72, "B": 0.80 },
        "501": { "R": 0.10, "G": 0.88, "B": 0.55 },
        "656": { "R": 0.78, "G": 0.30, "B": 0.05 },
        "672": { "R": 0.75, "G": 0.31, "B": 0.06 }
      },
      "confidence": "low"
    },
    "asi2600mc": {
      "sensor": "IMX571",
      "type": "OSC",
      "bayer": "RGGB",
      "qe": {
        "486": { "R": 0.07, "G": 0.78, "B": 0.86, "mono_pk": 0.89 },
        "501": { "R": 0.08, "G": 0.89, "B": 0.60, "mono_pk": 0.90 },
        "656": { "R": 0.46, "G": 0.05, "B": 0.04, "mono_pk": 0.50 },
        "672": { "R": 0.42, "G": 0.05, "B": 0.05, "mono_pk": 0.46 }
      },
      "confidence": "high"
    }
  },
  "filters": {}
}
```

- `my_custom_cmos` is a brand-new key: it's simply added.
- `asi2600mc` collides with the shipped entry, so the **entire record above
  replaces the shipped one**. Every field — including all four wavelength
  points — was copied from `share/qe_database.json` first; only
  `confidence` was actually changed (`medium` → `high`). Had any wavelength
  been left out here, that wavelength's shipped QE data would be gone for
  this camera.

Save this as, e.g., `~/.nukex4/qe_overrides.json`, then point the QE
override field (via the file picker in the NukeX interface) at it.
