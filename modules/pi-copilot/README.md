# PI Copilot Native PCL Module

This is the native PixInsight PCL implementation of PI Copilot, replacing the retired Node.js sidecar. It implements the core scripting engine and process management layer as a loadable PI module, enabling AI-assisted workflows directly within PixInsight through a dockable interface.

## Build

From `modules/pi-copilot/`:

```bash
cmake -B build -DPCLDIR=$HOME/PCL -DPICOPILOT_BUILD_MODULE=ON && cmake --build build -j$(nproc)
```

## Test

Self-test (proves `EvaluateScript==3`, `ProcessInstance` validity, a Settings round-trip, the worker-thread HTTP path, and request cancel/deadline):

```bash
bash test/run-selftest.sh
```

This signs the module and requires `/tmp/.pi_codesign_pass`. It covers:
- `EvaluateScript` execution: `1+2 == 3`
- `ProcessInstance` construction and validity
- Settings round-trip (throwaway key `PICopilot/SelfTestKey`, removed afterwards — never the stored API key)
- Worker-thread HTTP POST (invalid key → 401 from Anthropic API)
- Cancel and overall deadline against a local server that accepts and never replies (started by the harness): cancel mid-stall ends the turn with `request cancelled`; a 3 s deadline ends it with `request timed out after 3 s`
- Chat-log escaping: a literal `</raw>` in text stays literal in a real `TextBox`
- Real tiny Anthropic Messages API call (only if gitignored `test/.test_api_key` file is present with a valid key; otherwise skipped)

Also: `bash test/run-load.sh` — loads the module headlessly without running self-test.

## Design & Increment 1 Scope

Full specification: [`docs/superpowers/specs/2026-09-20-pi-copilot-native-pcl-design.md`](../../docs/superpowers/specs/2026-09-20-pi-copilot-native-pcl-design.md)

Increment 1 deliverables:
- Native module skeleton with CMake build
- Empty dockable `ProcessInterface` panel
- `EvaluateScript("1+2") == 3` proof of PJSR execution from C++
- Self-contained self-test shipped in-module. It runs on `ExecuteGlobal` **only** when the harness sets `PICOPILOT_SELFTEST_OUT`; in a normal install, executing the process just prints a console hint to open the panel — no Settings writes, no network, no files (harmless in production)
- Signing and headless loading verified

## Chat, key and settings (as of 0.1.2.0)

- **⚙ settings** (the gear button): your own Anthropic API key (BYO-key; trimmed; a key with spaces, line breaks or other non-printable-ASCII characters is rejected and nothing is saved), the **model** (exactly two: **Claude Opus 5.5**, the default, and **Claude Sonnet 5** — takes effect from your next message; a saved model that is no longer offered switches to Opus 5.5 with a one-time note in the chat log), **Allow scripts** (see *Scripts* below; off by default) and the panel's **default side** (right or left).
- **Key storage: the system keyring.** The key is stored with `secret-tool` (the freedesktop Secret Service, e.g. GNOME Keyring) under `service picopilot account anthropic-api-key`; the dialog says where it is. A key from 0.1.1.x or earlier, kept in plaintext in PixInsight's settings, is moved into the keyring on first use — the plaintext copy is removed only after the keyring copy has been read back equal. If no keyring is available (`secret-tool` missing, or the keyring refuses), the key falls back to PixInsight's settings **in plain text**, and the dialog says so. To remove the key, empty the field in ⚙ and press OK (both copies are removed). The key is never shipped, logged or shown.
- **Replies stream**: text appears as it is written. Each request has an overall **600 s** limit, and a stream that sends nothing for **120 s** ends as stalled. **Stop** cancels the request in flight; a reply that broke off (Stop or a failure) is marked as such in the chat log. A reply cut off by the output limit ends with `[truncated: max_tokens]`.
- **New chat** starts a fresh conversation; your images and their History are untouched.
- **Long conversations**: when the history grows past its budget (about 100,000 tokens, estimated), the oldest messages stop being sent to the model and the chat log says how many; New chat starts fresh. Prompt caching keeps repeated context cheap. On Opus 5.5, their reasoning is kept and re-sent so the model stays consistent across tool steps.
- **Errors** from the API (e.g. an invalid key, rate limits, overload) are shown verbatim in the chat log; a failed turn is dropped from the history and its prompt is put back in the input line.
- **GUI scope**: the panel and dialogs cannot be tested headlessly (`--automation-mode` can't run GUI); they are verified by hand on the released build.

## Increment 3 — Vision (the panel sees the active view)

- **Include view** (checkbox, on by default): each message carries the active view — an auto-stretched JPEG preview (long edge ≤ 1024 px, quality 85; display only) plus a JSON context: view id, **file name only** (never the full path), geometry, per-channel median / raw MAD / mean / min / max of the **real (usually linear) data**, and the first 60 FITS keywords (values cut at 80 characters). **Location/identity keywords are never sent** (SITELAT, SITELONG, SITEELEV, OBSGEO-B/L/H, LAT-OBS, LONG-OBS, ALT-OBS, OBSERVER). There is no process-history field: PCL has no API for it.
- **The preview path never modifies your image.** (Processes change it only through the increment-4 agent tools, and only in Copilot or Guided mode — see below.) The preview reads the view read-only and block-averages it into a small copy (≈32 MiB for a 24 MP frame, never a full-resolution duplicate) → resample → auto-STF → JPEG via a temp file that is always deleted. The self-test proves the image byte-identical before/after for 32-bit float, 16-bit RGB and 16-bit mono.
- **Busy view** (locked by a running process or script) → the message is sent as text with a visible note, instead of waiting on the lock (which would freeze PixInsight).
- **No active image** → text only, with a visible note. A context or preview failure is shown in the chat log and the text still sends.
- **Token cost:** only the latest message carries an image; older turns keep a one-line note instead of the picture and a collapsed context (view id + geometry).
- **Placement:** PCL has no docking API. On first open after this update the panel is placed at the right edge, full height, of the primary screen (estimated from its centre — PCL exposes nothing else); after that, PixInsight remembers wherever you move it.
- **Process catalog** (`list_processes` / `describe_process` JSON from native introspection + compiled-in summaries) is built and self-tested; it becomes a model tool in increment 4.
- **Self-test** additions: vision smoke (ImageWindow/Bitmap/JPEG headless), ViewContext values + redaction, ViewPreview (JPEG, size, unchanged image, temp removed, red square decodes red; float, uint16, mono, >2048 px), busy-view capture, ProcessCatalog, request content-block shape, history stripping/collapse, placement maths, and a gated **real vision call** (synthetic red square, image only → model must answer exactly "red"). Key source: system keyring (`secret-tool lookup service anthropic account default`), then `test/.test_api_key`, else skipped. Tests run in an isolated PixInsight instance slot (`PICOPILOT_TEST_SLOT`, default 90, must be ≥ 50) so they never touch your own PixInsight settings.

## Increment 4 — Agent (the model can work on your image)

- **Tools** the model can call: `list_processes` (installed processes), `describe_process {id}` (parameter ids, types, ranges, enumeration ids, table columns, defaults), `get_view_context {include_preview, view_id}` (fresh statistics / FITS keywords / optional new preview of a view), and — except in Advisor — `apply_process {process_id, parameters, table_parameters, view_id}`, which runs a process on the **real image**, starting from the process's defaults and setting only the given parameters. Every value is validated before PixInsight sees it; every failure goes back to the model as a precise error. Also except in Advisor, `run_global_process {process_id, parameters, table_parameters}` runs a process in the global context, e.g. ImageIntegration over frames on disk (absolute, existing, readable image files; at least 3 enabled frames): it opens **new** image windows and never changes an open image, and returns the result windows' statistics plus a preview of the main result. Guided always asks first; Copilot asks when the safety policy below says so.
- **Modes** (selector at the top left; persisted; a change applies from your next message):
  - **Copilot** — applies processes directly when you ask. Every run is recorded in the view's History, so Undo / the History Explorer work as usual.
  - **Guided** — shows each process and its parameters in a dialog and runs it only if you press **Yes** (the default button is **No**; Esc also declines).
  - **Advisor** — read-only: looks and advises, never changes the image.
- **Which image:** the target is the view that was active when you pressed Send (with Include view on, the one whose preview went with the message; with it off, still the view active at Send) — clicking another image while a request runs does not redirect it. The model can work on another view only after inspecting it with `get_view_context` in the same message. Each applied process is logged in the chat as `… on <view id>`.
- **Stop** ends the message: no further tool runs (a process that is already running always finishes) and the request in flight is cancelled. **New chat** starts a new conversation; your images and their History are untouched.
- **Limits:** at most **12** tool steps (model responses that call tools) per message, and at most **8** tool calls per step; extra calls are answered "not executed" and the model is asked to summarize.
- **Failures:** if a process started but did not complete (an error found while running, or you aborted it), the reason is in PixInsight's **Process Console** — the module cannot read it back, so the chat only says it failed.
- **Tool results** longer than 20,000 characters are cut, with a visible note to the model saying how much was left out (the chat log says so too).
- **Metadata is not a security boundary.** The model is *told* that text in image metadata (FITS keywords, file names) and in tool results is data, not instructions — but that is guidance to the model, not something PI Copilot can enforce. What limits what can happen is the mode you choose, the tools offered in it, the process safety policy and the confirmation dialogs below.

## Process safety policy

PI Copilot checks every process it runs (apply_process, run_global_process) against a compiled policy (data/process-safety.json). The self-test fails if an installed process with file, directory, overwrite or window-closing parameters is not classified, or if an installed process that can run in the global context has not been reviewed for global runs.

Before any dialog — for apply_process and run_global_process alike — every parameter goes through the same checks the run itself makes, as a dry run on a throwaway copy of the process: unknown ids, types, ranges, enumeration ids, table shapes and read-back; text containing a NUL character (U+0000) is refused anywhere (the core would silently cut it there); two keys that name the same parameter (its id and an alias) are refused; text parameters the policy restricts (its `parameterValues` section) must be one of the allowed values or match the declared format (`"version"`: empty, or 2–4 dot-separated groups of digits such as `3.0.2`); and, for global runs, file paths in file tables (ImageIntegration, HDRComposition, GradientHDRComposition, GradientMergeMosaic) must be absolute, existing, readable image files, with enough enabled frames. The process's own validation (its Validate step, and whether it can run on that image or globally) and the run itself still come after the dialog, so an approved call can still be refused there — but never for a parameter error the checks above can see. One exception: a process the policy always asks about (for example BlurXTerminator, StarXTerminator and NoiseXTerminator, whose compiled plug-ins can crash PixInsight on Blackwell GPUs) is never even instantiated before you approve it, so for those the pre-dialog checks are the ones that need no process instance (unknown ids, types, ranges, text, value rules, table shapes, NUL, pinned parameters and, for global runs, file paths); enumeration values and read-back are checked after you approve, with the same messages, and nothing runs if they fail.

A "confirm" asks you in every mode, Copilot included, and the dialog defaults to No. A denied process returns a precise error to the model; nothing runs. Advisor mode never runs anything.

| Process | PI Copilot will | Why |
|---|---|---|
| IndigoCCDFrame | never run it | it controls a camera through an INDIGO server (exposures, uploads, file saving) |
| IndigoDeviceController | never run it | it sends commands to observatory devices through an INDIGO server |
| IndigoMount | never run it | it moves the telescope mount through an INDIGO server |
| NetworkService | never run it | it runs PixInsight as a network processing service that fetches tasks from a remote server and executes them |
| PICopilot | never run it | it is PI Copilot itself (running it from the chat would recurse) |
| Preferences | never run it | it changes PixInsight's application settings, including its script-signature security settings |
| ProcessContainer | never run it | it runs a list of other processes that PI Copilot cannot check one by one |
| Script | never run it | it runs a PJSR script file, which PI Copilot cannot review from here (scripts go through run_pjsr, which shows the whole script first) |
| APASS | always ask you first | it can write catalog search results to files, and its configure commands change the catalog database settings |
| BlurXTerminator | always ask you first | its compiled plug-in can crash PixInsight (SIGABRT) on NVIDIA Blackwell GPUs such as the RTX 50 series, losing unsaved work; Script > RC-Astro runs the same tool through its command-line version instead |
| ColorManagementSetup | always ask you first | it changes PixInsight's global color-management settings |
| CometAlignment | always ask you first | it writes comet-aligned copies of the input frames to its output directory and can overwrite existing files |
| CosmeticCorrection | always ask you first | it writes corrected copies of the input frames to its output directory |
| Debayer | always ask you first | it writes debayered copies of the input frames to its output directory |
| DrizzleIntegration | always ask you first | it reads drizzle data files from disk and can write output files |
| EphemerisGenerator | always ask you first | it writes ephemeris (.xeph) and log files to disk |
| FastIntegration | always ask you first | it writes registered frames, log and weights files to its output directory |
| FilterManager | always ask you first | it reads and writes the filters database file |
| Gaia | always ask you first | it can write catalog search results to files, and its configure commands change the catalog database settings |
| ImageCalibration | always ask you first | it writes calibrated copies of the input frames to its output directory and can overwrite existing files |
| MARSGen | always ask you first | it generates MARS gradient-model database files on disk |
| LocalNormalization | always ask you first | it writes normalization data files (.xnml) to disk |
| NSGXnml | always ask you first | it writes normalization data files (.xnml) to its output directory |
| NoiseXTerminator | always ask you first | its compiled plug-in can crash PixInsight (SIGABRT) on NVIDIA Blackwell GPUs such as the RTX 50 series, losing unsaved work; Script > RC-Astro runs the same tool through its command-line version instead |
| NukeX | always ask you first | it writes cache files to its cache directory while stacking |
| RGBWorkingSpace | always ask you first | in the global context it changes PixInsight's default RGB working space for every image; on a view it assigns a new working space to that image |
| ReadoutOptions | always ask you first | it changes PixInsight's global pixel readout options (what the cursor readouts show and how they are computed) |
| SplitCFA | always ask you first | in the global context it writes split CFA frames to its output directory |
| StarAlignment | always ask you first | in the global context it writes registered copies of the input frames to its output directory |
| StarXTerminator | always ask you first | its compiled plug-in can crash PixInsight (SIGABRT) on NVIDIA Blackwell GPUs such as the RTX 50 series, losing unsaved work; Script > RC-Astro runs the same tool through its command-line version instead |
| SubframeSelector | always ask you first | it can copy or move approved and rejected frames to output directories |
| SubframeStudio | always ask you first | it writes measurement CSV files and a metrics cache to disk |
| CreateAlphaChannels | ask you first when `closeSource` is `true` | it closes the source image window used for the alpha channel, and unsaved changes there are lost |
| FindingChart | ask you first when `generateBitmapFile` is `true` | it writes the finding chart as an image file to its output directory |
| HDRComposition | ask you first when `closePreviousImages` is `true` | it closes the images created by a previous HDRComposition run, and unsaved results are lost |
| ImageIntegration | ask you first when `generateDrizzleData` is `true` | it updates the frames' .xdrz drizzle files on disk |
| ImageIntegration | ask you first when `closePreviousImages` is `true` | it closes the images created by a previous ImageIntegration run, and unsaved results are lost |
| MultiscaleGradientCorrection | ask you first when `command` is not `""` | a command runs a MARS database operation instead of a gradient correction |
| PhotometricColorCalibration | ask you first when `generateTextFiles` is `true` | it writes calibration text files to its output directory |
| SpectrophotometricColorCalibration | ask you first when `generateTextFiles` is `true` | it writes calibration text files to its output directory |
| SpectrophotometricFluxCalibration | ask you first when `generateTextFiles` is `true` | it writes calibration text files to its output directory |

Reviewed and allowed without asking (their file/path-like parameters have no effect beyond the image or new windows): ACDNR, ATrousWaveletTransform, AssignICCProfile, AstroResolver, AutomaticBackgroundExtractor, B3Estimator, ColorCalibration, DynamicAlignment, ExtractAlphaChannels, GradientHDRComposition, GradientHDRCompression, GradientMergeMosaic, GraXpert (see below), HDRMultiscaleTransform, ICCProfileTransformation, LRGBCombination, MLDenoise, MergeCFA, MorphologicalTransformation, MultiscaleLinearTransform, PixelMath, RestorationFilter, SCNR, TGVDenoise, UnsharpMask.

**GraXpert runs with the program you chose.** Its `appPath` parameter names the program GraXpert launches, so the model may never set it: PI Copilot fills it from **your own GraXpert setting** (the path you set in the GraXpert process window, read from PixInsight's settings), after checking that it is an existing executable file (links followed). The path is resolved **once**, before any dialog: the program the Guided dialog shows is exactly the program that runs, even if the setting or a symbolic link changes while the dialog is open. If the model passes `appPath` at all (in either parameter object, by any spelling that PixInsight resolves to it), the call is refused; `describe_process` marks it as set by PI Copilot so the model knows not to. If the setting is missing or invalid, the model is told to ask you to open GraXpert once and set its path — PI Copilot never falls back to searching your PATH. The chat log (and the Guided dialog) show the program that runs, e.g. `[appPath = /opt/graxpert/GraXpert, set by PI Copilot]`. GraXpert's other text parameters also reach its command line, so they are restricted to what they mean (policy `parameterValues`: an exact list of allowed values, or the `"version"` format): the AI model versions to a version number such as `3.0.2` or empty (GraXpert's default), `correction` to Subtraction/Division and `deconvolutionMode` to Object-only/Stars-only; anything else (e.g. `--flag`, a path) is refused before any dialog. Otherwise GraXpert behaves as usual: with `replaceImage` it changes the image (undoable in History), without it it opens new windows; like running GraXpert yourself, it may download its AI models on first use. The policy's `pinnedParameters` and `parameterValues` sections are general: any text parameter can be pinned, or restricted to a list of allowed values or the version format, this way, and a malformed pinned entry refuses the process rather than running it unpinned.

This pinning governs apply_process and run_global_process. A `run_pjsr` script (off by default) could still create a GraXpert instance with any `appPath` — but a script can start any program anyway, which is exactly why every script is shown to you in full, and runs only if you press **Run script**.

**Global runs** (run_global_process) are reviewed separately from runs on a view — also for the processes above that are allowed or conditionally asked about on a view, since that review was about their effect on an image. Most processes only inherit a "can run globally" flag and the core then refuses the run; those are listed as reviewed (`globalSafe`), as are the processes whose global run only reads images or frames and creates new image windows: NewImage, ChannelCombination, InverseFourierTransform, LRGBCombination, MergeCFA, PixelMath (with a new image), ImageIntegration, HDRComposition, GradientHDRComposition and GradientMergeMosaic (their confirm rules above still apply) — plus NoOperation. AstroResolver, B3Estimator, Blink, DynamicBackgroundExtraction, DynamicPSF, MultiscaleGradientCorrection and Statistics can run globally in ways PI Copilot cannot inspect, so a global run of them always asks (`globalConfirm`), together with any confirm rule that also applies. A global run of any process not reviewed this way (a newer PixInsight, a third-party module) asks too ("it has not been reviewed for global runs"), because a global run can change PixInsight-wide settings rather than create images. In Guided, and whenever it asks, the dialog reads "Run <process> globally?".

A process in none of these lists (a newer PixInsight, a third-party module) whose parameter ids look like files, folders, output, overwrite, closing windows, servers or commands is asked about at runtime ("it has not been reviewed and has file/output-like parameters: ..."). The dialog names the parameter and value that triggered a rule. If a run cannot be checked, PI Copilot asks rather than running it.

## Increment 5 — Scripts (`run_pjsr`, off by default)

- **Off unless you turn it on:** ⚙ → **Allow scripts** (saved as `PICopilot/RunPjsrEnabled`, default off). When off, and always in **Advisor**, the model is not offered the tool at all.
- **You see every script, every time, in every mode.** Before anything runs, a dialog shows the purpose, the image it is about, and the **whole** script. The default button is **Don't run** (Return and Esc both decline). Nothing skips this dialog.
- **Syntax is checked first, without running anything.** A script that doesn't parse goes back to the model with the error and you are never asked. PixInsight's script engine reports no line number for syntax errors, so none is given; runtime errors are reported at the line in the model's own script.
- **What it can do: anything a PixInsight script can.** A script runs with full access to PixInsight and your files. **Once it starts it cannot be stopped** — if a script never finishes (an endless loop), PixInsight hangs and must be closed. That is why the dialog shows the whole script: read it before you press **Run script**.
- **An approved script can also affect what runs after it** in the same PixInsight session (for example by redefining JavaScript globals that later scripts, including PI Copilot's own script wrappers, rely on) — it has full access. Restart PixInsight if a script did something you don't trust.
- **What you read is what runs:** scripts containing invisible or text-reordering characters (every Unicode format character — zero-width characters, bidi controls, BOM, soft hyphen — plus the invisible characters JavaScript allows inside names: Hangul fillers, variation selectors, the combining grapheme joiner and the two Khmer invisible vowels), control characters, or characters JavaScript treats as line breaks (U+2028/U+2029, a lone carriage return) are refused before you are asked; the model is told to write such characters as `\uXXXX` escapes. Windows line ends (CR LF) are accepted and shown and run as plain line feeds. The purpose line is limited to 300 characters.
- **Undo:** pixel changes are undoable only when the script wraps them in `view.beginProcess(UndoFlag.PixelData)` … `view.endProcess()` or runs process instances; the model is told to do so, but check the script. If a script fails part-way, whatever it changed before the error stays changed.
- **Limits:** scripts up to 20,000 characters; the model gets back the returned value (JSON, up to 4,000 characters), error text up to 2,000 characters, and the last 8,000 characters of console output.
- The script text reaches PixInsight only as a quoted data string, never spliced into code, so text inside it cannot escape the syntax check and run early (self-tested against quote, backslash, comment, `</script>`, NUL/control, U+2028/2029, surrogate and string-escape breakout attempts).
- The model is told never to use a script to run a process the policy refused. That is an instruction, not a guarantee — which is why every script is shown to you in full before it runs.
- The approval dialog itself cannot be exercised by the headless self-test (a modal cannot run there); everything behind it is. It is verified by hand on the released build.

## 0.1.0.4 — UTF-8 wire fix

- Any chat turn containing non-ASCII text (e.g. a model reply with "—" or "→" re-sent as history) failed with `Error 400: ... not valid UTF-8: surrogates not allowed`. Cause: `NetworkTransfer::POST(const String&)` is transmitted by the PI core one byte per UTF-16 code unit (low 8 bits). The body is now widened byte-for-byte from UTF-8 (`PostBytes`), and all outbound text uses our own surrogate-aware `U8()` because PCL's `String::ToUTF8` mis-encodes astral characters (📷 → 📽).
- This relies on undocumented PI-core transmit behaviour, proven by a loopback echo server that captures the wire bytes (self-test Section 8) plus a live two-turn check. **Re-run `bash test/run-selftest.sh` on every PixInsight upgrade** — if the core ever starts UTF-8-encoding POST bodies itself, Section 8 fails instead of users silently getting double-encoded text.

## Verified

**2026-09-20** — self-test PASS on built module:

```
{"evalResult":3,"evalOk":true,"processInstanceValid":true,"ok":true}PASS: EvaluateScript==3 and ProcessInstance valid
```

Full harness: signs module, loads headlessly under `PixInsight --automation-mode`, executes self-test, and exits with no interactive UI required.

**2026-09-23** — headless self-test PASS (Settings round-trip, worker-thread 401, cancel + deadline on a stalled connection, `</raw>` escaping). GUI chat: verified 2026-09-24 by the user on the released 0.1.0.2 installed from the repository URL (⚙ key entry → "say hello" → reply rendered in the panel).

**2026-09-24** — headless self-test PASS incl. real text chat and real vision round-trip (synthetic red square sent as the image only, no context → model answered "Red"). GUI: pending user verification (0.1.0.3 via repository pull).

**2026-09-24** — 0.1.1.0 headless self-test PASS incl. live agent run (real model called `apply_process PixelMath {"expression":"$T*0.5"}` → median ratio 0.5), text, two-turn and vision ('Red') checks. GUI: pending user verification (0.1.1.0 via repository pull).

**2026-09-25** — 0.1.2.0 headless self-test PASS with every live check required (`PICOPILOT_REQUIRE_LIVE=1`): text, two-turn, vision ('Red'), streamed agent run (ratio 0.5), cache read 3259 tokens, Opus 5.5 edited history accepted (thinking_dropped), live model switch Opus 5.5 ↔ Sonnet 5, and a real GraXpert background extraction with `appPath` pinned by PI Copilot (gradient 0.235 → 0.003, 4.0 s). GUI: pending user verification (0.1.2.0 via repository pull).
