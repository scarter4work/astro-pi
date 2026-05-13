# PJSR Parser Server

A language parser server for **PixInsight JavaScript Runtime (PJSR)** that helps Claude Code (and other AI assistants) write better code for PixInsight astrophotography software.

## Claude Code Plugin Installation

The easiest way to use this with Claude Code is to install it as a plugin:

```bash
claude plugin install scarter4work/pjsr-parser-server
```

This gives you:
- **PJSR Skill** - Claude automatically knows PJSR APIs and best practices
- **MCP Tools** - Code analysis, validation, completions, and documentation lookup
- **Slash Commands** - Quick access to common operations

### Slash Commands

After installation, these commands are available:

| Command | Description |
|---------|-------------|
| `/pjsr-analyze` | Analyze PJSR code for issues and best practices |
| `/pjsr-template` | Generate code templates (script, dialog, batch, etc.) |
| `/pjsr-class` | Look up detailed class documentation |
| `/pjsr-help` | Get best practices and coding guidelines |

### MCP Tools

The plugin provides these tools that Claude can use automatically:

| Tool | Description |
|------|-------------|
| `pjsr_analyze` | Analyze PJSR code structure |
| `pjsr_completions` | Get code completions at position |
| `pjsr_hover` | Get symbol documentation |
| `pjsr_validate` | Validate code and get diagnostics |
| `pjsr_template` | Get code templates |
| `pjsr_class_info` | Get detailed class documentation |
| `pjsr_constants` | Get constants by category |
| `pjsr_list_classes` | List all available classes |
| `pjsr_best_practices` | Get best practices guide |

---

## Overview

PixInsight is an advanced image processing platform primarily used in astrophotography. PJSR (PixInsight JavaScript Runtime) is its embedded scripting environment.

**As of PixInsight 1.9.4 Lockhart (March 2026), PJSR runs on Google's V8 JavaScript engine** (full ECMAScript 2025 support) alongside the legacy SpiderMonkey 24 engine. SpiderMonkey remains the default for backward compatibility but is scheduled for removal in a future 1.9 release. Scripts opt into V8 with the new `#engine v8` directive.

This project provides:

- **Comprehensive PJSR API Schema** - Type definitions for all PJSR classes, methods, and constants, including the new V8-only classes (FMath, Stat, ImageIterator, PSF, BRQuadTree, XMLDocument family, System, CoreApplication, Runtime)
- **Engine-aware Code Parser & Analyzer** - Detects `#engine` directive and applies engine-appropriate diagnostics
- **Schema-driven Validator** - 15+ lint rules covering V8 migration patterns (`__base__` inheritance, `.prototype.CONSTANT`, removed APIs, deprecated globals, flat constants, etc.)
- **HTTP Server** - RESTful API for code analysis
- **MCP Server** - Model Context Protocol server for AI assistant integration
- **Claude Code Plugin** - Ready-to-install plugin with skills and commands

## Features

- Parse and analyze PJSR scripts on V8 or SpiderMonkey
- Code completion suggestions
- Hover documentation for symbols
- Engine-aware code validation with rewrite hints (e.g. `StdIcon_Warning` → `StdIcon.Warning`)
- V8 and SM code templates for common patterns
- Full API reference for 60+ PJSR classes including the V8-only additions

## Engine-aware analysis

The validator detects the `#engine` directive at the top of a script:

| Directive | Engine | Notes |
|-----------|--------|-------|
| `#engine v8` | V8 (new runtime, isolated) | Recommended for new scripts |
| `#engine v8-new` | V8 (new runtime, isolated) | Explicit form of `v8` |
| `#engine v8-default` | V8 (root, persistent) | For testing only — pollutes the runtime |
| `#engine v8-private` | V8 (persistent, per-script) | Reuses script's runtime across executions |
| `#engine sm` | SpiderMonkey 24 (legacy) | Explicit legacy opt-in |
| *(none)* | SpiderMonkey 24 (default) | Validator suggests adding `#engine v8` |

Diagnostics tagged `appliesToEngines: ["v8"]` only fire on V8-targeting scripts, and SM-default scripts get a "consider migrating" hint.

## Standalone Usage

If you want to run the servers directly instead of using the plugin:

### Installation

```bash
git clone https://github.com/scarter4work/pjsr-parser-server.git
cd pjsr-parser-server
npm install
```

### Run HTTP Server

```bash
npm start
# Server runs on http://localhost:3847
```

### Run MCP Server

```bash
npm run mcp
```

## HTTP API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Server info and API documentation |
| `/analyze` | POST | Analyze PJSR code |
| `/completions` | POST | Get code completion suggestions |
| `/hover` | POST | Get documentation for a symbol |
| `/validate` | POST | Validate PJSR code |
| `/templates` | GET | List available code templates |
| `/templates/:name` | GET | Get specific template |
| `/classes` | GET | List all PJSR classes |
| `/classes/:name` | GET | Get class documentation |
| `/constants` | GET | List constant categories |
| `/constants/:category` | GET | Get constants by category |
| `/schema` | GET | Full PJSR API schema |

## Usage Examples

### Analyze Code

```bash
curl -X POST http://localhost:3847/analyze \
  -H "Content-Type: application/json" \
  -d '{"code": "var window = ImageWindow.activeWindow;"}'
```

### Get Class Info

```bash
curl http://localhost:3847/classes/ImageWindow
```

### Get Code Template

```bash
curl http://localhost:3847/templates/dialogTemplate
```

## Manual Claude Code Integration

If you prefer manual setup instead of the plugin:

### As an MCP Server

Add to your Claude Code MCP configuration (`~/.claude.json` or project `.mcp.json`):

```json
{
  "mcpServers": {
    "pjsr": {
      "command": "node",
      "args": ["/path/to/pjsr-parser-server/src/mcp-server.js"]
    }
  }
}
```

### As a Skill

Copy `skills/pjsr/SKILL.md` to your Claude Code skills directory.

## PJSR Schema

The `schemas/pjsr-core.json` file contains comprehensive documentation for:

### Core Classes
- `Image` - 2D image manipulation
- `ImageWindow` - Image window interface
- `View` - View interface
- `ProcessInstance` - Process execution
- `Dialog` - Modal dialogs
- `Control` - UI control base
- `Sizer` - Layout management
- `File` - File I/O
- `Matrix`, `Vector` - Math structures (pure-JS reimplementation under V8)
- And 40+ more...

### V8-only Classes (PixInsight 1.9.4+)
- `FMath` - Fast Math with WebAssembly backing; near-native speed
- `Stat` - Statistical calculations (median, MAD, stdDev, etc.)
- `ImageIterator` - Direct typed-array pixel access; replaces `Image.forEachSample`
- `StarDetector` - C++ core star detection (was JS-only)
- `PSF` / `PSFData` - Levenberg-Marquardt PSF fitting
- `BRQuadTree` - Bucket-region quadtree
- `XMLDocument`, `XMLDeclaration`, `XMLComment`, `XMLElement`, `XMLText` - XML support
- `System` - Host machine / OS introspection
- `CoreApplication` - Replaces most deprecated `Global.*` extensions; `ensureMinimumVersion()`
- `Runtime` - Active engine introspection

### UI Controls
- `Label`, `PushButton`, `ToolButton`
- `CheckBox`, `RadioButton`
- `Edit`, `TextBox`, `SpinBox`
- `ComboBox`, `Slider`
- `TreeBox`, `ViewList`
- `GroupBox`, `TabBox`, `ScrollBox`
- And more...

### Constants
- Color spaces
- Data types
- Image operations
- Morphological operations
- Interpolation methods
- Dialog/button codes
- Frame/focus styles
- Undo flags

## Code Templates

Available templates:

**Legacy (SpiderMonkey-compatible):**
- **scriptTemplate** - Standard script structure
- **dialogTemplate** - Dialog with controls and layout (uses `__base__` pattern)
- **imageProcessing** - Image processing with undo support
- **batchProcessing** - Batch file processing
- **processExecution** - Execute PixInsight processes
- **fileIO** - File reading and writing

**V8 (PixInsight 1.9.4+):**
- **scriptTemplateV8** - V8 script with `#engine v8` and `CoreApplication.ensureMinimumVersion`
- **dialogTemplateV8** - Dialog using `class extends Dialog` + arrow-function event handlers
- **imageProcessingV8** - Uses `ImageIterator` and `FMath.mtf`
- **starDetectionV8** - Uses new `StarDetector` and `PSF` core classes

## Resources

- [PixInsight PJSR](https://pixinsight.com/developer/pjsr/)
- [V8 Runtime Porting Guide (PixInsight 1.9.4)](https://pixinsight.net/dev/index.php?ams/the-new-v8-javascript-runtime-in-pixinsight-1-9-4-script-porting-guide.13/) - Juan Conejero's official guide
- [PCL Reference](https://pixinsight.com/developer/pcl/doc/html/)
- [PJSR GitLab Repository](https://gitlab.com/pixinsight/PJSR)
- [PixInsight Forum](https://pixinsight.com/forum/)

## License

MIT License

## Author

Brian Scott Carter (scarter4work@yahoo.com)

## Acknowledgments

- [PixInsight](https://pixinsight.com) by Pleiades Astrophoto
- API documentation derived from official PixInsight resources
- Community contributions from the PixInsight Forum
