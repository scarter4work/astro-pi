# PI Copilot — Increment 1: Module Skeleton + Execution Proof — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce a buildable, signable, PI-loadable `PICopilot-pxm.so` with a dockable (empty) panel, and prove at runtime — in code we keep — that a PCL module can execute PJSR via `MetaModule::EvaluateScript` and construct a native `ProcessInstance`.

**Architecture:** A standard PCL module (`MetaModule` + `MetaProcess` + `ProcessImplementation` + `ProcessInterface`) mirroring the existing NukeX module layout at `modules/nukex/`. The associated process is degenerate for now; its `ExecuteGlobal()` runs a self-test (EvaluateScript + ProcessInstance checks) and writes a JSON result file, which a headless harness (`PixInsight.sh -m=<so> -r=selftest.js`) asserts on. The interface is an empty dockable panel.

**Tech Stack:** C++17, PCL SDK (`~/PCL`), CMake ≥3.24, g++, PixInsight 1.9.5 headless (`--automation-mode`), module signing via `PixInsight.sh --sign-module-file`.

**Spec:** `docs/superpowers/specs/2026-09-20-pi-copilot-native-pcl-design.md` (see §2, §4, §12 increment 1).

## Global Constraints

- **Module ID string is `"PICopilot"`** — stable forever; installs update by ID (mirror the NukeX rule).
- **C++17**, compile flags exactly as NukeX: `-fPIC -fvisibility=hidden -fvisibility-inlines-hidden`, defines `__PCL_LINUX __PCL_BUILDING_MODULE _REENTRANT`.
- **Module naming:** output is `PICopilot-pxm.so`; signing produces `PICopilot-pxm.xsgn`.
- **PCL SDK:** `PCLDIR=$HOME/PCL`; link `-lPCL-pxi -llz4-pxi -lzstd-pxi -lzlib-pxi -lRFC6234-pxi -llcms-pxi -lcminpack-pxi`.
- **`EvaluateScript` is root-thread only** — it throws if called from a running `Thread`. In increment 1 it is called from `ExecuteGlobal()`, which runs on the root thread. Do not move it onto a worker thread.
- **Never `make install`.** Dev testing loads the built module with `-m=<path>`; the shipped path is repo-pull only (later increment).
- **Signing:** keys `/home/scarter4work/projects/keys/scarter4work_keys.xssk`, password from `/tmp/.pi_codesign_pass` (0600, never committed). Command form: `PixInsight.sh --sign-module-file=<so> --xssk-file=<keys> --xssk-password="<pass>"`.
- **Version starts at `0.1.0.1`** (`PICOPILOT_MODULE_VERSION_{MAJOR,MINOR,REVISION,BUILD}`).

---

## File Structure

```
modules/pi-copilot/
  CMakeLists.txt                       # root: project, C++17, flags/defines, options, add_subdirectory(src/module)
  cmake/PCLConfig.cmake                # PCL SDK discovery — copied verbatim from modules/nukex/cmake/PCLConfig.cmake
  src/module/CMakeLists.txt            # module target -> PICopilot-pxm.so, links PCL libs
  src/module/PICopilotVersion.h        # version macros + version-string helper
  src/module/PICopilotModule.h         # class PICopilotModule : public MetaModule
  src/module/PICopilotModule.cpp       # MetaModule impl + InstallPixInsightModule + global ThePICopilotModule
  src/module/PICopilotProcess.h        # class PICopilotProcess : public MetaProcess  (id "PICopilot")
  src/module/PICopilotProcess.cpp
  src/module/PICopilotInstance.h       # class PICopilotInstance : public ProcessImplementation
  src/module/PICopilotInstance.cpp     # ExecuteGlobal() -> RunSelfTest(); CanExecuteGlobal()->true
  src/module/PICopilotInterface.h      # class PICopilotInterface : public ProcessInterface (dockable, empty)
  src/module/PICopilotInterface.cpp    # Features()->InterfaceFeature::None
  src/module/PICopilotSelfTest.h       # RunSelfTest() free function decl (in namespace pcl)
  src/module/PICopilotSelfTest.cpp     # EvaluateScript + ProcessInstance checks -> JSON result file
  test/selftest.js                     # PJSR harness: new PICopilot; P.executeGlobal();
  test/run-selftest.sh                 # build -> sign -> load headless -> assert result file
  README.md                            # one-paragraph: what this module is, how to build/test
```

`cmake/PCLConfig.cmake` is copied verbatim from `modules/nukex/cmake/PCLConfig.cmake` (same SDK, same discovery logic). Do not hand-write it.

---

## Task 1: Buildable, signable, PI-loadable empty module

Proves PCL linkage, the module output name, signing, and the `-m=` headless load path — the foundational risks — before any process/interface exists.

**Files:**
- Create: `modules/pi-copilot/CMakeLists.txt`
- Create: `modules/pi-copilot/cmake/PCLConfig.cmake` (copy of `modules/nukex/cmake/PCLConfig.cmake`)
- Create: `modules/pi-copilot/src/module/CMakeLists.txt`
- Create: `modules/pi-copilot/src/module/PICopilotVersion.h`
- Create: `modules/pi-copilot/src/module/PICopilotModule.h`
- Create: `modules/pi-copilot/src/module/PICopilotModule.cpp`
- Create: `modules/pi-copilot/test/run-load.sh`

**Interfaces:**
- Consumes: PCL SDK at `$HOME/PCL`; NukeX module files as authoritative boilerplate templates.
- Produces: `build/src/module/PICopilot-pxm.so`; global `pcl::PICopilotModule* pcl::ThePICopilotModule`; `PICopilotModule` (a `MetaModule` whose `Version()/Description()/Company()/etc.` follow the NukeX pattern).

- [ ] **Step 1: Write the failing test (headless load harness)**

Create `modules/pi-copilot/test/run-load.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
PI=/opt/PixInsight/bin/PixInsight.sh
KEYS=/home/scarter4work/projects/keys/scarter4work_keys.xssk
PASS="$(cat /tmp/.pi_codesign_pass)"
SO="$ROOT/build/src/module/PICopilot-pxm.so"

[ -f "$SO" ] || { echo "FAIL: module not built at $SO"; exit 1; }
"$PI" --sign-module-file="$SO" --xssk-file="$KEYS" --xssk-password="$PASS"
[ -f "${SO%.so}.xsgn" ] || { echo "FAIL: signing produced no .xsgn"; exit 1; }

# Load the module headlessly; --force-exit returns 0 only if startup+load succeeded.
"$PI" -n --automation-mode --no-startup-scripts -m="$SO" --force-exit
echo "PASS: module built, signed, and loaded"
```

- [ ] **Step 2: Run it to verify it fails**

Run: `bash modules/pi-copilot/test/run-load.sh`
Expected: FAIL — "module not built at …/PICopilot-pxm.so" (nothing built yet).

- [ ] **Step 3: Write the version header**

`modules/pi-copilot/src/module/PICopilotVersion.h` (model on `modules/nukex/src/module/NukeXVersion.h`):

```cpp
#ifndef PICopilotVersion_h
#define PICopilotVersion_h

#define PICOPILOT_MODULE_VERSION_MAJOR     0
#define PICOPILOT_MODULE_VERSION_MINOR     1
#define PICOPILOT_MODULE_VERSION_REVISION  0
#define PICOPILOT_MODULE_VERSION_BUILD     1

#define PICOPILOT_STR_(x)  #x
#define PICOPILOT_STR(x)   PICOPILOT_STR_(x)

#define PICOPILOT_VERSION_STRING \
    PICOPILOT_STR(PICOPILOT_MODULE_VERSION_MAJOR) "." \
    PICOPILOT_STR(PICOPILOT_MODULE_VERSION_MINOR) "." \
    PICOPILOT_STR(PICOPILOT_MODULE_VERSION_REVISION) "." \
    PICOPILOT_STR(PICOPILOT_MODULE_VERSION_BUILD)

#endif
```

- [ ] **Step 4: Write the MetaModule class**

`PICopilotModule.h` and `PICopilotModule.cpp` — mirror `modules/nukex/src/module/NukeXModule.{h,cpp}` exactly for the set of required `MetaModule` virtuals (`Version()`, `Description()`, `Company()`, `Author()`, `Copyright()`, `TradeMarks()`, `OriginalFileName()`, `GetReleaseDate()`), substituting PICopilot values. The `.cpp` ends with:

```cpp
namespace pcl {
PICopilotModule* ThePICopilotModule = nullptr;
} // namespace pcl

PCL_MODULE_EXPORT int InstallPixInsightModule( int mode )
{
   new pcl::PICopilotModule;
   // No Process/Interface yet — added in Task 2.
   return 0;
}
```

`Version()` uses `PCL_MODULE_VERSION( PICOPILOT_MODULE_VERSION_MAJOR, PICOPILOT_MODULE_VERSION_MINOR, PICOPILOT_MODULE_VERSION_REVISION, PICOPILOT_MODULE_VERSION_BUILD, eng )`. Set the constructor to assign `ThePICopilotModule = this;` if NukeX does so; otherwise assign in `InstallPixInsightModule` before returning.

- [ ] **Step 5: Write CMake (root + module)**

`modules/pi-copilot/CMakeLists.txt` — copy the compile-flags/defines/options block from `modules/nukex/CMakeLists.txt` (lines 1–58), set `project(PICopilot VERSION 0.1.0 LANGUAGES CXX)`, keep `option(PICOPILOT_BUILD_MODULE ... OFF)` guarding `include(PCLConfig)`, then `add_subdirectory(src/module)`. Do NOT copy NukeX's nlohmann/json or Eigen blocks in this increment (not needed until increment 2+).

`modules/pi-copilot/src/module/CMakeLists.txt` — model on `modules/nukex/src/module/CMakeLists.txt`: define target `PICopilot` producing output name `PICopilot-pxm` with no `lib` prefix and `.so` suffix, sources `PICopilotModule.cpp`, link the PCL static libs listed in Global Constraints, link flags `-shared -Wl,-z,defs -L${PCLDIR}/lib/x64`.

Set output naming explicitly:
```cmake
set_target_properties(PICopilot PROPERTIES PREFIX "" OUTPUT_NAME "PICopilot-pxm" SUFFIX ".so")
```

- [ ] **Step 6: Build**

Run:
```bash
cd modules/pi-copilot && cmake -B build -DPCLDIR=$HOME/PCL -DPICOPILOT_BUILD_MODULE=ON && cmake --build build -j$(nproc)
```
Expected: `build/src/module/PICopilot-pxm.so` exists.

- [ ] **Step 7: Run the load harness to verify it passes**

Run: `bash modules/pi-copilot/test/run-load.sh`
Expected: `PASS: module built, signed, and loaded` and exit 0.

- [ ] **Step 8: Commit**

```bash
git add modules/pi-copilot/CMakeLists.txt modules/pi-copilot/cmake modules/pi-copilot/src/module/CMakeLists.txt \
        modules/pi-copilot/src/module/PICopilotVersion.h modules/pi-copilot/src/module/PICopilotModule.h \
        modules/pi-copilot/src/module/PICopilotModule.cpp modules/pi-copilot/test/run-load.sh
git commit -m "feat(pi-copilot): buildable, signable, PI-loadable native module skeleton"
```

---

## Task 2: PICopilot process + dockable empty panel

Adds the registered `"PICopilot"` process and its dockable `ProcessInterface`, proving process/interface registration and the docking primitive.

**Files:**
- Create: `modules/pi-copilot/src/module/PICopilotProcess.h` / `.cpp`
- Create: `modules/pi-copilot/src/module/PICopilotInstance.h` / `.cpp`
- Create: `modules/pi-copilot/src/module/PICopilotInterface.h` / `.cpp`
- Modify: `modules/pi-copilot/src/module/PICopilotModule.cpp` (register process + interface)
- Modify: `modules/pi-copilot/src/module/CMakeLists.txt` (add the three new .cpp files)
- Create: `modules/pi-copilot/test/selftest.js`

**Interfaces:**
- Consumes: `ThePICopilotModule` from Task 1.
- Produces: `PICopilotProcess` (`Id()=="PICopilot"`, `CanExecuteGlobal()->true`); `PICopilotInstance : ProcessImplementation` with `bool ExecuteGlobal() override`; `PICopilotInterface : ProcessInterface` with `Features()->InterfaceFeature::None`; globals `ThePICopilotProcess`, `ThePICopilotInterface`.

- [ ] **Step 1: Write the failing test (process-exists harness)**

Create `modules/pi-copilot/test/selftest.js`:

```javascript
// Increment-1 harness. Writes a JSON verdict; console output never reaches
// stdout under --automation-mode.
var RESULT = "/tmp/.picopilot_selftest.json";
function write(o){ var f=new File; f.createForWriting(RESULT); f.outTextLn(JSON.stringify(o)); f.close(); }
try {
   var P = new PICopilot;          // fails here if the process id isn't registered
   var ok = P.executeGlobal();     // Task 3 makes this run the self-test
   write({ processConstructed: true, executeGlobalReturned: ok });
} catch (e) {
   write({ error: String(e) });
}
```

Extend `run-selftest.sh` (create it now as a copy of `run-load.sh` with the load line replaced by):
```bash
rm -f /tmp/.picopilot_selftest.json
"$PI" -n --automation-mode --no-startup-scripts -m="$SO" -r="$HERE/selftest.js" --force-exit
R=/tmp/.picopilot_selftest.json
[ -f "$R" ] || { echo "FAIL: no result file"; exit 1; }
cat "$R"
python3 -c "import json,sys; d=json.load(open('$R')); sys.exit(0 if d.get('processConstructed') else 1)" \
   || { echo "FAIL: PICopilot process not constructible"; exit 1; }
echo "PASS: process registered and constructible"
```

- [ ] **Step 2: Run it to verify it fails**

Run: `bash modules/pi-copilot/test/run-selftest.sh`
Expected: FAIL — result JSON contains an `error` (`new PICopilot` undefined; process not registered yet).

- [ ] **Step 3: Write the process, instance, and interface classes**

Mirror the class shapes of `modules/nukex/src/module/NukeX{Process,Instance,Interface}.{h,cpp}`, substituting PICopilot values. Concretely:

`PICopilotProcess` (`MetaProcess`): `Id() -> "PICopilot"`; `Category() -> "Utilities"` (or ""); `Version() -> 0x100`; `Description()` a one-line HTML blurb; `IconImageSVG()` optional; `NeedsValidation()->false`; `CanProcessGlobal()->true`; `CreateProcessInterface()` returns/launches `ThePICopilotInterface`; `DefaultInterface()` returns `ThePICopilotInterface`; `Create()`/`Clone()` return `new PICopilotInstance(...)`.

`PICopilotInstance` (`ProcessImplementation`):
```cpp
bool CanExecuteGlobal( String& /*whyNot*/ ) const override { return true; }
bool ExecuteGlobal() override { return true; }   // Task 3 replaces the body with RunSelfTest()
void Assign( const ProcessImplementation& ) override {}
```

`PICopilotInterface` (`ProcessInterface`):
```cpp
IsoString Id() const override { return "PICopilot"; }
MetaProcess* Process() const override { return ThePICopilotProcess; }
InterfaceFeatures Features() const override { return InterfaceFeature::None; }
bool Launch( const MetaProcess&, const ProcessImplementation*, bool& dynamic, unsigned& ) override
{ dynamic = false; if ( GUI == nullptr ) { /* empty control constructed in Task/increment later */ } return true; }
```
For increment 1 the panel body is empty (a single label "PI Copilot" is acceptable); the real chat UI lands in increment 2. Being a `ProcessInterface` is what makes it dockable.

- [ ] **Step 4: Register process + interface in the module**

In `PICopilotModule.cpp`, update `InstallPixInsightModule`:
```cpp
PCL_MODULE_EXPORT int InstallPixInsightModule( int mode )
{
   new pcl::PICopilotModule;
   if ( mode == pcl::InstallMode::FullInstall )
   {
      new pcl::PICopilotProcess;
      new pcl::PICopilotInterface;
   }
   return 0;
}
```
Add the three new `.cpp` files to `src/module/CMakeLists.txt`.

- [ ] **Step 5: Build**

Run: `cd modules/pi-copilot && cmake --build build -j$(nproc)`
Expected: rebuild succeeds; `PICopilot-pxm.so` regenerated.

- [ ] **Step 6: Run the harness to verify it passes**

Run: `bash modules/pi-copilot/test/run-selftest.sh`
Expected: result JSON has `processConstructed: true`; script prints `PASS: process registered and constructible`.

- [ ] **Step 7: Commit**

```bash
git add modules/pi-copilot/src/module/PICopilotProcess.* modules/pi-copilot/src/module/PICopilotInstance.* \
        modules/pi-copilot/src/module/PICopilotInterface.* modules/pi-copilot/src/module/PICopilotModule.cpp \
        modules/pi-copilot/src/module/CMakeLists.txt modules/pi-copilot/test/selftest.js modules/pi-copilot/test/run-selftest.sh
git commit -m "feat(pi-copilot): register PICopilot process + dockable empty panel"
```

---

## Task 3: Execution proof — EvaluateScript + ProcessInstance

Proves the two hybrid execution paths from C++: PJSR via `MetaModule::EvaluateScript` and native `ProcessInstance` construction against the process registry.

**Files:**
- Create: `modules/pi-copilot/src/module/PICopilotSelfTest.h` / `.cpp`
- Modify: `modules/pi-copilot/src/module/PICopilotInstance.cpp` (`ExecuteGlobal()` calls `RunSelfTest()`)
- Modify: `modules/pi-copilot/src/module/CMakeLists.txt` (add `PICopilotSelfTest.cpp`)
- Modify: `modules/pi-copilot/test/selftest.js` (assert the self-test values)
- Modify: `modules/pi-copilot/test/run-selftest.sh` (assert PASS on the values)

**Interfaces:**
- Consumes: `ThePICopilotModule` (has `EvaluateScript`); `PICopilotInstance::ExecuteGlobal`.
- Produces: `bool pcl::RunSelfTest( String& jsonOut )` — populates `jsonOut` with `{evalResult, evalOk, processInstanceValid, ok}` and returns `ok`.

- [ ] **Step 1: Write the failing assertions**

Replace the body of `modules/pi-copilot/test/selftest.js` verdict handling to assert values, and update `run-selftest.sh`'s python check to:
```bash
python3 -c "import json,sys; d=json.load(open('$R')); sys.exit(0 if (d.get('evalOk') and d.get('evalResult')==3 and d.get('processInstanceValid')) else 1)" \
   || { echo "FAIL: self-test did not prove both execution paths"; exit 1; }
echo "PASS: EvaluateScript==3 and ProcessInstance valid"
```
`selftest.js` stays as in Task 2 (it calls `P.executeGlobal()`); the C++ side now writes the richer JSON directly to `/tmp/.picopilot_selftest.json`, so simplify `selftest.js` to just `new PICopilot; P.executeGlobal();` and let C++ own the result file.

- [ ] **Step 2: Run it to verify it fails**

Run: `bash modules/pi-copilot/test/run-selftest.sh`
Expected: FAIL — `ExecuteGlobal()` still returns true without writing eval values; python check exits 1.

- [ ] **Step 3: Implement RunSelfTest**

`PICopilotSelfTest.h`:
```cpp
#ifndef PICopilotSelfTest_h
#define PICopilotSelfTest_h
#include <pcl/String.h>
namespace pcl { bool RunSelfTest( String& jsonOut ); }
#endif
```

`PICopilotSelfTest.cpp`:
```cpp
#include "PICopilotSelfTest.h"
#include "PICopilotModule.h"     // ThePICopilotModule
#include <pcl/Process.h>
#include <pcl/ProcessInstance.h>
#include <pcl/Variant.h>

namespace pcl {

bool RunSelfTest( String& jsonOut )
{
   int  evalResult = -1;
   bool evalOk = false;
   bool piValid = false;

   // Path 1: execute PJSR from C++ and read the result Variant.
   try {
      Variant v = ThePICopilotModule->EvaluateScript( "1+2", "JavaScript" );
      evalResult = int( v.ToInt() );
      evalOk = (evalResult == 3);
   } catch ( ... ) { evalOk = false; }

   // Path 2: construct a native ProcessInstance against the registry.
   try {
      Process P( "PixelMath" );
      ProcessInstance instance( P );
      String whyNot;
      piValid = instance.CanExecuteGlobal( whyNot );
   } catch ( ... ) { piValid = false; }

   bool ok = evalOk && piValid;
   jsonOut = String().Format(
      "{\"evalResult\":%d,\"evalOk\":%s,\"processInstanceValid\":%s,\"ok\":%s}",
      evalResult, evalOk ? "true":"false", piValid ? "true":"false", ok ? "true":"false" );
   return ok;
}

} // namespace pcl
```
(Confirm `Variant::ToInt()` is the correct accessor from `pcl/Variant.h`; if the accessor differs, use the header's integer getter. This is the one API to verify against `/opt/PixInsight/include/pcl/Variant.h` before building.)

- [ ] **Step 4: Wire ExecuteGlobal to write the result file**

In `PICopilotInstance.cpp`:
```cpp
#include "PICopilotSelfTest.h"
#include <pcl/File.h>

bool PICopilotInstance::ExecuteGlobal()
{
   String json;
   bool ok = RunSelfTest( json );
   File f;
   f.CreateForWriting( "/tmp/.picopilot_selftest.json" );
   f.OutTextLn( IsoString( json ) );
   f.Close();
   return ok;
}
```
Add `PICopilotSelfTest.cpp` to `src/module/CMakeLists.txt`.

- [ ] **Step 5: Build**

Run: `cd modules/pi-copilot && cmake --build build -j$(nproc)`
Expected: builds clean.

- [ ] **Step 6: Run the harness to verify it passes**

Run: `bash modules/pi-copilot/test/run-selftest.sh`
Expected: `/tmp/.picopilot_selftest.json` = `{"evalResult":3,"evalOk":true,"processInstanceValid":true,"ok":true}`; prints `PASS: EvaluateScript==3 and ProcessInstance valid`.

- [ ] **Step 7: Commit**

```bash
git add modules/pi-copilot/src/module/PICopilotSelfTest.* modules/pi-copilot/src/module/PICopilotInstance.cpp \
        modules/pi-copilot/src/module/CMakeLists.txt modules/pi-copilot/test/selftest.js modules/pi-copilot/test/run-selftest.sh
git commit -m "feat(pi-copilot): prove EvaluateScript + ProcessInstance execution paths in-module"
```

---

## Task 4: README + green-run record

**Files:**
- Create: `modules/pi-copilot/README.md`

**Interfaces:**
- Consumes: nothing.
- Produces: developer docs.

- [ ] **Step 1: Write README**

`modules/pi-copilot/README.md`: one paragraph stating this is the native PCL PI Copilot module (replaces the retired Node sidecar), the build command (`cmake -B build -DPCLDIR=$HOME/PCL -DPICOPILOT_BUILD_MODULE=ON && cmake --build build -j$(nproc)`), the test command (`bash test/run-selftest.sh`), and a pointer to the spec.

- [ ] **Step 2: Run the full harness once more and paste its output into the README under "Verified"**

Run: `bash modules/pi-copilot/test/run-selftest.sh`
Expected: PASS; copy the JSON line + PASS line into README "Verified" section.

- [ ] **Step 3: Commit**

```bash
git add modules/pi-copilot/README.md
git commit -m "docs(pi-copilot): increment-1 build/test instructions + verified run"
```

---

## Self-Review

**Spec coverage (increment 1 rows of §12):** module skeleton → Task 1; empty dockable panel → Task 2; `EvaluateScript("1+2")==3` → Task 3; trivial `apply_process` proof → Task 3 (ProcessInstance construction + `CanExecuteGlobal`); "in kept code" → self-test ships in-module (behind `ExecuteGlobal`, harmless in production). Threading constraint (root-thread `EvaluateScript`) → honored (called from `ExecuteGlobal`). Signing/loading → Task 1 harness.

**Deferred to later increments (correctly out of scope):** chat UI (2), key store/Config (2), grounding/vision (3), agent loop + full `apply_process` with params (4), `run_pjsr` toggle (5), `release.sh`/`updates.xri` (6).

**Placeholder scan:** one flagged verify-before-build item remains — `Variant::ToInt()` accessor name (Task 3, Step 3). This is an explicit "confirm against `pcl/Variant.h`" instruction, not a hidden TODO; the implementer reads the header and uses the exact integer getter. All other code blocks are concrete.

**Type consistency:** `RunSelfTest(String&)->bool` defined in Task 3, consumed only in Task 3. `ThePICopilotModule/Process/Interface` globals defined in Tasks 1–2, used consistently. `ExecuteGlobal()->bool` signature identical in Tasks 2 and 3. Process id `"PICopilot"` identical across process, interface, and harness.

**Note for the implementer:** the exact required-virtuals set for `MetaModule`, `MetaProcess`, `ProcessImplementation`, and `ProcessInterface` is authoritatively shown in the sibling `modules/nukex/src/module/NukeX*.{h,cpp}` files — read them first and follow their structure; this plan specifies only the PICopilot-specific values and the self-test logic that differs from NukeX.
