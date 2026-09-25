# PI Copilot — Image Journey (design)

**Date:** 2026-09-25 · **Target release:** PICopilot **0.2.0.0** (one increment) · **Status:** approved in conversation, pending written-spec review

## 1. Purpose

Record each image's processing journey end to end, from the stacked master to the final image, whether the steps were done by hand or by PI Copilot. When the user declares a result a keeper ("the best one ever"), turn the journey into something reusable.

Priorities (user-ranked):
1. **Repeat it:** apply the workflow to a new master of the same target/filter/camera, with Copilot adapting to the new data.
2. **Compare and learn:** compare new work against keepers using recorded stats and acquisition facts.
3. **Document and share:** a readable write-up of how the image was made.

Motivating use: the user processes "their way" by hand, then runs a Copilot polish pass. So a journey must capture **manual steps**, not only Copilot's.

## 2. Decisions (made with the user)

| # | Decision |
|---|---|
| D1 | All three goals, in the order above. |
| D2 | The journey starts at the **stacked master**. Calibration/stacking is recorded as acquisition *facts* only (not replayed). |
| D3 | **Always recording:** tracking starts automatically when a master is opened; no user action needed. |
| D4 | Repeat = **both** an adaptive, Copilot-driven replay (default) **and** a literal `.xpsm` process icon set export. |
| D5 | Storage: a **local** library (always available), plus an **optional export folder** for keepers, set in ⚙ and **off by default** (other users may have no NAS). Scott's value: a folder under `/mnt/qnap/astro_data/`. |
| D6 | Capture: **image notifications first, with a history-index poll as a backstop**, if the first plan task proves notifications work; if they don't arrive while the panel is closed, the poll carries capture on its own. |
| D7 | Location/observer keywords are **never stored** (same redaction list as ViewContext). |
| D8 | Unkept journeys are pruned after **30 days** (setting); keepers are never pruned. |
| D9 | The keeper write-up is produced by **Haiku** (its only role in PI Copilot); replay and comparison use the user's chat model (Opus 5.5 default). |

## 3. Proven facts this design rests on

- **PJSR `View.processing`** returns the view's full processing history as a `ProcessContainer` (every step with full parameters, start time, execution time); **`View.initialProcessing`** holds the history embedded in the file at load time (XISF stores it); **`View.historyIndex`** gives the current undo position. Verified headless on PI 1.9.5 (slot 93, 2026-09-25): PixelMath + HistogramTransformation appeared with full parameters and timestamps.
- **PCL has no C++ API for view history** → read it via `MetaModule::EvaluateScript` (root thread only; already used by `run_pjsr` and increment 1).
- **PCL `ProcessInterface`** offers `WantsImageNotifications()` → `ImageCreated / ImageUpdated / ImageRenamed / ImageDeleted / ImageSaved`, plus process notifications. **Unproven:** whether they are delivered while the PICopilot interface is closed/hidden → plan task 1.

## 4. Architecture

Six new units inside the PICopilot module. Each has one job and a narrow interface.

| Unit | Responsibility | Depends on |
|---|---|---|
| **JourneyTracker** | Receives image notifications and runs a backstop timer that compares each open view's `historyIndex` with the last recorded one. Decides journey membership (a master, or an image linked to one), records links when a step creates a window, and queues work for idle ticks. | PCL notifications, HistoryReader, StepStats, JourneyStore |
| **HistoryReader** | Reads `processing` / `initialProcessing` / `historyIndex` via `EvaluateScript`. Returns structured steps (process id, parameters JSON, start time, duration) and computes the diff against the last recorded history: appended steps, undo (index down), redo (index back up), and a new branch after an undo. | EvaluateScript |
| **StepStats** | Per-channel median, MAD, mean, min, max and a noise estimate, plus a 256 px thumbnail, computed from a read-only block-averaged copy (the ViewContext/ViewPreview path; never a full-resolution duplicate). | ViewContext / ViewPreview code |
| **JourneyStore** | SQLite (vendored amalgamation, compiled into the module) plus a folder per journey under the platform user-data location (Linux: `$XDG_DATA_HOME/PICopilot/journeys`, default `~/.local/share/PICopilot/journeys`). Handles schema versioning, retention pruning, and the location-keyword redaction on write. | sqlite3 |
| **JourneyExport** | For keepers: writes the `.xpsm`, `recipe.json`, `journey.md` (Haiku) and thumbnails, then copies the folder to the optional export location. | JourneyStore, AnthropicClient |
| **Journey tools** | Agent tools `mark_journey_best`, `list_journeys`, `get_journey`, `compare_to_journey`, `replay_journey`, offered like the existing tools (`replay_journey` only in Copilot/Guided). | JourneyStore, JourneyExport, existing tool loop |

**Data flow:** a step is done (by hand or by Copilot) → notification or poll → JourneyTracker → HistoryReader diff → StepStats → JourneyStore. Keeper → JourneyExport. Replay → the model reads the recipe and uses the existing tools; each replayed step goes through the same process safety policy and mode rules as any Copilot action, and is recorded as a new journey.

**Threading:** all of it runs on the root thread inside timer/notification handlers (EvaluateScript, views and SQLite access are there). Work is deferred to an idle tick whenever a view is locked or a process is running (`View::CanRead()` probe; never wait on a lock). The only off-root work is the Haiku HTTP call, which uses the existing ChatThread pattern.

## 5. Data model

**Journey:** a small graph. **Roots** = one or more stacked masters (e.g. three for SHO). **Nodes** = image windows. **Edges** = links recording how one image was made from another. **End** = the image the user declares the keeper.

Tables (schema version 1):
- `journey(id, created, updated, name, target, kept, kept_at, end_image_id, status)`
- `image(id, journey_id, view_id, file_path, fingerprint, is_master, created)`: fingerprint = width × height × channels × sample format + master history hash.
- `acquisition(image_id, target, filter, camera, gain, offset, sensor_temp, sub_exposure, sub_count, total_integration_s, session_date)`: masters only. Fields come from FITS/XISF keywords and the master's embedded ImageIntegration history. Redacted keywords are never read into this table.
- `step(id, image_id, seq, process_id, params_json, started, duration_s, actor ('user'|'copilot'), reason, reason_inferred, state ('active'|'undone'|'superseded'), history_index)`
- `stats(step_id, channel, median, mad, mean, min, max, noise)`: `step_id` NULL = the master's starting stats.
- `link(from_image_id, to_image_id, via_step_id, evidence ('copilot'|'timing'|'reference'))`
- `gap(journey_id, image_id, after_step_seq, reason)`: recording failures, shown in the recipe as "unknown steps here".
- Thumbnails as files in `<journey-folder>/thumbs/<step_id>.jpg`.

**Membership:** an image joins a journey when (a) its `initialProcessing`/history begins with an integration result (a master), or (b) it is linked from a journey image. An unlinked new window is not tracked until a later step links it.

**Link evidence (checked in order):**
1. A Copilot tool created the window, so the source view is known.
2. **Timing:** the window appeared while a known step ran on a tracked view (e.g. StarXTerminator `<id>_stars`, ChannelExtraction, PixelMath `createNewImage`).
3. **References:** view ids named in the creating step's parameters (ChannelCombination channels, ids in PixelMath expressions).

Each link stores its evidence, and the write-up says "linked by timing" instead of asserting it.

**Undo/redo:** `historyIndex` going down marks the steps above it `undone`. Going back up restores them. A new step after an undo marks the undone ones `superseded`. Only `active` steps go into recipes and `.xpsm`.

**Retention:** a daily pass deletes non-kept journeys whose `updated` is older than the setting (default 30 days) and removes their folders. Keepers are never pruned.

## 6. Keepers, replay, comparison

**Declaring a keeper:** in chat ("this is the best one ever" → `mark_journey_best`) or with the ★ **Keep journey** button. Either way, a summary is shown first: masters, step count, links and their evidence, and gaps. The user confirms, then JourneyExport writes:
1. **`<name>.xpsm`:** a process icon set of the active steps in order, grouped per image with window names in comments. Native PI and usable without Copilot.
2. **`recipe.json`:** each step's parameters, stats before/after, the target it achieved (e.g. median 0.08 → 0.12), actor, and reason (stated or inferred), plus the acquisition facts and links. It has a versioned schema.
3. **`journey.md`:** written by Haiku from the recipe and acquisition facts (equipment → acquisition → processing, in plain language, with thumbnail references). Haiku receives **no image pixels**. Reasons Haiku infers for manual steps are labelled as inferred.
4. **Export copy:** if an export folder is set, the folder is copied to `<export>/<target>/<YYYY-MM-DD>-<name>/`. If that fails (unmounted, permissions), the log names the failure, the local copy stands, and the user can retry from chat.

Each output is independent: a failure in one is reported and does not block the others.

**Replay (`replay_journey`, "process this like my best Cone"):**
- Keepers are matched by target, filter and camera. With several matches Copilot asks which one; with none it says so.
- A plan is shown first, including which steps will adapt and why (e.g. "new master is noisier: stronger noise reduction").
- The chat model works through the recipe step by step using the existing tools, compares stats after each step with the recorded target, and adjusts parameters toward it.
- Mode rules apply unchanged: Guided confirms every step, Copilot runs, Advisor only presents the plan. The safety policy applies to every step, and History undoes everything.
- Steps that need the user (DBE/ABE sample points, hand-drawn masks, previews, anything with interactive geometry) are **listed as manual**: Copilot pauses and tells the user what to do. It never invents a substitute.
- The replay itself is recorded as a new journey.

**Comparison (`compare_to_journey`):** acquisition side by side (integration time, subs, conditions), the masters' starting stats (noise, background, signal), and where the processing diverged. Answered from the database; no image is sent.

## 7. UI

- **Journey strip** (one line above the chat): `Journey: <target> (<master kind>) · <n> steps · recording`, or `not tracked` / `recording paused: <reason>`. Clicking it opens a small read-only dialog listing the steps with thumbnails.
- **★ Keep journey** button on the strip.
- **⚙ additions:** *Record journeys* (default on), *Export folder for keepers* (default empty = off; folder picker), *Keep unsaved journeys for N days* (default 30).
- Everything else goes through chat.

## 8. Failure handling (loud, never silent)

| Failure | Behaviour |
|---|---|
| History read / stats / DB write fails | Strip shows `recording paused: <reason>`; retried on the next change; a `gap` row is recorded so recipes show "unknown steps here". |
| DB can't be opened or is corrupt | Reported in the strip and the log with the exact path. **Never recreated silently** (that would destroy keepers). Recording stays paused until fixed. |
| View busy (locked / process running) | Deferred to the next idle tick; never waits on a lock. |
| Replay step fails | Returned to the model as a normal tool error; the model adapts or stops; the replay log shows where. |
| Export sub-step fails | Each output is independent; what worked is saved, what failed is named. |
| Recording disabled in ⚙ | Strip shows `recording off`; nothing is read or stored. |

## 9. Performance

Work runs only after a step completes, never during one. Per step: one history read, stats on a block-averaged copy (tens of ms on 24 MP by analogy with the vision preview path; **to be measured** in the plan), a 256 px JPEG, and a few small inserts. The backstop poll only compares integers. Target: no user-visible stall. The plan measures the per-step cost on a 60 MP float image and sets a budget.

## 10. Testing

Headless self-test (slot 90, existing harness, `PICOPILOT_REQUIRE_LIVE=1` for release):
1. **Notification spike (plan task 1):** are `ImageCreated`/`ImageUpdated` delivered while the interface is closed? The result decides D6 (notifications alone, or notifications plus poll).
2. **HistoryReader:** parse the `processing` of synthetic views; diff for appended steps, undo, redo and a superseding branch.
3. **Tracker:**
   - a synthetic master (ImageIntegration history plus keywords) with PJSR-driven "manual" steps → correct steps and stats;
   - linking by all three evidence kinds (StarX-style split via a stand-in process, ChannelCombination, PixelMath `createNewImage`);
   - busy-view deferral.
4. **Store:** schema round-trip; retention (keepers survive); redacted keywords never written; a corrupt DB file → paused, not recreated.
5. **Export:**
   - the `.xpsm` loads and replays on a copy of the master with an identical result;
   - `recipe.json` validates against its schema;
   - export copy to a temp folder, plus an unwritable-target failure;
   - live gated: Haiku write-up produced, no pixels in the request.
6. **Replay (live gated):** two synthetic masters with different noise/background; Opus 5.5 replays a recorded recipe and lands within tolerance of the recorded median per step.
7. **Agent tools:** schemas, mode gating (`replay_journey` never offered in Advisor), and errors.

**GUI (user checklist, repo pull):** journey strip states, steps dialog, ★ Keep, ⚙ fields, replay in Guided and Copilot.

## 11. Out of scope

Calibration/stacking replay (WBPP); multi-machine sync of the library; sharing journeys with other users beyond the exported folder; Windows/macOS builds (paths are platform-neutral so this is not blocked).

## 12. Risks / open points (resolved during the plan)

1. Notification delivery while the interface is closed (task 1, decides D6).
2. `ProcessContainer` export as `.xpsm` from C++: via `EvaluateScript` writing a process icon set, or by serialising `toSource()` — the plan picks after a probe.
3. History reads on views with very long histories (hundreds of steps): cost and size, measured in the plan.
4. Masters not produced by ImageIntegration/WBPP (e.g. third-party stackers): detected by keywords where possible; otherwise the user can start a journey from chat (`start_journey` on the active view).
5. Vendoring SQLite: amalgamation + licence (public domain) under `third_party/`, following the vendoring rules (source + licence only).
