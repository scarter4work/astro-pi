# PJSR MCP — V8 Runtime Update Design

**Date:** 2026-05-13
**Trigger:** PixInsight 1.9.4 Lockhart introduced a new V8-based JavaScript runtime replacing the legacy SpiderMonkey 24 engine. The official porting guide (Juan Conejero, Mar 2026) lists ~30+ migration points the MCP must understand to remain useful.

## Goals

1. PJSR MCP fully supports PixInsight 1.9.4 V8 runtime (ECMAScript 2025) with V8 as the primary target.
2. **All knowledge about new classes, deprecated APIs, and lint rules lives in `schemas/*.json`** — JavaScript code in `src/` only interprets the schema.
3. The validator detects a script's target engine from its `#engine` directive (or absence) and applies engine-appropriate diagnostics.
4. Future runtime changes (1.9.5+ deprecations, additional classes) are JSON edits, not code edits.

## Non-Goals

- Rewriting the lexer/parser. The current lexer already handles ES2025 keywords (`class`, `extends`, `super`); only minimal extension is needed for `#engine` directive recognition.
- Building a full ES2025 AST. We only need enough analysis to drive symbol-existence and pattern-match rules.
- Removing SpiderMonkey support entirely. PixInsight still ships both engines in 1.9.4; we mark SM as legacy but still answer questions about it.

## Architecture: Pure Schema-Driven Validation

```
pjsr-core.json (extended)              src/pjsr-parser.js
┌──────────────────────────┐           ┌─────────────────────┐
│ engines: {v8, sm}        │           │ Lexer (#engine,     │
│ classes: { ...           │  reads    │  #field tokens)     │
│   FMath: {availableIn},  │ ────────► │ Analyzer (detects   │
│   VectorGraphics:        │           │  target engine)     │
│     {removedIn,replacedBy│           │ RuleEngine          │
│ constants: { ...         │           │  (interprets schema │
│ lintRules: [             │           │   lintRules)        │
│   {match: {type, ...},   │           └─────────────────────┘
│    severity, message}]   │
└──────────────────────────┘
```

The `RuleEngine` is generic: it knows how to interpret 5 `match` types and combines them with the analyzer's detected engine context to emit diagnostics. No rule logic is hardcoded.

## Schema Conventions

### Top-level additions to `pjsr-core.json`

```json
{
  "version": "2.0.0",
  "description": "PJSR API schema for PixInsight 1.9.4+ V8 runtime (with legacy SpiderMonkey support)",
  "engines": {
    "v8": {
      "since": "1.9.4",
      "default": false,
      "ecmascript": "2025",
      "directiveValues": ["v8", "v8-new", "v8-default", "v8-private"],
      "description": "V8-based runtime introduced in PixInsight 1.9.4 Lockhart"
    },
    "sm": {
      "since": "legacy",
      "default": true,
      "deprecated": true,
      "ecmascript": "ES5",
      "directiveValues": ["sm"],
      "description": "Legacy SpiderMonkey 24 engine. Default for backward compatibility; scheduled for removal."
    }
  },
  "lintRules": [ ... ]
}
```

### Per-symbol lifecycle metadata

Added to existing classes/methods/constants and used on new V8-only entries:

- `availableIn: ["v8"]` — symbol exists only on this engine (used on FMath, Stat, ImageIterator, PSF, BRQuadTree, XML*, System, CoreApplication, Runtime, all new constant classes).
- `deprecatedIn: "v8"` — exists but warns on this engine (used on `gc`, `jsGarbageCollect`, old constant names, Math statistical extensions).
- `removedIn: "v8"` — error if used on this engine (used on VectorGraphics, ImageStatistics, Image.forEachSample, etc.).
- `replacedBy: "..."` — suggestion text appended to any diagnostic.
- `since: "1.9.4"` — first version introducing the symbol.

### Lint rule schema

```json
{
  "id": "v8-no-base-inheritance",
  "appliesToEngines": ["v8"],
  "severity": "error",
  "match": {
    "type": "code-pattern",
    "regex": "this\\.__base__\\s*="
  },
  "message": "__base__ inheritance does not work in V8. Use `class X extends Y { constructor(...) { super(); ... } }`.",
  "docUrl": "https://pixinsight.net/dev/.../page/replacing-constructor-functions-with-classes.4/"
}
```

### Five `match` types the RuleEngine understands

1. **`code-pattern`** — regex over raw source. Used for `__base__`, `.prototype.CONSTANT`, etc.
2. **`function-call`** — calls to a named function (e.g. `gc()`).
3. **`member-access`** — `Class.member` references (e.g. `Math.median`, `ImageStatistics.*`).
4. **`include`** — `#include <path>` matching a prefix (e.g. `pjsr/`).
5. **`symbol-use`** — any identifier matching a name pattern (e.g. `^StdIcon_`, `^Cipher_`).

Each rule's diagnostic message is augmented with the matched symbol's `replacedBy` (if available) so users see the rewrite.

## Inventory of Changes

### New V8-only classes (added to `pjsr-core.json` with `availableIn: ["v8"]`)

- **`FMath`** — fast Math alternative with WebAssembly backing. ~40+ methods mirroring `Math.*` plus PJSR extensions (`mtf`, `mad`, etc.).
- **`Stat`** — statistical calculations (median, MAD, stdDev, etc.). Static methods replacing deprecated `Math.*` PJSR extensions.
- **`ImageIterator`** — typed-array-backed pixel access. Replaces `Image.forEachSample/Pixel`. Methods: `toReal`, `toSample`. Indexable as `i[y][x]`.
- **`PSF`** — Levenberg-Marquardt point-spread-function fitting. Static `PSF.fitStars(image, stars, function)`.
- **`StarDetector`** — promoted from `pjsr-extended.json` JavaScript implementation to core C++ class. Update `availableIn` and remove `#include <pjsr/StarDetector.jsh>` requirement.
- **`BRQuadTree`** — bucket-region quadtree. Replaces `#include <pjsr/BRQuadTree.jsh>`.
- **`XMLDocument`**, **`XMLDeclaration`**, **`XMLComment`**, **`XMLElement`**, **`XMLText`** — full XML support.
- **`System`** — host machine introspection (`physicalMemoryStatus()`, etc.).
- **`CoreApplication`** — replaces many `Global.*` extensions. Includes `ensureMinimumVersion(major, minor, patch)`.
- **`Runtime`** — runtime introspection.
- **`ElapsedTime`** — used in V8 example scripts (already exists; just verify metadata).
- **New constant *classes*** (replacing flat `*_CONSTANT` names): `StdIcon`, `CipherAlgorithm`, `CompressionAlgorithm`, `FileFlag`, `GradientSpreadMode`, `InterpolationAlgorithm`, `KeyCode`, `MorphologicalOp`, `RadialBasisFunction`, `ReadTextOption`, `PixelSampleType`, `TextAlignment`, `PropertyAttribute`, `PropertyType`, `SeekMode`, `DataType`, `UndoFlag`, `PSFunction`, `StdButton`, `StdCursor`, `StdDialogCode`, `FrameStyle`, `FocusStyle`.

### Removed under V8 (`removedIn: "v8"` + `replacedBy`)

- `VectorGraphics` → `Graphics`
- `ImageStatistics` → `Image.median/MAD/stdDev/rangeClippingEnabled/...`
- `Image.forEachSample` / `forEachMutableSample` / `forEachPixel` / `forEachMutablePixel` → `ImageIterator`
- `Control.showAlias` / `hideAlias` → (remove; layout handles it)
- `FileFormat.formatSpecificData` / `usesFormatSpecificData` / `validateFormatSpecificData` / `disposeFormatSpecificData`

### Deprecated under V8 (`deprecatedIn: "v8"`)

- `gc()` → no replacement; use explicit `Image.free()`, `Bitmap.clear()`.
- `jsGarbageCollect()` → same.
- All flat constants `StdIcon_*`, `Cipher_*`, `Compression_*`, `FileType_*`, `File_Attribute_*`, `FilePermission_*`, `GradientSpread_*`, `Interpolation_*`, `Key_*`, `MorphOp_*`, `RBFType_*`, `ReadTextOptions_*`, `SampleType_*`, `TextAlign_*` → static class properties.
- Math statistical extensions (`Math.median`, `Math.MAD`, `Math.mtf`, etc.) → `Stat.*` / `FMath.*`.
- Most `Global.*` extensions except `cerr`, `cerrln`, `cflush`, `cout`, `coutln`, `format`.

### Behavior shifts (documented, low validator value)

- `Compression.compress/uncompress` return shape (array of arrays → array of objects).
- `EphemerisFile.objects` / `visibleObjects` similar.
- `FileFormatInstance.open` returns `[]` vs `null`.
- View null-returns (`previewById`, `viewById`, `propertyAttributes`, `propertyType`, `window`, `image`, `properties`).

Documented in schema descriptions; not directly enforced by lint (would need flow analysis).

### Concrete lint rules (initial set)

| ID | Engines | Severity | Trigger |
|---|---|---|---|
| `v8-no-base-inheritance` | v8 | error | `this.__base__ = ...` |
| `v8-no-prototype-process-constants` | v8 | error | `[A-Z]\w+\.prototype\.[A-Z]` |
| `v8-no-prototype-class-assign` | v8 | error | `\w+\.prototype\s*=\s*new\s+\w+` |
| `v8-deprecated-gc` | v8 | warning | call to `gc` |
| `v8-deprecated-jsGarbageCollect` | v8 | warning | call to `jsGarbageCollect` |
| `v8-removed-VectorGraphics` | v8 | error | identifier `VectorGraphics` |
| `v8-removed-ImageStatistics` | v8 | error | identifier `ImageStatistics` |
| `v8-removed-forEachSample` | v8 | error | member access `.forEachSample/forEachMutableSample/forEachPixel/forEachMutablePixel` |
| `v8-removed-showAlias` | v8 | error | member access `.showAlias/.hideAlias` |
| `v8-deprecated-pjsr-include` | v8 | warning | `#include <pjsr/...>` |
| `v8-deprecated-flat-constants` | v8 | warning | symbol matching `^(StdIcon|Cipher|Compression|FileType|File_Attribute|FilePermission|GradientSpread|Interpolation|Key|MorphOp|RBFType|ReadTextOptions|SampleType|TextAlign)_` |
| `v8-deprecated-math-stats` | v8 | warning | member access `Math.median/MAD/avgDev/mtf/...` |
| `v8-missing-engine-directive` | sm-default | hint | no `#engine` directive present (suggests opting into V8) |
| `v8-recommend-ensureMinimumVersion` | v8 | hint | `#engine v8*` present but no `CoreApplication.ensureMinimumVersion` call |

The single `hint`-level rule for "no #engine" only fires when the user requests *V8-aware* diagnostics on a script that's defaulting to SM — useful for migration.

## Parser/Validator Changes

### Lexer (`src/pjsr-parser.js`)

- Add `'#engine'` to `PREPROCESSOR_DIRECTIVES`.
- No private-field tokenization needed: the existing preprocessor-line skip (`# at column 1`) already handles `#engine`, but private class fields (`#bar`) appear mid-line and would currently be misread as preprocessor directives. **Fix:** preprocessor directive detection already requires `startColumn === 1 || prev char === '\n'`. Verify mid-line `#bar` (after `this.` or `Foo.`) is not at column 1 and won't be misread. Confirmed by reading the lexer — mid-line `#` won't be at column 1. No change needed.

### Analyzer

- Extend `analyzePreprocessor` to capture `#engine <value>` into `analysis.engine` (one of `v8`, `sm`, or `undefined` for "not specified — defaults to sm").
- Replace `runDiagnostics()` with `runDiagnostics()` that loads `pjsrSchema.lintRules` and applies each rule whose `appliesToEngines` includes the resolved engine (`v8` or `sm`).
- New `RuleEngine` module (kept inside `pjsr-parser.js` for cohesion, ~150 LOC) with one method per `match.type`.

### Symbol metadata diagnostics

When a script uses an identifier flagged with `removedIn` matching the active engine, emit an error. When it uses one with `deprecatedIn`, emit a warning. Combined with `replacedBy` to form a single useful diagnostic.

## Templates Update

Add to schema's `templates` section:

- **`scriptTemplateV8`** — V8-flavored standard script (uses `#engine v8`, `CoreApplication.ensureMinimumVersion`, no `jsAutoGC`, no `#include <pjsr/...>`).
- **`dialogTemplateV8`** — V8 dialog using `class FooDialog extends Dialog`.
- **`imageProcessingV8`** — uses `ImageIterator`, explicit `Image.free()`.
- **`starDetectionV8`** — uses new `StarDetector` core class.

Keep existing templates as `scriptTemplate`/`dialogTemplate`/etc. for SM users.

## README Update

- Remove the "based on SpiderMonkey 24 (ECMA 262-5)" line; replace with explanation that PJSR now ships V8 (1.9.4+) with SM as legacy.
- Add a section "Engine-aware analysis" describing the `#engine` directive.
- Add to "Core Classes" list: FMath, Stat, ImageIterator, PSF, BRQuadTree, XMLDocument family, System, CoreApplication, Runtime.

## Testing

`src/test.js` currently has ~225 lines of tests. Add:

- Test: `#engine v8` parsed correctly into `analysis.engine`.
- Test: `__base__` pattern emits error under v8, no warning under sm.
- Test: `gc()` call emits warning under v8.
- Test: `StdIcon_NoIcon` flagged under v8 with replacement hint.
- Test: `VectorGraphics` flagged as removed under v8.
- Test: `FMath` symbol resolves as PJSR class.
- Test: existing SM-mode tests continue to pass.

Aim for ≥6 new test cases, keep total run time under 2 seconds.

## File Changes Summary

- **Modify** `schemas/pjsr-core.json` — add `engines`, `lintRules`, V8 classes, lifecycle metadata. Expected growth ~1500 lines.
- **Modify** `schemas/pjsr-extended.json` — remove StarDetector duplicate definition (now in core), update any obsolete entries.
- **Modify** `src/pjsr-parser.js` — add `#engine` recognition, RuleEngine, schema-driven diagnostics. Net change ~+150 LOC (replace existing `runDiagnostics`).
- **Modify** `src/test.js` — add 6+ test cases.
- **Modify** `README.md` — update engine description, class list.
- **Add** `docs/superpowers/specs/2026-05-13-pjsr-v8-update-design.md` — this document.

No new source files. No npm dependencies added.

## Build/Verification Sequence

1. JSON schema files parse (`node -e "JSON.parse(require('fs').readFileSync('schemas/pjsr-core.json'))"`).
2. `npm test` passes all old + new cases.
3. Manual smoke test: feed `examples/v8-mtf-iterator.js` (the V8 example from the porting guide) through `pjsr_analyze` — expect zero errors. Feed an SM-style script with `__base__` and `#engine v8` — expect the error.
4. Git commit, push to origin/main.
