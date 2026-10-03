# Proposal: Excalidraw MCP for NukeX Architecture Visualization

**To:** CC (Claude Code)  
**From:** Scott (via Claude Desktop architecture session)  
**Re:** Adding visual architecture scaffolding to the NukeX development workflow  

---

## The Problem We're Solving

Scott works as a 10,000ft system architect who sees entire systems simultaneously. When he hands you a large block of work, he already has the full system wired up in his head. You don't. You have what's in your context window.

This mismatch is the root cause of two recurring problems:

1. **The stub problem** — you sketch method signatures and class structures that look architecturally complete but have no call sites. They get discovered later. This is frustrating and wastes time.
2. **Context degradation** — on long sessions, your coherent picture of how the system fits together degrades. You start filling gaps you can't see with stubs or assumptions.

The goal of this proposal is to give you a persistent, low-token visual anchor for the NukeX architecture that travels with each session — so the system Scott sees in his head is also something you can reference explicitly.

---

## The Proposed Solution: Excalidraw MCP

### What It Is

The Excalidraw MCP server connects you (Claude Code) to an Excalidraw rendering engine via the Model Context Protocol. You describe architecture in structured JSON or plain English, the server renders it as a PNG or SVG, and the file lands on disk locally. No external services. No data leaving the machine. Runs on Nobara Linux without issues.

### How It Works

```bash
# Install the MCP server
claude mcp add --scope user --transport stdio excalidraw -- \
  npx -y excalidraw-render
```

Once connected, you can call it directly:

```json
{
  "elements": [],
  "format": "png",
  "output_path": "/home/scott/nukex/docs/diagrams/pipeline.png"
}
```

First render spins up a headless Chromium instance (~3 seconds). Subsequent renders are near-instant.

### The Bidirectional Workflow

- You generate the Excalidraw JSON representation of a system component
- Scott refines it manually in Excalidraw if needed
- The diagram lives in `/home/scott/nukex/docs/diagrams/`
- At the start of each session, the diagram is the context anchor — not prose, not memory, the actual visual structure

---

## What to Diagram for NukeX v4

These are the components Scott and Claude Desktop identified as needing visual clarity before implementation proceeds:

### 1. The Voxel Cube Data Structure
The core data structure. RGB metadata per pixel stored as a voxel, all voxels forming a cube. The cube is the source of truth across the entire pipeline. Diagram should show:
- Voxel structure (x, y, RGB distribution data)
- Cube assembly from input frames
- How downstream services read from the cube vs. write to it

### 2. The Full Processing Pipeline
End to end. Input frames → calibration → stacking (distribution fitting) → voxel cube → color space service → recombiner → stretch → rating popup → output. Diagram should show:
- Each stage as a discrete node
- Data flow between stages
- Where SHO / LRGB / OSC paths diverge and where they converge
- Where the color space service sits in the chain

### 3. The Color Space Service
The new service being built to eliminate the green problem. Diagram should show:
- Input: raw stacked channel data
- SHO path: emission line mapping (Ha→Red, OIII→Blue/Green, SII→Red)
- LRGB path: standard recombination
- OSC path: debayer → treated as LRGB internally (OSC mode collapsed into LRGB)
- Output: recombined image data handed to the recombiner
- Explicit call sites — every public method on this service must have a visible caller in the diagram

### 4. The Recombiner
Downstream of the color space service. Diagram should show:
- Input from color space service
- Normalization within the cube (global scale factor, not per-channel)
- Saturation and luminance balance logic
- Output to stretch pipeline

---

## Rules for Using These Diagrams in Sessions

**Before implementing any new service or module:**
- Generate the Excalidraw diagram first
- Every public method must have a visible call site on the diagram
- If you can't draw the call chain from entry point to output, do not write the code yet — tell Scott

**Evidence-based delivery:**
- After writing any new code block, explicitly trace every public method to its call site
- If a method has no call site, flag it immediately rather than leaving it as a stub
- "Show me the call graph" is a valid and expected request from Scott at any time

**Incremental wiring:**
- New services get wired into exactly one place in the existing pipeline first
- Prove that one path works end to end before expanding
- The diagram updates to reflect each wiring step

---

## Why This Matters for NukeX Specifically

NukeX v4 is close to shipping. The remaining issues are:

- Green narrowband output → color space service (in progress)
- OSC/LRGB unification → collapse OSC into LRGB path
- Recombination balance → normalize within the cube, not per-channel
- Stretch default → currently locked to Veralux, needs to open up
- Plate solving → iffy, may be deferred or handled via FITS header WCS passthrough

All of these touch the same pipeline region. A single accurate diagram of that region is worth more than any amount of prose context at the start of a session. Build the diagram first. Then implement.

---

## Next Step

Scott wants to know: **can you set up the Excalidraw MCP, generate an initial diagram of the NukeX v4 pipeline as you currently understand it from the codebase, and flag anywhere the diagram reveals a gap between what exists and what's described above?**

The gaps in the diagram are the stubs we haven't found yet.
