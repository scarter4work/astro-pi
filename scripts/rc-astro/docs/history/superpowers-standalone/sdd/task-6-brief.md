### Task 6: Install, register, README, full-suite verification

**Files:**
- Create: `~/PixInsightScripts/RC-Astro/install.sh`
- Create: `~/PixInsightScripts/RC-Astro/README.md`

- [ ] **Step 1: Write `install.sh`**

Create `install.sh`:

```bash
#!/usr/bin/env bash
# RC-Astro PI wrappers are already in this directory. This prints the one-time
# Feature Scripts registration steps (PJSR cannot register feature dirs itself).
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
echo "Scripts live in: $DIR"
echo
echo "One-time registration in PixInsight:"
echo "  1. SCRIPT menu > Feature Scripts..."
echo "  2. Add  ->  select: $DIR"
echo "  3. Done. Then find them under:  Scripts > RC-Astro > *(CLI)"
echo
echo "rc-astro binary: $(command -v rc-astro || echo 'NOT FOUND on PATH')"
```

Then `chmod +x install.sh`.

- [ ] **Step 2: Write `README.md`**

Create `README.md` with: purpose (CLI wrappers avoiding the plugin's Blackwell TF crash), the three tools + their key params, install steps (run `install.sh`, then Feature Scripts add), the headless test commands, and a note that GPU requires system cuDNN ≥ 9.13 (already installed: 9.24). Reference the design doc.

- [ ] **Step 3: Run the full headless test suite**

Each test writes its verdict to its own result log (`console.*` does not reach stdout under `--automation-mode`). Map test → result log:

```bash
cd ~/PixInsightScripts/RC-Astro
declare -A LOG=( [t_lib_roundtrip]=/tmp/rc_t1_result.log [t_lib_runcli]=/tmp/rc_t2_result.log \
                 [t_bxt]=/tmp/rc_t3_result.log [t_sxt]=/tmp/rc_t4_result.log [t_nxt]=/tmp/rc_t5_result.log )
fail=0
for t in t_lib_roundtrip t_lib_runcli t_bxt t_sxt t_nxt; do
  rm -f "${LOG[$t]}"
  test/run-headless.sh "$PWD/test/$t.js" >/dev/null 2>&1 || true
  if grep -q "PASS $t" "${LOG[$t]}" 2>/dev/null; then echo "== $t OK =="
  else echo "== $t FAILED =="; cat "${LOG[$t]}" 2>/dev/null; fail=1; fi
done
exit $fail
```
Expected: all five print `OK`.

- [ ] **Step 4: Register + interactive verification (manual, once)**

Run `./install.sh`, follow the Feature Scripts steps in PI, then confirm all three appear under `Scripts > RC-Astro` and run on an open image (BXT/NXT modify in place + undo; SXT spawns `_starless` and, when enabled, `_stars`). Force an error (e.g., set Device combo via a bad temp edit or disconnect) to confirm a loud MessageBox.

- [ ] **Step 5: Commit**

```bash
cd ~/PixInsightScripts/RC-Astro
git add install.sh README.md
git commit -m "feat: install script, README, full-suite headless verification"
```

---

## Self-Review notes (author)

- **Spec coverage:** engine (§4)→Tasks 1–2; BXT/SXT/NXT dialogs+params (§5)→Tasks 3–5; automation/Parameters (§6)→each tool's `main()` + new-instance button; error handling (§7)→`RCAstro.fail` + `runCli` error path; testing (§8)→headless `t_*` + manual steps; install (§9)→Task 6.
- **Known implementer follow-ups (flagged explicitly):**
  1. **Dialog-open check:** the `t_bxt/t_sxt/t_nxt` headless tests exercise the engine + arg-building path, not the GUI dialog (dialogs can't run under `--automation-mode`). Each tool's dialog must additionally get a one-time manual open check (Task 3/4/5 Step 5 / Task 6 Step 4).
  2. **Task 4 Step 2 discovery:** the stars-only output filename is *verified from disk*, not assumed; `runSXT`'s `starsP` must match what the CLI actually writes.
  3. **PixInsight API spot-checks during implementation** (confirm against `/opt/PixInsight/src/scripts` examples, don't assume): `NumericControl.setRange/setPrecision/setValue`, `ViewList.getMainViews/onViewSelected`, `SectionBar.setSection/onToggleSection`, `ImageWindow.open` arg signature, `PixelMath` in-place `executeOn(view)` undo behavior, and `format()` availability (used for arg formatting).
- **Type consistency:** all tools use `RCAstro.{findBinary,tempDir,saveView,importResult,applyInPlace,newWindow,runCli,cleanup,fail}` exactly as defined in Tasks 1–2; each `*Params.buildArgs(inPath,outPath)` returns a string array consumed by `runCli(tool, args, onEvent)`.
