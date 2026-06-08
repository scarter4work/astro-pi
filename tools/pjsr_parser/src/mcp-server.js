#!/usr/bin/env node
/**
 * PJSR MCP Server - Model Context Protocol server for PixInsight scripting assistance
 *
 * This server provides tools and resources for writing PixInsight PJSR scripts.
 * It can be used by Claude Code to assist with PixInsight script development.
 */

import { PJSRAnalyzer, pjsrSchema } from './pjsr-parser.js';
import { readFileSync } from 'fs';
import { createInterface } from 'readline';

const analyzer = new PJSRAnalyzer();

/**
 * MCP Server implementation using stdio transport
 */
class PJSRMCPServer {
  constructor() {
    this.tools = {
      'pjsr_analyze': {
        name: 'pjsr_analyze',
        description: 'Analyze PJSR (PixInsight JavaScript Runtime) code. Returns information about includes, functions, variables, used PJSR objects, and diagnostics.',
        inputSchema: {
          type: 'object',
          properties: {
            code: {
              type: 'string',
              description: 'The PJSR code to analyze'
            }
          },
          required: ['code']
        }
      },
      'pjsr_completions': {
        name: 'pjsr_completions',
        description: 'Get code completion suggestions for PJSR code at a given position',
        inputSchema: {
          type: 'object',
          properties: {
            code: {
              type: 'string',
              description: 'The PJSR code'
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
      'pjsr_hover': {
        name: 'pjsr_hover',
        description: 'Get documentation/hover information for a PJSR symbol (class, method, constant, etc.)',
        inputSchema: {
          type: 'object',
          properties: {
            symbol: {
              type: 'string',
              description: 'The symbol to look up (e.g., "ImageWindow", "View", "ProcessInstance")'
            }
          },
          required: ['symbol']
        }
      },
      'pjsr_validate': {
        name: 'pjsr_validate',
        description: 'Validate PJSR code and return any errors or warnings',
        inputSchema: {
          type: 'object',
          properties: {
            code: {
              type: 'string',
              description: 'The PJSR code to validate'
            }
          },
          required: ['code']
        }
      },
      'pjsr_template': {
        name: 'pjsr_template',
        description: 'Get a PJSR code template/snippet for common patterns',
        inputSchema: {
          type: 'object',
          properties: {
            templateName: {
              type: 'string',
              description: 'Template name: "scriptTemplate", "dialogTemplate", "imageProcessing", "batchProcessing", "processExecution", "fileIO"'
            }
          },
          required: ['templateName']
        }
      },
      'pjsr_class_info': {
        name: 'pjsr_class_info',
        description: 'Get detailed information about a PJSR class including constructors, properties, methods, and event handlers',
        inputSchema: {
          type: 'object',
          properties: {
            className: {
              type: 'string',
              description: 'The class name (e.g., "Image", "ImageWindow", "Dialog", "Control")'
            }
          },
          required: ['className']
        }
      },
      'pjsr_constants': {
        name: 'pjsr_constants',
        description: 'Get PJSR constants by category',
        inputSchema: {
          type: 'object',
          properties: {
            category: {
              type: 'string',
              description: 'Category: "colorSpaces", "dataTypes", "imageOperations", "morphologicalOperations", "interpolationMethods", "dialogCodes", "buttonCodes", "iconCodes", "textAlignment", "frameStyles", "focusStyles", "cursors", "undoFlags", "maskModes"'
            }
          },
          required: ['category']
        }
      },
      'pjsr_list_classes': {
        name: 'pjsr_list_classes',
        description: 'List all available PJSR classes (core classes and UI controls)',
        inputSchema: {
          type: 'object',
          properties: {},
          required: []
        }
      },
      'pjsr_best_practices': {
        name: 'pjsr_best_practices',
        description: 'Get best practices and guidelines for PJSR script development',
        inputSchema: {
          type: 'object',
          properties: {
            topic: {
              type: 'string',
              description: 'Optional topic: "general", "memory", "ui", "processing", "errors"'
            }
          },
          required: []
        }
      }
    };

    this.resources = {
      'pjsr://schema': {
        uri: 'pjsr://schema',
        name: 'PJSR API Schema',
        description: 'Complete PJSR API schema with all classes, methods, and constants',
        mimeType: 'application/json'
      },
      'pjsr://quickref': {
        uri: 'pjsr://quickref',
        name: 'PJSR Quick Reference',
        description: 'Quick reference guide for PJSR scripting',
        mimeType: 'text/markdown'
      }
    };
  }

  handleRequest(request) {
    const { method, params, id } = request;

    switch (method) {
      case 'initialize':
        return this.handleInitialize(id, params);
      case 'tools/list':
        return this.handleToolsList(id);
      case 'tools/call':
        return this.handleToolCall(id, params);
      case 'resources/list':
        return this.handleResourcesList(id);
      case 'resources/read':
        return this.handleResourceRead(id, params);
      default:
        return this.errorResponse(id, -32601, `Method not found: ${method}`);
    }
  }

  handleInitialize(id, params) {
    return {
      jsonrpc: '2.0',
      id,
      result: {
        protocolVersion: '2024-11-05',
        capabilities: {
          tools: {},
          resources: {}
        },
        serverInfo: {
          name: 'pjsr-parser-server',
          version: '1.0.0'
        }
      }
    };
  }

  handleToolsList(id) {
    return {
      jsonrpc: '2.0',
      id,
      result: {
        tools: Object.values(this.tools)
      }
    };
  }

  handleToolCall(id, params) {
    const { name, arguments: args } = params;

    try {
      let result;

      switch (name) {
        case 'pjsr_analyze':
          result = analyzer.analyze(args.code);
          break;

        case 'pjsr_completions':
          result = analyzer.getCompletions(args.code, args.line, args.column);
          break;

        case 'pjsr_hover':
          result = analyzer.getHoverInfo(args.symbol);
          if (!result) {
            result = { error: `No documentation found for symbol: ${args.symbol}` };
          }
          break;

        case 'pjsr_validate':
          result = analyzer.validate(args.code);
          break;

        case 'pjsr_template':
          const templates = analyzer.getTemplates();
          result = templates[args.templateName];
          if (!result) {
            result = { error: `Template not found: ${args.templateName}`, available: Object.keys(templates) };
          }
          break;

        case 'pjsr_class_info':
          result = this.getDetailedClassInfo(args.className);
          break;

        case 'pjsr_constants':
          result = pjsrSchema.constants?.[args.category];
          if (!result) {
            result = { error: `Category not found: ${args.category}`, available: Object.keys(pjsrSchema.constants || {}) };
          }
          break;

        case 'pjsr_list_classes':
          result = this.listAllClasses();
          break;

        case 'pjsr_best_practices':
          result = this.getBestPractices(args.topic);
          break;

        default:
          return this.errorResponse(id, -32602, `Unknown tool: ${name}`);
      }

      return {
        jsonrpc: '2.0',
        id,
        result: {
          content: [{
            type: 'text',
            text: typeof result === 'string' ? result : JSON.stringify(result, null, 2)
          }]
        }
      };
    } catch (error) {
      return this.errorResponse(id, -32603, error.message);
    }
  }

  handleResourcesList(id) {
    return {
      jsonrpc: '2.0',
      id,
      result: {
        resources: Object.values(this.resources)
      }
    };
  }

  handleResourceRead(id, params) {
    const { uri } = params;

    switch (uri) {
      case 'pjsr://schema':
        return {
          jsonrpc: '2.0',
          id,
          result: {
            contents: [{
              uri,
              mimeType: 'application/json',
              text: JSON.stringify(pjsrSchema, null, 2)
            }]
          }
        };

      case 'pjsr://quickref':
        return {
          jsonrpc: '2.0',
          id,
          result: {
            contents: [{
              uri,
              mimeType: 'text/markdown',
              text: this.generateQuickReference()
            }]
          }
        };

      default:
        return this.errorResponse(id, -32602, `Unknown resource: ${uri}`);
    }
  }

  getDetailedClassInfo(className) {
    const classInfo = pjsrSchema.coreClasses?.[className] ||
                      pjsrSchema.uiControls?.[className] ||
                      pjsrSchema.globalObjects?.[className];

    if (!classInfo) {
      return {
        error: `Class not found: ${className}`,
        availableClasses: [
          ...Object.keys(pjsrSchema.coreClasses || {}),
          ...Object.keys(pjsrSchema.uiControls || {})
        ]
      };
    }

    return {
      name: className,
      ...classInfo
    };
  }

  listAllClasses() {
    return {
      coreClasses: Object.keys(pjsrSchema.coreClasses || {}).map(name => ({
        name,
        description: pjsrSchema.coreClasses[name].description
      })),
      uiControls: Object.keys(pjsrSchema.uiControls || {}).map(name => ({
        name,
        description: pjsrSchema.uiControls[name].description
      })),
      globalObjects: Object.keys(pjsrSchema.globalObjects || {}).map(name => ({
        name,
        description: pjsrSchema.globalObjects[name].description || pjsrSchema.globalObjects[name].type
      }))
    };
  }

  getBestPractices(topic) {
    const practices = {
      general: `# PJSR General Best Practices

1. **Always include version checks**
   \`\`\`javascript
   #iflt __PI_VERSION__ 01.08.00
   #error This script requires PixInsight 1.8.0 or higher.
   #endif
   \`\`\`

2. **Enable automatic garbage collection**
   \`\`\`javascript
   jsAutoGC = true;
   \`\`\`

3. **Use proper script structure**
   - Feature declarations (#feature-id, #feature-info)
   - Includes at the top
   - Version checks
   - Main function wrapper

4. **Use meaningful variable names** following JavaScript conventions

5. **Comment your code** especially for complex algorithms`,

      memory: `# PJSR Memory Management

1. **Enable automatic GC**: \`jsAutoGC = true;\`

2. **Force GC when needed**: \`jsGarbageCollect();\`

3. **Close resources explicitly**:
   - \`ImageWindow.forceClose()\` when done with windows
   - \`File.close()\` after file operations

4. **Avoid large intermediate objects** - process in chunks for large images

5. **Use beginProcess/endProcess** for efficient undo management`,

      ui: `# PJSR UI Best Practices

1. **Use the __base__ pattern for inheritance**:
   \`\`\`javascript
   function MyDialog() {
       this.__base__ = Dialog;
       this.__base__();
   }
   MyDialog.prototype = new Dialog;
   \`\`\`

2. **Use Sizers for layout** instead of absolute positioning

3. **Use DPI-scaled dimensions**: \`setScaledFixedWidth()\` instead of \`setFixedWidth()\`

4. **Provide tooltips** for all interactive controls

5. **Use SectionBar** for collapsible sections

6. **Handle dialog events properly**: onExecute, onReturn`,

      processing: `# PJSR Image Processing Best Practices

1. **Always use beginProcess/endProcess**:
   \`\`\`javascript
   view.beginProcess(UndoFlag_PixelData);
   try {
       // processing
   } finally {
       view.endProcess();
   }
   \`\`\`

2. **Check for null references**:
   \`\`\`javascript
   if (window.isNull) return;
   \`\`\`

3. **Use appropriate UndoFlags**:
   - UndoFlag_PixelData for pixel modifications
   - UndoFlag_Keywords for metadata changes
   - UndoFlag_All for comprehensive undo

4. **Handle mask state** before processing

5. **Use console output** for progress feedback`,

      errors: `# PJSR Error Handling

1. **Use try-catch blocks**:
   \`\`\`javascript
   try {
       // risky operation
   } catch (error) {
       console.criticalln("Error: " + error.message);
   }
   \`\`\`

2. **Validate inputs early**:
   - Check ImageWindow.isNull
   - Validate file paths with File.exists()
   - Check array lengths before access

3. **Use console methods appropriately**:
   - console.writeln() for normal output
   - console.warning() for warnings
   - console.criticalln() for errors

4. **Clean up on errors**: Close files, windows in finally blocks`
    };

    if (topic && practices[topic]) {
      return practices[topic];
    }

    return {
      availableTopics: Object.keys(practices),
      overview: `PJSR Best Practices Guide

Available topics:
- general: General scripting guidelines
- memory: Memory management
- ui: User interface development
- processing: Image processing
- errors: Error handling

Use pjsr_best_practices with a specific topic for detailed information.`
    };
  }

  generateQuickReference() {
    return `# PJSR Quick Reference

## Core Classes

### Image
Primary image manipulation class.
- \`new Image(width, height, channels, colorSpace, bits)\`
- \`sample(x, y, channel)\` - Get pixel value
- \`apply(value, operation)\` - Apply arithmetic/blend operation
- \`mean()\`, \`median()\`, \`stdDev()\` - Statistics

### ImageWindow
Interface to PixInsight image windows.
- \`ImageWindow.activeWindow\` - Current active window
- \`ImageWindow.open(path)\` - Open image file
- \`mainView\` / \`currentView\` - Access views
- \`save()\` / \`saveAs(path)\` - Save image

### View
Interface to image views (main or preview).
- \`image\` - Get Image object
- \`beginProcess(undoFlags)\` - Start processing
- \`endProcess()\` - End processing

### ProcessInstance
Execute PixInsight processes programmatically.
- \`new ProcessInstance("ProcessId")\`
- \`ProcessInstance.fromIcon("iconId")\`
- \`executeOn(view)\` / \`executeGlobal()\`

## UI Controls

### Dialog
- \`execute()\` - Show modal dialog
- \`ok()\` / \`cancel()\` - Close dialog

### Common Controls
- \`Label\`, \`PushButton\`, \`CheckBox\`, \`Edit\`
- \`ComboBox\`, \`SpinBox\`, \`Slider\`
- \`TreeBox\`, \`ViewList\`, \`GroupBox\`

### Layout
- \`Sizer(vertical)\` - Layout manager
- \`add(control, stretch)\` - Add to sizer
- \`addSpacing(size)\` / \`addStretch()\`

## Common Constants

### Image Operations
\`ImageOp_Add\`, \`ImageOp_Sub\`, \`ImageOp_Mul\`, \`ImageOp_Div\`

### Color Spaces
\`ColorSpace_Gray\`, \`ColorSpace_RGB\`

### Undo Flags
\`UndoFlag_PixelData\`, \`UndoFlag_Keywords\`, \`UndoFlag_All\`

## Preprocessor Directives
- \`#include <pjsr/File.jsh>\`
- \`#feature-id ScriptName\`
- \`#iflt __PI_VERSION__ 01.08.00\`
`;
  }

  errorResponse(id, code, message) {
    return {
      jsonrpc: '2.0',
      id,
      error: { code, message }
    };
  }

  async run() {
    const rl = createInterface({
      input: process.stdin,
      output: process.stdout,
      terminal: false
    });

    let buffer = '';

    rl.on('line', (line) => {
      buffer += line;
      try {
        const request = JSON.parse(buffer);
        buffer = '';
        const response = this.handleRequest(request);
        console.log(JSON.stringify(response));
      } catch (e) {
        // Incomplete JSON, wait for more data
        if (e instanceof SyntaxError) {
          return;
        }
        console.error(JSON.stringify({
          jsonrpc: '2.0',
          id: null,
          error: { code: -32700, message: 'Parse error' }
        }));
        buffer = '';
      }
    });

    rl.on('close', () => {
      process.exit(0);
    });
  }
}

// Run the server
const server = new PJSRMCPServer();
server.run();
