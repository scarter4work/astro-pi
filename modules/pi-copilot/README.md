# PI Copilot Native PCL Module

This is the native PixInsight PCL implementation of PI Copilot, replacing the retired Node.js sidecar. It implements the core scripting engine and process management layer as a loadable PI module, enabling AI-assisted workflows directly within PixInsight through a dockable interface.

## Build

From `modules/pi-copilot/`:

```bash
cmake -B build -DPCLDIR=$HOME/PCL -DPICOPILOT_BUILD_MODULE=ON && cmake --build build -j$(nproc)
```

## Test

Self-test (proves `EvaluateScript` execution and `ProcessInstance` construction):

```bash
bash test/run-selftest.sh
```

Also: `bash test/run-load.sh` — loads the module headlessly without running self-test.

## Design & Increment 1 Scope

Full specification: [`docs/superpowers/specs/2026-09-20-pi-copilot-native-pcl-design.md`](../../docs/superpowers/specs/2026-09-20-pi-copilot-native-pcl-design.md)

Increment 1 deliverables:
- Native module skeleton with CMake build
- Empty dockable `ProcessInterface` panel
- `EvaluateScript("1+2") == 3` proof of PJSR execution from C++
- Self-contained self-test shipped in-module (runs on `ExecuteGlobal`, harmless in production)
- Signing and headless loading verified

## Verified

**2026-09-20** — self-test PASS on built module:

```
{"evalResult":3,"evalOk":true,"processInstanceValid":true,"ok":true}PASS: EvaluateScript==3 and ProcessInstance valid
```

Full harness: signs module, loads headlessly under `PixInsight --automation-mode`, executes self-test, and exits with no interactive UI required.
