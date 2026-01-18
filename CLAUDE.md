## Available Tools

- **GitHub CLI (`gh`)**: Fully authenticated with full permissions. Use for all GitHub operations.
- **MCP GitHub tools**: Available but `gh` CLI is often more flexible
- **Bash**: Full shell access
- **Render MCP**: For deployment operations

**Remember**: Check available tools before claiming something can't be done.

---

## Project Quick Reference

**NukeX** is a PixInsight module with two processes:
1. **NukeX** - Region-aware stretching (lower priority)
2. **NukeXStack** - Intelligent pixel selection stacking (PRIMARY FOCUS)

**Key Files for Stacking:**
- `src/NukeXStackInstance.cpp` - Main integration pipeline
- `src/engine/PixelStackAnalyzer.cpp` - Per-pixel distribution fitting
- `src/engine/PixelSelector.cpp` - ML-guided pixel selection
- `src/engine/TransitionChecker.cpp` - Transition smoothing

**See PROJECT.md for full architecture and status.**

---

## Hardware

- **GPU**: NVIDIA GeForce RTX 5070 Ti (16GB VRAM)
- **Training**: Use `--batch-size 32` or `--batch-size 64`

---

## PixInsight Module Signing

```bash
/opt/PixInsight/bin/PixInsight.sh --sign-module-file=<module.so> \
  --xssk-file=/home/scarter4work/projects/keys/scarter4work_keys.xssk \
  --xssk-password="***REDACTED***"
```

---

## Build (Once Makefile Exists)

```bash
cd /home/scarter4work/projects/NukeX2
make clean && make -j8
# Then sign and install
```

---

## GitHub

- **Repo**: https://github.com/scarter4work/NukeX2
- **CLI**: `gh` is authenticated as `scarter4work`
