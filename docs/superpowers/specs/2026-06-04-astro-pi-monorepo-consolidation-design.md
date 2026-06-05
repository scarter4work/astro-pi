# astro-pi — Monorepo Consolidation & Unified PixInsight Distribution

**Date:** 2026-06-04
**Status:** Approved design — pending spec review, then implementation plan
**Author:** scarter4work (with Claude)

## Goal

Consolidate all PixInsight astrophotography development into a single repository
(`astro-pi`) that publishes **one** PixInsight repository URL serving every
shippable package. Today these are scattered across many independent GitHub
repos (five NukeX versions, the EZ-Stretch-BSC scripts, Node-based dev tooling),
each with its own `updates.xri` and distribution URL. Users must register
several repository URLs. The consolidated repo gives them one.

The primary, explicitly chosen goal is **unified PixInsight distribution**. Source
consolidation ("scripts, PCLs, parsers, all of it") and reduced repo sprawl are
secondary benefits that fall out of the monorepo layout.

## Constraints & context

- **Module collision:** `NukeX`, `NukeX2`, and `nukex3`/`4`/`5` all build the same
  `NukeX-pxm.so` (same module ID, same install path). PixInsight can install only
  one. The unified repository must ship exactly **one** NukeX.
- **Single signing identity:** everything is signed by developer `scarter4work`
  (`/home/scarter4work/projects/keys/scarter4work_keys.xssk`). One signed manifest
  can therefore cover all packages.
- **Native signing only:** script signing MUST use PixInsight's own
  `Security.generateScriptSignatureFile()` (validated 2026-06-04). The
  reverse-engineered `pi_codesign.py` produced PI-rejected signatures (correct key,
  wrong signed-message format) and is retired.
- **Heterogeneous build systems:** C++ (CMake) modules, PJSR (JS) scripts, and
  Node (MCP server) tooling coexist. They stay isolated within the monorepo; no
  attempt to unify their build tooling beyond a shared signing/release layer.
- **No local installs for releases:** packages ship via the GitHub-served
  repository URL and PixInsight's update mechanism, never `make install`.

## Canonical decisions

| Decision | Choice |
|---|---|
| Source layout | **Full monorepo** (`astro-pi`) holding all source, archive, and the distribution dir |
| Canonical NukeX | **nukex5** (newest; 2026-05-01). Other four archived. |
| Legacy NukeX (v1–v4) | **Archived in-repo** under `archive/`, history preserved via `git subtree` |
| User migration | **Keep old repos as frozen redirects** — old URLs keep resolving; final release note points to `astro-pi` |
| Repo name | `astro-pi` (new GitHub repo) |
| nukex5 version | Reconcile internal `4.0.1.0` → clean **NukeX 5.0.0** on import |
| What actually ships | **NukeX module + the 3 EZ scripts only.** Everything else is tooling or archive. |

## Inventory & disposition

| Source | git? | Destination | Ships? |
|---|---|---|---|
| `nukex5` | yes | `modules/nukex/` | ✅ `NukeX-pxm` module |
| `EZ-suite-bsc/EZ-Stretch-BSC` | yes | `scripts/ez-stretch/` (incl. nested `BayesianAstro/`) | ✅ EZStretch, EZDonutRepair, EZHazeKill |
| `EZ-suite-bsc/pjsr_parser` | yes | `tools/pjsr_parser/` | no (dev tool) |
| `EZ-suite-bsc/pcl_parser` | no | `tools/pcl_parser/` (plain copy) | no (dev tool) |
| `EZ-suite-bsc/.../astro-stretch-studio` | yes | `tools/astro-stretch-studio/` | no |
| `NukeX` (v1/v2 C++) | yes | `archive/nukex-v1/` | no |
| `NukeX2` (ML/JS) | yes | `archive/nukex2/` | no |
| `nukex3` | yes | `archive/nukex3/` | no |
| `nukex4` | yes | `archive/nukex4/` | no |
| `pi-scripts/HazeKill.js` | no | `archive/pi-scripts/` | no (superseded by EZHazeKill) |

## Target layout

```
astro-pi/
  modules/
    nukex/                 # = nukex5 source (CMake), builds NukeX-pxm.so
  scripts/
    ez-stretch/            # PJSR scripts (.js + .xsgn), BayesianAstro research subtree
  tools/
    sign/                  # shared signing: SignScriptsNative.js + native module/XRI signing wrappers
    pcl_parser/            # Node MCP dev tool (not shipped)
    pjsr_parser/           # Node MCP dev tool (not shipped)
    astro-stretch-studio/  # research/dev (not shipped)
  archive/
    nukex-v1/ nukex2/ nukex3/ nukex4/   # history-preserved; never built or shipped
    pi-scripts/
  repository/
    updates.xri            # ONE signed manifest, all packages
    <NukeX module .tar.gz>
    EZStretch_v*.zip  EZDonutRepair_v*.zip  EZHazeKill_v*.zip
  release.sh               # top-level build → sign → package → manifest → verify
  CLAUDE.md                # single source of truth (supersedes the per-project ones)
  README.md
```

## Unified distribution

A single `repository/updates.xri`, signed once with the `scarter4work` identity,
contains two platform blocks:

- `<platform os="linux" arch="x64" version="1.8.0:2.0.0">` — the `NukeX-pxm`
  module package (`.tar.gz` containing `bin/NukeX-pxm.so` + `bin/NukeX-pxm.xsgn`).
- `<platform os="all" arch="noarch" version="1.8.0:2.0.0">` — the three EZ script
  packages (`.zip` each containing `src/scripts/.../<Name>.js` + `.xsgn`).

`releaseDate` drives PixInsight's "newer?" decision, so each republish bumps it.
All package `fileName`/`sha1` pairs are written into the manifest by the release
script (never hand-edited), then the manifest is signed last so no later edit can
invalidate a declared hash.

### Signing (validated 2026-06-04)

- Scripts: `tools/sign/SignScriptsNative.js` run via `PixInsight.sh -n
  --automation-mode -r=<script> --force-exit`, writing a machine-readable result
  file so a silent no-op cannot look like success.
- Module `.so`: `PixInsight.sh --sign-module-file=… --xssk-file=… --xssk-password=…`.
- Manifest: `PixInsight.sh --sign-xml-file=…`.

## History-preserving import

Each git source is folded in with
`git subtree add --prefix=<dest> <source-remote-or-path> <branch>` so every commit
survives under the new path. Non-git sources (`pcl_parser`, `pi-scripts`) are plain
copies. Order: import canonical/active trees first (`modules/nukex`,
`scripts/ez-stretch`, `tools/*`), then archive trees, so the working monorepo is
functional before the bulky archive merges.

## Frozen redirects (user migration)

`NukeX` and `EZ-Stretch-BSC` GitHub repos remain live but receive one final
release whose package description states the package has moved to the `astro-pi`
repository URL. Their `repository/` dirs are otherwise frozen. Existing registered
URLs keep resolving; no user is broken by the move. `astro-pi` becomes the only
actively updated repository.

## Release orchestration

`release.sh` performs, in order:

1. Build the module (`cmake` + `make` in `modules/nukex`).
2. Native-sign the module `.so` and the EZ scripts.
3. Package: module `.tar.gz`, script `.zip`s.
4. Write every package `fileName`/`sha1` into `repository/updates.xri`; bump
   `releaseDate`.
5. Sign the manifest (`--sign-xml-file`).
6. Integrity-check: declared SHA1 vs on-disk artifact for every package; fail loud
   on mismatch.

This mirrors the proven `build-packages.sh` ordering (sign → package → hash → sign
manifest) that guarantees package hashes and the signed manifest never drift.

## Testing & verification

- Module: `cd modules/nukex/build && ctest --output-on-failure`.
- Signatures: native-signed by construction = PI-valid; release script verifies the
  manifest re-signs cleanly and every declared SHA1 matches its artifact.
- End-to-end: register the `astro-pi` raw URL in PixInsight, confirm the module and
  all three scripts install and verify (done once at first publish).

## Out of scope (YAGNI)

- Unifying CMake/Node/JS build tooling beyond the shared signing/release layer.
- Shipping NukeX2 (ML/JS), the parsers, or astro-stretch-studio to users.
- Rewriting or version-bumping archived NukeX versions — they are frozen as-is.
- Deleting the old GitHub repos (kept as frozen redirects, not removed).

## Open implementation details (resolve during planning)

- Exact NukeX module-version reconciliation (5.0.0 vs keep 4.0.1.0) and whether the
  module-id/feature-id strings change.
- Whether `git subtree` imports use the local working copies or the GitHub remotes
  as the subtree source (remotes give canonical history; local copies may have
  uncommitted WIP — e.g. EZ-Stretch-BSC currently has dirty BayesianAstro changes).
- Final repository URL string and where `release.sh` writes the "moved to astro-pi"
  notice in the two frozen repos.
