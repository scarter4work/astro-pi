# PCL Parser Server

A language parser server for PixInsight's PCL (PixInsight Class Library) for C++ module development.

## Overview

PCL Parser provides code analysis, completions, validation, and documentation for PCL-based C++ development. It enables AI assistants and development tools to understand and work with PixInsight module code.

## Features

- **Code Analysis**: Parse PCL C++ code to extract includes, classes, functions, and PCL object usage
- **Code Completions**: Context-aware completion suggestions for PCL classes and methods
- **Hover Documentation**: Inline documentation for PCL symbols
- **Validation**: Check code for common issues and best practices
- **Code Templates**: Ready-to-use templates for common PCL patterns
- **Schema Reference**: Complete PCL API documentation

## Installation

```bash
cd pcl_parser
npm install
```

## Usage

### HTTP Server

Start the REST API server (port 3848):

```bash
npm start
```

### MCP Server

Run as a Model Context Protocol server for Claude integration:

```bash
npm run mcp
```

### Tests

```bash
npm test
```

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Server info and API documentation |
| `/analyze` | POST | Analyze PCL C++ code structure |
| `/completions` | POST | Get code completions at position |
| `/hover` | POST | Get documentation for symbol |
| `/validate` | POST | Validate PCL code |
| `/templates` | GET | List code templates |
| `/templates/:name` | GET | Get specific template |
| `/classes` | GET | List all PCL classes |
| `/classes/:name` | GET | Get detailed class info |
| `/constants` | GET | List constant categories |
| `/constants/:category` | GET | Get constants by category |
| `/schema` | GET | Full PCL API schema |
| `/best-practices` | GET | Best practices guide |
| `/headers` | GET | List PCL header files |

## MCP Tools

The MCP server provides these tools:

- `pcl_analyze` - Analyze code structure
- `pcl_completions` - Get code completions
- `pcl_hover` - Get symbol documentation
- `pcl_validate` - Validate code
- `pcl_template` - Get code templates
- `pcl_class_info` - Get class documentation
- `pcl_constants` - Get constants by category
- `pcl_list_classes` - List available classes
- `pcl_best_practices` - Get best practices
- `pcl_headers` - List PCL headers

## MCP Resources

- `pcl://schema` - Complete API schema (JSON)
- `pcl://quickref` - Quick reference guide (Markdown)
- `pcl://module-structure` - Module file structure guide

## Claude Code Integration

Add to your `.claude.json` or MCP configuration:

```json
{
  "mcpServers": {
    "pcl": {
      "command": "node",
      "args": ["/path/to/pcl_parser/src/mcp-server.js"]
    }
  }
}
```

## Code Templates

Available templates:

- **moduleDefinition** - Basic PixInsight module structure
- **processInterface** - GUI interface for a process
- **imageProcessing** - Image processing with progress monitoring
- **dialogTemplate** - Custom dialog with controls
- **fileIO** - Reading and writing XISF files
- **threadedProcessing** - Parallel image processing

## Example Usage

### Analyze Code

```bash
curl -X POST http://localhost:3848/analyze \
  -H "Content-Type: application/json" \
  -d '{"code": "#include <pcl/Image.h>\nusing namespace pcl;\nImage img;"}'
```

### Get Class Info

```bash
curl http://localhost:3848/classes/Image
```

### Get Template

```bash
curl http://localhost:3848/templates/imageProcessing
```

## PCL Resources

- **PCL Repository**: https://gitlab.com/pixinsight/PCL
- **Reference Documentation**: https://pixinsight.com/developer/pcl/
- **XISF Specification**: https://gitlab.com/pixinsight/XISF-specification

## PCL Class Categories

- **image** - Image, ImageWindow, View, ImageVariant
- **core** - Console, String, IsoString, Array
- **math** - Vector, Matrix, Point, Rect
- **file** - File, XISFReader, XISFWriter
- **module** - MetaModule, MetaProcess, ProcessImplementation, ProcessInterface
- **threading** - Thread, Mutex, AutoLock
- **ui** - Dialog, Control, Sizer, Label, PushButton, etc.
- **graphics** - Graphics, Bitmap, Pen, Brush, Font
- **transform** - Convolution, FFTConvolution, HistogramTransformation
- **geometry** - Resample, Rotation, Translation, Crop
- **astrometry** - AstrometricMetadata, Position, TimePoint
- **analysis** - StarDetector, PSFFit

## License

MIT

## Author

scarter4work - PixInsight Certified Developer
