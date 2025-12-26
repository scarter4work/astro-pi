# PJSR Parser Server

A language parser server for **PixInsight JavaScript Runtime (PJSR)** that helps Claude Code (and other AI assistants) write better code for PixInsight astrophotography software.

## Overview

PixInsight is an advanced image processing platform primarily used in astrophotography. PJSR (PixInsight JavaScript Runtime) is its embedded scripting environment based on SpiderMonkey 24 (ECMA 262-5 compliant).

This project provides:

- **Comprehensive PJSR API Schema** - Complete type definitions for all PJSR classes, methods, and constants
- **Code Parser & Analyzer** - Lexer and analyzer for PJSR code
- **HTTP Server** - RESTful API for code analysis
- **MCP Server** - Model Context Protocol server for AI assistant integration
- **Claude Code Skill** - Ready-to-use skill file for Claude Code

## Features

- Parse and analyze PJSR scripts
- Code completion suggestions
- Hover documentation for symbols
- Code validation with diagnostics
- Code templates for common patterns
- Full API reference for 50+ PJSR classes

## Quick Start

### Installation

```bash
# Clone the repository
git clone https://github.com/YOUR_USERNAME/pjsr-parser-server.git
cd pjsr-parser-server

# Install dependencies
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

## API Endpoints

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

## Claude Code Integration

### As an MCP Server

Add to your Claude Code MCP configuration:

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

Copy `skills/pjsr.md` to your Claude Code skills directory.

## MCP Tools

The MCP server provides these tools:

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
- `Matrix`, `Vector` - Math structures
- And 40+ more...

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

- **scriptTemplate** - Standard script structure
- **dialogTemplate** - Dialog with controls and layout
- **imageProcessing** - Image processing with undo support
- **batchProcessing** - Batch file processing
- **processExecution** - Execute PixInsight processes
- **fileIO** - File reading and writing

## Resources

- [PixInsight PJSR](https://pixinsight.com/developer/pjsr/)
- [PCL Reference](https://pixinsight.com/developer/pcl/doc/html/)
- [PJSR GitLab Repository](https://gitlab.com/pixinsight/PJSR)
- [PixInsight Forum](https://pixinsight.com/forum/)

## License

MIT License

## Acknowledgments

- [PixInsight](https://pixinsight.com) by Pleiades Astrophoto
- API documentation derived from official PixInsight resources
- Community contributions from the PixInsight Forum
