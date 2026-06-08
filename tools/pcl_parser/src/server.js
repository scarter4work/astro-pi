/**
 * PCL Parser HTTP Server
 *
 * REST API for PCL C++ code analysis and documentation.
 *
 * Endpoints:
 *   GET  /                    - Server info and API documentation
 *   POST /analyze             - Analyze PCL C++ code
 *   POST /completions         - Get code completions at position
 *   POST /hover               - Get documentation for symbol
 *   POST /validate            - Validate PCL code
 *   GET  /templates           - List code templates
 *   GET  /templates/:name     - Get specific template
 *   GET  /classes             - List all PCL classes
 *   GET  /classes/:name       - Get detailed class info
 *   GET  /constants           - List constant categories
 *   GET  /constants/:category - Get constants by category
 *   GET  /schema              - Full PCL API schema
 *   GET  /best-practices      - Get best practices guide
 *   GET  /headers             - List PCL header files
 */

import { createServer } from 'http';
import { PCLAnalyzer } from './pcl-parser.js';
import { readFileSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

const PORT = process.env.PORT || 3848;

// Load schemas
let coreSchema = {};
let extendedSchema = {};

try {
  coreSchema = JSON.parse(readFileSync(join(__dirname, '../schemas/pcl-core.json'), 'utf8'));
  extendedSchema = JSON.parse(readFileSync(join(__dirname, '../schemas/pcl-extended.json'), 'utf8'));
} catch (e) {
  console.error('Warning: Could not load schemas:', e.message);
}

const analyzer = new PCLAnalyzer();

/**
 * Parse JSON request body
 */
async function parseBody(req) {
  return new Promise((resolve, reject) => {
    let body = '';
    req.on('data', chunk => body += chunk);
    req.on('end', () => {
      try {
        resolve(body ? JSON.parse(body) : {});
      } catch (e) {
        reject(new Error('Invalid JSON'));
      }
    });
    req.on('error', reject);
  });
}

/**
 * Send JSON response
 */
function sendJSON(res, data, status = 200) {
  res.writeHead(status, {
    'Content-Type': 'application/json',
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
    'Access-Control-Allow-Headers': 'Content-Type'
  });
  res.end(JSON.stringify(data, null, 2));
}

/**
 * Send error response
 */
function sendError(res, message, status = 400) {
  sendJSON(res, { error: message }, status);
}

/**
 * Route handlers
 */
const routes = {
  'GET /': (req, res) => {
    sendJSON(res, {
      name: 'PCL Parser Server',
      version: '1.0.0',
      description: 'PixInsight Class Library (PCL) C++ code analysis server',
      documentation: 'https://pixinsight.com/developer/pcl/',
      repository: 'https://gitlab.com/pixinsight/PCL',
      endpoints: {
        'GET /': 'Server info and API documentation',
        'POST /analyze': 'Analyze PCL C++ code structure',
        'POST /completions': 'Get code completions at position',
        'POST /hover': 'Get documentation for a symbol',
        'POST /validate': 'Validate PCL code',
        'GET /templates': 'List available code templates',
        'GET /templates/:name': 'Get specific template',
        'GET /classes': 'List all PCL classes',
        'GET /classes/:name': 'Get detailed class information',
        'GET /constants': 'List constant categories',
        'GET /constants/:category': 'Get constants by category',
        'GET /schema': 'Full PCL API schema',
        'GET /best-practices': 'Get best practices guide',
        'GET /best-practices/:topic': 'Get topic-specific best practices',
        'GET /headers': 'List PCL header files'
      }
    });
  },

  'POST /analyze': async (req, res) => {
    try {
      const { code } = await parseBody(req);
      if (!code) {
        return sendError(res, 'Missing "code" field in request body');
      }
      const result = analyzer.analyze(code);
      sendJSON(res, result);
    } catch (e) {
      sendError(res, e.message);
    }
  },

  'POST /completions': async (req, res) => {
    try {
      const { code, line, column } = await parseBody(req);
      if (!code) {
        return sendError(res, 'Missing "code" field');
      }
      if (typeof line !== 'number' || typeof column !== 'number') {
        return sendError(res, 'Missing or invalid "line" and "column" fields');
      }
      const completions = analyzer.getCompletions(code, line, column);
      sendJSON(res, { completions });
    } catch (e) {
      sendError(res, e.message);
    }
  },

  'POST /hover': async (req, res) => {
    try {
      const { symbol } = await parseBody(req);
      if (!symbol) {
        return sendError(res, 'Missing "symbol" field');
      }
      const info = analyzer.getHoverInfo(symbol);
      if (info) {
        sendJSON(res, { markdown: info });
      } else {
        sendJSON(res, { markdown: null, message: 'Symbol not found' });
      }
    } catch (e) {
      sendError(res, e.message);
    }
  },

  'POST /validate': async (req, res) => {
    try {
      const { code } = await parseBody(req);
      if (!code) {
        return sendError(res, 'Missing "code" field');
      }
      const result = analyzer.validate(code);
      sendJSON(res, result);
    } catch (e) {
      sendError(res, e.message);
    }
  },

  'GET /templates': (req, res) => {
    const templates = analyzer.getTemplates();
    const list = Object.entries(templates).map(([id, t]) => ({
      id,
      name: t.name,
      description: t.description
    }));
    sendJSON(res, { templates: list });
  },

  'GET /classes': (req, res) => {
    const classes = analyzer.listClasses();
    sendJSON(res, { classes, count: classes.length });
  },

  'GET /constants': (req, res) => {
    const categories = Object.keys(coreSchema.constants || {});
    sendJSON(res, { categories });
  },

  'GET /schema': (req, res) => {
    sendJSON(res, {
      core: coreSchema,
      extended: extendedSchema
    });
  },

  'GET /headers': (req, res) => {
    const headers = analyzer.getPCLHeaders();
    sendJSON(res, { headers, count: headers.length });
  },

  'GET /best-practices': (req, res) => {
    const topics = ['general', 'imageProcessing', 'threading', 'ui'];
    const practices = {};
    topics.forEach(t => {
      practices[t] = analyzer.getBestPractices(t);
    });
    sendJSON(res, { topics, practices });
  }
};

/**
 * Handle dynamic routes
 */
function handleDynamicRoute(method, path, req, res) {
  // GET /templates/:name
  if (method === 'GET' && path.startsWith('/templates/')) {
    const name = path.slice('/templates/'.length);
    const templates = analyzer.getTemplates();
    if (templates[name]) {
      sendJSON(res, templates[name]);
    } else {
      sendError(res, `Template "${name}" not found`, 404);
    }
    return true;
  }

  // GET /classes/:name
  if (method === 'GET' && path.startsWith('/classes/')) {
    const name = path.slice('/classes/'.length);
    const classInfo = analyzer.getClassInfo(name);
    if (classInfo) {
      sendJSON(res, { name, ...classInfo });
    } else {
      sendError(res, `Class "${name}" not found`, 404);
    }
    return true;
  }

  // GET /constants/:category
  if (method === 'GET' && path.startsWith('/constants/')) {
    const category = path.slice('/constants/'.length);
    const constants = coreSchema.constants?.[category];
    if (constants) {
      sendJSON(res, { category, constants });
    } else {
      sendError(res, `Category "${category}" not found`, 404);
    }
    return true;
  }

  // GET /best-practices/:topic
  if (method === 'GET' && path.startsWith('/best-practices/')) {
    const topic = path.slice('/best-practices/'.length);
    const practices = analyzer.getBestPractices(topic);
    sendJSON(res, { topic, content: practices });
    return true;
  }

  return false;
}

/**
 * Main request handler
 */
const server = createServer(async (req, res) => {
  const method = req.method;
  const url = new URL(req.url, `http://localhost:${PORT}`);
  const path = url.pathname;

  // Handle CORS preflight
  if (method === 'OPTIONS') {
    res.writeHead(204, {
      'Access-Control-Allow-Origin': '*',
      'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
      'Access-Control-Allow-Headers': 'Content-Type'
    });
    res.end();
    return;
  }

  // Try static routes
  const routeKey = `${method} ${path}`;
  if (routes[routeKey]) {
    try {
      await routes[routeKey](req, res);
    } catch (e) {
      sendError(res, e.message, 500);
    }
    return;
  }

  // Try dynamic routes
  if (handleDynamicRoute(method, path, req, res)) {
    return;
  }

  // 404 Not Found
  sendError(res, `Endpoint not found: ${method} ${path}`, 404);
});

server.listen(PORT, () => {
  console.log(`PCL Parser Server running at http://localhost:${PORT}`);
  console.log('');
  console.log('Available endpoints:');
  console.log('  GET  /             - Server info');
  console.log('  POST /analyze      - Analyze PCL C++ code');
  console.log('  POST /completions  - Get code completions');
  console.log('  POST /hover        - Get symbol documentation');
  console.log('  POST /validate     - Validate code');
  console.log('  GET  /templates    - List templates');
  console.log('  GET  /classes      - List PCL classes');
  console.log('  GET  /constants    - List constants');
  console.log('  GET  /schema       - Full API schema');
  console.log('  GET  /headers      - List PCL headers');
  console.log('');
});
