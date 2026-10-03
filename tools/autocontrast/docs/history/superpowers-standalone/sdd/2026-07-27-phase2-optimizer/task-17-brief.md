### Task 17: PJSR release compliance for the AutoContrast scripts

**Why this exists:** AutoContrast ships PJSR scripts, and the user's global workflow
rules require every PJSR script to be versioned, signed, packaged, and installable.
Currently **none of the four** is true, and Task 13 is about to add a second
non-compliant script (`autocontrast_optimize.js`), compounding the defect.

Verified state on disk (2026-07-28):
- `pixinsight/autocontrast_analyze.js` — has `#feature-id`, **no `#define VERSION`**
- `pixinsight/sidecar_bridge_test.js` — has `#feature-id`, no version (a dev harness)
- `pixinsight/solve_spike.js` — a spike; its `#define VERSION "6.4.2"` is *ImageSolver's*,
  not ours. Do not mistake it for our versioning.
- **No `repository/` directory.** No `updates.xri`. No packages.
- `tools/` holds only `capture_gallery_fixtures.sh` — no `sign.sh`, no
  `build-packages.sh`, no `install-local.sh`.

---

## Scope

Bring the AutoContrast PJSR scripts into compliance with the project's established
PJSR release workflow. The reference implementation to follow is the EZ-Stretch-BSC
suite at `~/projects/EZ-suite-bsc/EZ-Stretch-BSC/` — read its `tools/` scripts and
`updates.xri` and follow those conventions rather than inventing new ones.

Deliverables:

1. **Version defines.** Add `#define VERSION` to the shipping scripts
   (`autocontrast_analyze.js`, and `autocontrast_optimize.js` if Task 13 has landed
   by the time you run — check; if it has not, leave a note in your report rather
   than creating the file).
   Decide and state the starting version. `sidecar_bridge_test.js` and
   `solve_spike.js` are development harnesses, not shipping scripts — say explicitly
   in your report whether you are shipping them, and prefer NOT to.
2. **`tools/sign.sh`** — signs the scripts using the developer keys.
3. **`tools/build-packages.sh`** — builds the distributable package(s) and computes
   the SHA1s.
4. **`repository/updates.xri`** — the signed XML manifest, with SHA1s matching the
   actual package files and a platform version range covering current PI.
5. **`tools/install-local.sh`** — PJSR scripts DO get a local install step (this is
   the documented difference from C++ modules, which must NOT be installed locally).

## Credentials and paths — do NOT hardcode secrets into the repo

- Signing keys: `/home/scarter4work/projects/keys/scarter4work_keys.xssk`
- Developer ID: `scarter4work`
- The signing password and the PixInsight signing command form are in the user's
  global `CLAUDE.md` — read it rather than asking. Follow the existing convention of
  keeping the password OUT of committed files (EZ-Stretch-BSC uses a password file
  outside the repo).
- **Anything you commit must contain no secret.** If a tool needs the password, it
  reads it from a path, never a literal. Check your diff for leaked credentials
  before committing — a secret in git history is not fixed by removing it from HEAD.

## Hard limits

- **Do NOT push.** Committing is in scope; pushing is not. The user's workflow
  forbids pushing without the full release sequence complete, and Phase 2 is not done.
- **Do NOT run any install that would create a version conflict with a user's
  working PixInsight.** `install-local.sh` should exist and be correct; state clearly
  in your report whether you executed it and what it touched.
- Do not modify any Python under `src/autocontrast/`. This task is packaging only.
- Do not modify the optimizer, the loop, or any test.
- Branch `phase2-optimizer`. Do not commit to master.

## Verification

- Show the signing command actually succeeding on at least one script, with real
  output — a `.xsgn` produced.
- Show the SHA1 in `updates.xri` matching the real package file (compute both, paste
  both).
- Show `updates.xri` carries a `<Signature>` block after XML signing.
- Confirm `.venv/bin/python -m pytest -q` still passes unchanged — this task should
  not affect it at all, so any change in the count is a finding.

If PixInsight is unavailable or signing fails, **report that plainly and stop** —
do not fake a signature, stub a SHA1, or commit an unsigned manifest as if it were
signed. An honest BLOCKED is the correct outcome; a manifest that looks signed and
is not would be worse than no manifest.

## Report

Write to `.superpowers/sdd/2026-07-27-phase2-optimizer/task-17-report.md` AS YOU GO.
Reply with only: status, commit SHA(s), what was signed, and concerns.
