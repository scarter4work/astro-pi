# Consolidated fix-wave report

Branch: `feat/rc-astro-pi-wrappers` (committed there, no branch switch).

## Fix 1 — wrong PixInsight menu name (Important)

**Claim checked**: README.md / install.sh said `Scripts > RC-Astro > ...` (plural), and
install.sh was internally inconsistent (`SCRIPT menu` in step 1 vs `Scripts >` in step 3).

**Evidence gathered (empirical, on this box, PI 1.9.4 "Lockhart" at `/opt/PixInsight`)**:

1. PixInsight's own bundled doc, `/opt/PixInsight/doc/docs/ScriptCodeSigning/ScriptCodeSigning.html`
   line 264, states directly:
   > "The `#feature-id` directive is best suited for JavaScript scripts that can be
   > included in the Script main menu item ... or by means of the *Script > Feature
   > Scripts...* command."
   This is the authoritative source: the top-level menu item is **`Script`** (singular),
   and the registration command is exactly `Script > Feature Scripts...`.

2. Corroborating evidence from bundled/vendored scripts under
   `/opt/PixInsight/src/scripts/`:
   - `AdP/MosaicByCoordinates-change-log.txt:40` and `AdP/MosaicPlanner.js:33`:
     "Move the script to the SCRIPT > Mosaic menu."
   - `GaiaDepthGrade/GaiaDepthGradeDialog.js:7` (the user's own script):
     "Run from PixInsight: Script > Execute Script... -> this file."
   - `CosgrovesCosmos/AstroColorMixer/AstroColorMixer.pidoc:14`: "Manual beta install
     currently uses Script > Execute Script."

   No bundled script, changelog, or doc anywhere under `/opt/PixInsight` uses the
   plural "Scripts" for the top-level menu.

3. `#feature-id` directives across `/opt/PixInsight/src/scripts/*.js` follow the
   `Category > Name` convention (e.g. `#feature-id AstroDepth : SetiAstro > AstroDepth
   Image Enhancer`), confirming that `#feature-id RC-Astro > BlurXTerminator (CLI)`
   places the tools at `Script > RC-Astro > BlurXTerminator (CLI)` — the category
   becomes a submenu under the singular `Script` top-level item, not under a
   plural "Scripts" item.

**Fix applied**: `README.md` and `install.sh` now consistently say `Script` (singular)
everywhere — the intro paragraph, the tool-location line, the GPU/tools table copy,
and both registration steps 2 and 3 in each file (previously step 1 of install.sh said
`SCRIPT menu` while step 3 said `Scripts >`; both now read `Script`).

## Fix 2 — silent fallback in SXT stars output (Important, standing project rule)

`RCAstroSXT.js` `runSXT()`: previously, if `SXTParams.outputStars` was true and `r.ok`
was true but `starsP` was missing, the `if (SXTParams.outputStars && File.exists(starsP))`
guard silently skipped opening the stars window — no error, no message, nothing.

**Fix**: split the check. `outputStars` alone gates the branch; inside it, a missing
`starsP` now calls `RCAstro.fail(...)` with a message naming the exact expected path,
which (per `RCAstro.fail`) does a loud `console.criticalln` + modal `MessageBox` + throws
— matching every other error path in this codebase (e.g. the "No target view" and
"StarXTerminator failed" checks two lines above it). No public signature changed;
`buildArgs`/params untouched.

## Fix 3 — SXT README table missing CPU option (Minor)

SXT's `--device` table row said only `gpu`; BXT/NXT say `gpu (default) / cpu`. The code
(`RCAstroSXT.js`'s `dev` ComboBox: GPU/CPU, `SXTParams.device`) already supports both.
Changed the README row to `gpu (default) / cpu` to match.

## Fix 4 — cuDNN claim needs a citation (Minor)

README's GPU-requirement paragraph rewritten to name the source (NVIDIA's cuDNN release
notes documenting a heuristics-engine issue in all cuDNN 9.x releases before 9.13.0 that
causes bad convolution engine recommendations on Blackwell/sm_120) and the observed
local symptom (cuDNN 9.8.0 → silent CPU fallback + `CUDNN_FE ... HEURISTIC_QUERY_FAILED`
for `smVersion:1200`; fixed by upgrading to cuDNN 9.24.0).

## Fix 5 — runCli silently dropped malformed JSON lines (Minor, runtime code)

`RCAstroLib.jsh` `runCli()`'s `dispatch()`: a line starting with `{` that failed
`JSON.parse` was silently discarded (`catch (e) { return; }`).

**Fix**: on parse failure, emit `console.warningln(...)` with the parse error and the
raw line, and fold the raw line into `strayText` (the same accumulator already used for
non-JSON stray output, e.g. C++ abort/terminate messages) so it can still surface via
the existing stderr/stray fallback path in `errorMsg` when a run fails. No signature
change to `runCli`.

## Verification (real runs, not faked)

```
cd /home/scarter4work/PixInsightScripts/RC-Astro
for t in t_lib_runcli:/tmp/rc_t2_result.log t_sxt:/tmp/rc_t4_result.log; do
  n=${t%%:*}; f=${t##*:}; rm -f "$f"
  test/run-headless.sh "$PWD/test/$n.js" >/dev/null 2>&1
  echo "--- $n ---"; cat "$f"
done
```

Output:

```
--- t_lib_runcli ---
progress events seen: true
device used: gpu (NVIDIA GeForce RTX 5070 Ti)
negative-path errorMsg: terminate called after throwing an instance of 'rcastro::Error'
what():  '/home/scarter4work/pixinsight-swap/rc-astro-1783811564942-446519/not_really_xisf.xisf' is not a valid XISF file (bad signature)
PASS t_lib_runcli
--- t_sxt ---
wrote: /home/scarter4work/pixinsight-swap/rc-astro-1783811572904-717604/image.xisf
wrote: /home/scarter4work/pixinsight-swap/rc-astro-1783811572904-717604/image_starless.xisf
wrote: /home/scarter4work/pixinsight-swap/rc-astro-1783811572904-717604/image_starless-stars.xisf
argv[outputStars=false]: ["IN.xisf","--device","gpu","--output","OUT.xisf"]
argv[outputStars=true, unscreen=true]: ["IN.xisf","--output-stars","--unscreen","--device","gpu","--output","OUT.xisf"]
argv[mlVersion=11]: ["IN.xisf","--output-stars","--unscreen","--ml-version","11","--device","gpu","--output","OUT.xisf"]
PASS t_sxt
```

Both PASS. Neither test needed weakening; both exercise the real `rc-astro` binary on
GPU end to end (the stars-file existed in the real run, so Fix 2's new failure path
wasn't hit here — it's the same code path already covered by the pre-existing
"no starless output" / "no stars output at discovered path" asserts in `t_sxt.js`,
which prove the file actually gets produced by the CLI under normal operation; Fix 2
only changes what happens in the CLI-success-but-file-missing case, which did not occur
in this run).

## Constraints respected

- No public `RCAstro.*` name/signature changed.
- No change to params/buildArgs behavior of BXT/SXT/NXT (Fix 2 only changed the
  post-run file-existence handling, not argument building).
- `RCAstro.applyInPlace()` ownership/forceClose behavior untouched (not touched by
  this fix wave at all — SXT never calls it).
