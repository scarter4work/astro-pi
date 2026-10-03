### Task 24: Memory closeout

**Files (all under `~/.claude/projects/-home-scarter4work-projects-nukex5/memory/`):**
- Create: `project_v5000_closeout.md`
- Modify: `MEMORY.md` (replace the "RESUME HERE" line), `project_v5_session_pause_2026-05-01.md` (mark superseded), `project_color_science_brainstorm.md` (checklist items 8–10 → `[x]`)

- [ ] **Step 1: Write the closeout memory**

```markdown
---
name: v5.0.0.0 Color Science Overhaul Closeout
description: v5.0.0.0 SHIPPED <date> from github.com/scarter4work/nukex5. Filter taxonomy + QE Q-solve + Lab/LCH composer; M16 HaO3 green cast fixed; QE DB ships under <PI>/share. Rating DB user_version 2.
type: project
---

**Status <date>:** v5.0.0.0 shipped; updater install verified on this machine (bin/ + share/ delivered).

## What changed vs the April plan
- Task 14 mapping corrected (rating-axis codes, not enum ints); rating DB user_version 1 → 2.
- 14b: broadband LPR names resolve by sensor type (M16 LPro frames stack as OSC again).
- 15: transform derives canonical HaO3/S2O3/… from product entries; ships generic_sony_imx_osc.
- 15b: INSTRUME normalisation + substring resolution; spec 6.3 generic fallback + NUKEX_QE_CONFIDENCE.
- 16: qe_database_path from PixInsightSettings "Application/BaseDirectory"; release.sh ships share/.
- 21: spec's "M27 HaO3" corpus does not exist; M16 HaO3 (~/projects/processing/M16, *HaO3*.fit) is the dual-NB corpus. v4 LRGB-mono golden preserved bit-identical.

## Real-data validation
- M16 HaO3 12 frames: <verdict from visual-evidence/README.md>
- M27 LRGB-mono 2025 (ATR585M): <verdict>
- M27 OSC 2023 (ASI2400MC): <verdict>

## Environment
- Fedora 44 / GCC 16.2.1 / glog 0.7.1: stale build dirs from before the OS upgrade fail on libglog.so.0 — clean rebuild.

## Open follow-ups (each needs a decision, per no-stub-tickets)
- S2O3 and LRGBSHO corpora: none on disk; E2E placeholders skip.
- Per-camera Q dispatch for mixed-camera dual-NB batches: engine loud-fails today.
```

- [ ] **Step 2: Index + supersede**

In `MEMORY.md` replace the `project_v5_session_pause_2026-05-01.md` line with `- [project_v5000_closeout.md](project_v5000_closeout.md) — v5.0.0.0 SHIPPED <date>: colour-science overhaul; QE DB under <PI>/share; rating DB v2; M16 HaO3 validated.` and append `- [project_v5_session_pause_2026-05-01.md](…) — superseded by the closeout; keeps the Task 14 mapping rationale.` Prepend `**SUPERSEDED <date> by project_v5000_closeout.md.**` to the pause memory. Tick items 8–10 in `project_color_science_brainstorm.md`.

No commit — memory files are outside the repo.

---
