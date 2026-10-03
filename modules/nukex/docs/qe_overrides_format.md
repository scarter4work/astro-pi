# QE override file

NukeX ships a quantum-efficiency database, built into the module from
`share/qe_database.json` (55 cameras, 97 filters; a newer copy downloaded by the
camera-database updater, `~/.config/nukex4/qe_database.json`, takes precedence). To add a camera or filter it does not know, or to
replace shipped values with your own measurements, write a JSON file with
the same shape and select it with **QE override file… → Browse** in the
NukeX interface. Leave the field empty to use the shipped database only.

## Schema

```json
{
  "schema_version": 1,
  "cameras": {
    "<camera-key>": {
      "sensor": "IMX585",
      "type": "OSC",
      "bayer": "RGGB",
      "qe": {
        "501": { "R": 0.03, "G": 0.85, "B": 0.50 },
        "656": { "R": 0.73, "G": 0.32, "B": 0.03 }
      },
      "confidence": "high"
    }
  },
  "filters": {
    "<filter-key>": {
      "type": "DUAL_NB",
      "lines": [
        { "name": "Ha",   "wavelength_nm": 656.3, "fwhm_nm": 7.0 },
        { "name": "OIII", "wavelength_nm": 500.7, "fwhm_nm": 7.0 }
      ]
    }
  }
}
```

- `qe` keys are wavelengths in nm; values are QE fractions 0–1 per photosite.
  OSC cameras use `R`, `G`, `B`; mono cameras use `mono_pk`.
- `confidence` is `high`, `medium` or `low`. It is informational.
- `type` on filters is informational (`DUAL_NB`, `NARROWBAND`, `BROADBAND`).

## How keys are matched

**Cameras** are matched against the FITS `INSTRUME` keyword after normalising
both to lowercase alphanumerics, and a database key may be a substring of the
header value. `INSTRUME = 'ZWO ASI2400MC Pro'` matches the shipped key
`asi2400mc`. Use the model name as the key (`asi2400mc`, `qhy268c`), not the
full header string. When no key matches, NukeX uses `generic_sony_imx_osc`,
prints a warning in the Process Console, and writes
`NUKEX_QE_CONFIDENCE = 'generic-fallback'` on the composed image.

**Filters** are matched by the *canonical* name the classifier derives from
the FITS `FILTER` keyword, not by the raw header text. Recognised spellings
(case and punctuation ignored): `HaO3`/`HaOIII` → `HaO3`; `S2O3`/`SIIOIII` →
`S2O3`; `L-eXtreme`, `L-eNhance`, `L-Ultimate`, `ALP-T`; `Ha`/`Halpha`,
`OIII`/`O3`, `SII`/`S2`; `L`/`Luminance`/`L-Pro`/`LPS`/`UV-IR-cut`/`CLS`
(broadband); `R`, `G`, `B`; `HaO3S2`/`HaOIIISII`, and the L-Quad Enhance and
L-Synergy spellings, for a filter passing all three of Ha, OIII and SII.

A dual-narrowband filter with any other name on a Bayer camera stops the batch
at start, because guessing would mis-colour the result silently. **NukeX offers
to learn it:** the dialog asks which emission lines the filter passes and
records the answer in `<user-data>/nukex4/filter_aliases.json`, then re-runs
the stack. That file is plain JSON you can read, edit or delete:

```json
{ "schema_version": 1, "aliases": { "lqef": "HaO3S2" } }
```

Keys are the header name reduced to lowercase alphanumerics, so `L-QEF`,
`l qef` and `Lqef` are one entry. Values must be a canonical name the database
carries. The file is consulted *after* the shipped table, so an entry can add a
spelling but never redefine what `Ha` or `HaO3` mean.

H-beta is never offered as a choice. At 486 nm it falls on the same blue and
green photosites as OIII at 501 nm, so a Q matrix carrying both columns is
rank-deficient and the decomposition has no unique answer; a quad-band filter
is described by its other three lines. For the same reason Ha and SII without
OIII has no calibrated entry, and the dialog refuses that combination rather
than accepting it and failing later.

Mono frames with an unknown `FILTER` are treated as luminance with a warning.

## Override semantics

- `cameras` and `filters` merge with the shipped database.
- An entry whose key collides with a shipped key **replaces the whole entry**.
- New keys are added. `"cameras": {}` / `"filters": {}` is a valid no-op.

## Errors

Malformed JSON stops the batch with the parser's line and column. A camera
that exists but lacks QE at a needed wavelength yields a singular Q matrix
and stops Phase B with the camera and filter named.
