#!/usr/bin/env node

/**
 * PCL Parser MCP Server
 *
 * Model Context Protocol server for PCL C++ code analysis.
 * Provides tools and resources for AI-assisted PCL development.
 *
 * Protocol: JSON-RPC 2.0 over stdio
 */

import { PCLAnalyzer } from './pcl-parser.js';
import { readFileSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';
import { createInterface } from 'readline';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

// Load schemas
let coreSchema = {};
let extendedSchema = {};

try {
  coreSchema = JSON.parse(readFileSync(join(__dirname, '../schemas/pcl-core.json'), 'utf8'));
  extendedSchema = JSON.parse(readFileSync(join(__dirname, '../schemas/pcl-extended.json'), 'utf8'));
} catch (e) {
  // Schemas will be loaded when available
}

const analyzer = new PCLAnalyzer();

/**
 * MCP Protocol Constants
 */
const PROTOCOL_VERSION = '2024-11-05';
const SERVER_INFO = {
  name: 'pcl-parser-server',
  version: '1.0.0'
};

/**
 * Available tools
 */
const TOOLS = [
  {
    name: 'pcl_analyze',
    description: 'Analyze PCL C++ code structure. Returns includes, classes, functions, PCL objects used, and diagnostics.',
    inputSchema: {
      type: 'object',
      properties: {
        code: {
          type: 'string',
          description: 'The PCL C++ source code to analyze'
        }
      },
      required: ['code']
    }
  },
  {
    name: 'pcl_completions',
    description: 'Get code completion suggestions at a specific position in PCL C++ code.',
    inputSchema: {
      type: 'object',
      properties: {
        code: {
          type: 'string',
          description: 'The PCL C++ source code'
        },
        line: {
          type: 'number',
          description: 'Line number (1-indexed)'
        },
        column: {
          type: 'number',
          description: 'Column number (1-indexed)'
        }
      },
      required: ['code', 'line', 'column']
    }
  },
  {
    name: 'pcl_hover',
    description: 'Get documentation for a PCL symbol (class, method, constant, etc.).',
    inputSchema: {
      type: 'object',
      properties: {
        symbol: {
          type: 'string',
          description: 'The symbol name to look up (e.g., "Image", "Console", "ImageWindow")'
        }
      },
      required: ['symbol']
    }
  },
  {
    name: 'pcl_validate',
    description: 'Validate PCL C++ code for common issues and best practices.',
    inputSchema: {
      type: 'object',
      properties: {
        code: {
          type: 'string',
          description: 'The PCL C++ source code to validate'
        }
      },
      required: ['code']
    }
  },
  {
    name: 'pcl_template',
    description: 'Get a code template for common PCL patterns.',
    inputSchema: {
      type: 'object',
      properties: {
        name: {
          type: 'string',
          description: 'Template name: moduleDefinition, processInterface, imageProcessing, dialogTemplate, fileIO, threadedProcessing',
          enum: ['moduleDefinition', 'processInterface', 'imageProcessing', 'dialogTemplate', 'fileIO', 'threadedProcessing']
        }
      },
      required: ['name']
    }
  },
  {
    name: 'pcl_class_info',
    description: 'Get detailed documentation for a specific PCL class including constructors, methods, and properties.',
    inputSchema: {
      type: 'object',
      properties: {
        className: {
          type: 'string',
          description: 'The PCL class name (e.g., "Image", "ImageWindow", "Dialog", "Thread")'
        }
      },
      required: ['className']
    }
  },
  {
    name: 'pcl_constants',
    description: 'Get constants for a specific category.',
    inputSchema: {
      type: 'object',
      properties: {
        category: {
          type: 'string',
          description: 'Constant category: colorSpaces, imageOperations, interpolationMethods, threadPriorities, fileModes, textAlignment',
          enum: ['colorSpaces', 'imageOperations', 'interpolationMethods', 'threadPriorities', 'fileModes', 'textAlignment']
        }
      },
      required: ['category']
    }
  },
  {
    name: 'pcl_list_classes',
    description: 'List all available PCL classes with their categories.',
    inputSchema: {
      type: 'object',
      properties: {
        category: {
          type: 'string',
          description: 'Optional: filter by category (image, core, ui, math, file, module, threading, graphics, etc.)'
        }
      }
    }
  },
  {
    name: 'pcl_best_practices',
    description: 'Get best practices guide for PCL development.',
    inputSchema: {
      type: 'object',
      properties: {
        topic: {
          type: 'string',
          description: 'Topic: general, imageProcessing, threading, ui',
          enum: ['general', 'imageProcessing', 'threading', 'ui']
        }
      },
      required: ['topic']
    }
  },
  {
    name: 'pcl_headers',
    description: 'List common PCL header files and what they provide.',
    inputSchema: {
      type: 'object',
      properties: {}
    }
  }
];

/**
 * Available resources
 */
const RESOURCES = [
  {
    uri: 'pcl://schema',
    name: 'PCL API Schema',
    description: 'Complete PCL class library API schema in JSON format',
    mimeType: 'application/json'
  },
  {
    uri: 'pcl://quickref',
    name: 'PCL Quick Reference',
    description: 'Quick reference guide for PCL development',
    mimeType: 'text/markdown'
  },
  {
    uri: 'pcl://module-structure',
    name: 'PCL Module Structure',
    description: 'Standard PixInsight module file structure',
    mimeType: 'text/markdown'
  }
];

/**
 * Handle tool calls
 */
function handleToolCall(name, args) {
  switch (name) {
    case 'pcl_analyze': {
      const result = analyzer.analyze(args.code);
      return {
        includes: result.includes,
        defines: result.defines,
        namespaces: result.namespaces,
        classes: result.classes,
        functions: result.functions,
        pclObjects: result.pclObjects,
        diagnostics: result.diagnostics
      };
    }

    case 'pcl_completions': {
      const completions = analyzer.getCompletions(args.code, args.line, args.column);
      return { completions };
    }

    case 'pcl_hover': {
      const info = analyzer.getHoverInfo(args.symbol);
      return {
        symbol: args.symbol,
        documentation: info || `No documentation found for "${args.symbol}"`
      };
    }

    case 'pcl_validate': {
      return analyzer.validate(args.code);
    }

    case 'pcl_template': {
      const templates = analyzer.getTemplates();
      const template = templates[args.name];
      if (template) {
        return template;
      }
      return { error: `Template "${args.name}" not found` };
    }

    case 'pcl_class_info': {
      const classInfo = analyzer.getClassInfo(args.className);
      if (classInfo) {
        return { name: args.className, ...classInfo };
      }
      return { error: `Class "${args.className}" not found in schema` };
    }

    case 'pcl_constants': {
      const constants = coreSchema.constants?.[args.category];
      if (constants) {
        return { category: args.category, constants };
      }
      return { error: `Category "${args.category}" not found` };
    }

    case 'pcl_list_classes': {
      let classes = analyzer.listClasses();
      if (args.category) {
        classes = classes.filter(c => c.category === args.category);
      }
      return { classes, count: classes.length };
    }

    case 'pcl_best_practices': {
      const content = analyzer.getBestPractices(args.topic);
      return { topic: args.topic, content };
    }

    case 'pcl_headers': {
      const headers = analyzer.getPCLHeaders();
      const headerInfo = extendedSchema.includeFiles || {};
      return {
        headers: headers.map(h => ({
          path: h,
          ...headerInfo[h]
        }))
      };
    }

    default:
      throw new Error(`Unknown tool: ${name}`);
  }
}

/**
 * Handle resource requests
 */
function handleResourceRequest(uri) {
  switch (uri) {
    case 'pcl://schema':
      return {
        contents: [{
          uri,
          mimeType: 'application/json',
          text: JSON.stringify({ core: coreSchema, extended: extendedSchema }, null, 2)
        }]
      };

    case 'pcl://quickref':
      return {
        contents: [{
          uri,
          mimeType: 'text/markdown',
          text: generateQuickReference()
        }]
      };

    case 'pcl://module-structure':
      return {
        contents: [{
          uri,
          mimeType: 'text/markdown',
          text: generateModuleStructureGuide()
        }]
      };

    default:
      throw new Error(`Unknown resource: ${uri}`);
  }
}

/**
 * Generate quick reference markdown
 */
function generateQuickReference() {
  return `# PCL Quick Reference

## Core Classes

### Image Processing
- \`Image\` - 2D image with multiple channels
- \`ImageWindow\` - PixInsight image window
- \`View\` - View of an image
- \`ImageVariant\` - Type-agnostic image reference

### Math
- \`Vector\` - Mathematical vector
- \`Matrix\` - Mathematical matrix
- \`Point\`, \`DPoint\` - 2D point (int/double)
- \`Rect\`, \`DRect\` - Rectangle (int/double)

### File I/O
- \`File\` - General file operations
- \`XISFReader\` / \`XISFWriter\` - XISF format
- \`FITSKeyword\` - FITS metadata

### Threading
- \`Thread\` - Thread class
- \`Mutex\` - Mutual exclusion
- \`AutoLock\` - RAII lock

### UI Controls
- \`Dialog\` - Modal/modeless dialog
- \`Control\` - Base widget class
- \`Sizer\` - Layout manager
- \`Label\`, \`Edit\`, \`SpinBox\`, \`CheckBox\`, etc.

## Module Development

\`\`\`cpp
// Required classes to subclass:
class MyModule : public MetaModule { };
class MyProcess : public MetaProcess { };
class MyInstance : public ProcessImplementation { };
class MyInterface : public ProcessInterface { };
\`\`\`

## Common Patterns

### Access Image Data
\`\`\`cpp
pcl::ImageWindow window = pcl::ImageWindow::ActiveWindow();
pcl::View view = window.MainView();
pcl::Image image;
view.GetImage(image);
\`\`\`

### Console Output
\`\`\`cpp
pcl::Console console;
console.WriteLn("Processing...");
console.Warning("Check parameters");
\`\`\`

### Progress Monitoring
\`\`\`cpp
pcl::StandardStatus status;
image.SetStatusCallback(&status);
\`\`\`
`;
}

/**
 * Generate module structure guide
 */
function generateModuleStructureGuide() {
  const structure = extendedSchema.moduleStructure || {};

  let md = `# PixInsight Module Structure

## Required Files

`;

  if (structure.requiredFiles) {
    structure.requiredFiles.forEach(f => {
      md += `- **${f.name}** - ${f.description}\n`;
    });
  }

  md += `
## Optional Files

`;

  if (structure.optionalFiles) {
    structure.optionalFiles.forEach(f => {
      md += `- **${f.name}** - ${f.description}\n`;
    });
  }

  md += `
## Version Macros

Define these in your Module.h:

\`\`\`cpp
#define MODULE_VERSION_MAJOR     1
#define MODULE_VERSION_MINOR     0
#define MODULE_VERSION_REVISION  0
#define MODULE_VERSION_BUILD     1
#define MODULE_VERSION_LANGUAGE  "eng"

#define MODULE_RELEASE_YEAR      2024
#define MODULE_RELEASE_MONTH     1
#define MODULE_RELEASE_DAY       1
\`\`\`

## Directory Structure

\`\`\`
MyModule/
├── Module.cpp
├── Module.h
├── MyProcess.cpp
├── MyProcess.h
├── MyProcessParameters.cpp
├── MyProcessParameters.h
├── MyProcessInstance.cpp
├── MyProcessInstance.h
├── MyProcessInterface.cpp
├── MyProcessInterface.h
├── freebsd/
│   └── g++/
│       └── makefile-x64
├── linux/
│   └── g++/
│       └── makefile-x64
├── macosx/
│   └── g++/
│       └── makefile-x64
└── windows/
    └── vc17/
        └── MyModule.vcxproj
\`\`\`
`;

  return md;
}

/**
 * Handle JSON-RPC requests
 */
function handleRequest(request) {
  const { id, method, params } = request;

  try {
    let result;

    switch (method) {
      case 'initialize':
        result = {
          protocolVersion: PROTOCOL_VERSION,
          serverInfo: SERVER_INFO,
          capabilities: {
            tools: {},
            resources: {}
          }
        };
        break;

      case 'initialized':
        result = {};
        break;

      case 'tools/list':
        result = { tools: TOOLS };
        break;

      case 'tools/call':
        const toolResult = handleToolCall(params.name, params.arguments || {});
        result = {
          content: [{
            type: 'text',
            text: typeof toolResult === 'string' ? toolResult : JSON.stringify(toolResult, null, 2)
          }]
        };
        break;

      case 'resources/list':
        result = { resources: RESOURCES };
        break;

      case 'resources/read':
        result = handleResourceRequest(params.uri);
        break;

      case 'ping':
        result = {};
        break;

      default:
        throw { code: -32601, message: `Method not found: ${method}` };
    }

    return { jsonrpc: '2.0', id, result };

  } catch (error) {
    return {
      jsonrpc: '2.0',
      id,
      error: {
        code: error.code || -32603,
        message: error.message || 'Internal error'
      }
    };
  }
}

/**
 * Main entry point
 */
const rl = createInterface({
  input: process.stdin,
  output: process.stdout,
  terminal: false
});

rl.on('line', (line) => {
  if (!line.trim()) return;

  try {
    const request = JSON.parse(line);
    const response = handleRequest(request);

    // Only send response if request has an id (not a notification)
    if (request.id !== undefined) {
      console.log(JSON.stringify(response));
    }
  } catch (e) {
    console.log(JSON.stringify({
      jsonrpc: '2.0',
      id: null,
      error: {
        code: -32700,
        message: 'Parse error'
      }
    }));
  }
});

rl.on('close', () => {
  process.exit(0);
});

// Handle errors gracefully
process.on('uncaughtException', (e) => {
  console.error('Uncaught exception:', e.message);
});
