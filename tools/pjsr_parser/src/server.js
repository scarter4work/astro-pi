#!/usr/bin/env node
/**
 * PJSR Parser HTTP Server
 *
 * A simple HTTP server that provides PJSR parsing and analysis capabilities.
 * Can be used as a standalone service or integrated with other tools.
 */

import { createServer } from 'http';
import { PJSRAnalyzer, pjsrSchema } from './pjsr-parser.js';

const analyzer = new PJSRAnalyzer();
const PORT = process.env.PORT || 3847;

/**
 * Parse JSON body from request
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
function sendJSON(res, statusCode, data) {
  res.writeHead(statusCode, {
    'Content-Type': 'application/json',
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
    'Access-Control-Allow-Headers': 'Content-Type'
  });
  res.end(JSON.stringify(data, null, 2));
}

/**
 * Route handlers
 */
const routes = {
  'GET /': (req, res) => {
    sendJSON(res, 200, {
      name: 'PJSR Parser Server',
      version: '1.0.0',
      description: 'Language parser server for PixInsight JavaScript Runtime',
      endpoints: {
        'POST /analyze': 'Analyze PJSR code',
        'POST /completions': 'Get code completions',
        'POST /hover': 'Get hover information for a symbol',
        'POST /validate': 'Validate PJSR code',
        'GET /templates': 'Get all code templates',
        'GET /templates/:name': 'Get specific template',
        'GET /classes': 'List all PJSR classes',
        'GET /classes/:name': 'Get class information',
        'GET /constants': 'List all constant categories',
        'GET /constants/:category': 'Get constants by category',
        'GET /schema': 'Get full PJSR schema'
      }
    });
  },

  'POST /analyze': async (req, res) => {
    try {
      const { code } = await parseBody(req);
      if (!code) {
        return sendJSON(res, 400, { error: 'Missing required field: code' });
      }
      const result = analyzer.analyze(code);
      sendJSON(res, 200, result);
    } catch (e) {
      sendJSON(res, 500, { error: e.message });
    }
  },

  'POST /completions': async (req, res) => {
    try {
      const { code, line, column } = await parseBody(req);
      if (!code || line === undefined || column === undefined) {
        return sendJSON(res, 400, { error: 'Missing required fields: code, line, column' });
      }
      const result = analyzer.getCompletions(code, line, column);
      sendJSON(res, 200, { completions: result });
    } catch (e) {
      sendJSON(res, 500, { error: e.message });
    }
  },

  'POST /hover': async (req, res) => {
    try {
      const { symbol } = await parseBody(req);
      if (!symbol) {
        return sendJSON(res, 400, { error: 'Missing required field: symbol' });
      }
      const result = analyzer.getHoverInfo(symbol);
      if (result) {
        sendJSON(res, 200, { content: result });
      } else {
        sendJSON(res, 404, { error: `No documentation found for: ${symbol}` });
      }
    } catch (e) {
      sendJSON(res, 500, { error: e.message });
    }
  },

  'POST /validate': async (req, res) => {
    try {
      const { code } = await parseBody(req);
      if (!code) {
        return sendJSON(res, 400, { error: 'Missing required field: code' });
      }
      const result = analyzer.validate(code);
      sendJSON(res, 200, result);
    } catch (e) {
      sendJSON(res, 500, { error: e.message });
    }
  },

  'GET /templates': (req, res) => {
    const templates = analyzer.getTemplates();
    sendJSON(res, 200, {
      templates: Object.keys(templates).map(name => ({
        name,
        description: templates[name].description
      }))
    });
  },

  'GET /classes': (req, res) => {
    const coreClasses = Object.keys(pjsrSchema.coreClasses || {}).map(name => ({
      name,
      type: 'core',
      description: pjsrSchema.coreClasses[name].description
    }));
    const uiControls = Object.keys(pjsrSchema.uiControls || {}).map(name => ({
      name,
      type: 'ui',
      description: pjsrSchema.uiControls[name].description
    }));
    sendJSON(res, 200, { classes: [...coreClasses, ...uiControls] });
  },

  'GET /constants': (req, res) => {
    const categories = Object.keys(pjsrSchema.constants || {}).map(name => ({
      name,
      count: Object.keys(pjsrSchema.constants[name]).length
    }));
    sendJSON(res, 200, { categories });
  },

  'GET /schema': (req, res) => {
    sendJSON(res, 200, pjsrSchema);
  }
};

/**
 * Dynamic route handler for parameterized routes
 */
function handleDynamicRoute(req, res, path) {
  // GET /templates/:name
  const templateMatch = path.match(/^\/templates\/(.+)$/);
  if (templateMatch && req.method === 'GET') {
    const templates = analyzer.getTemplates();
    const template = templates[templateMatch[1]];
    if (template) {
      return sendJSON(res, 200, template);
    }
    return sendJSON(res, 404, {
      error: `Template not found: ${templateMatch[1]}`,
      available: Object.keys(templates)
    });
  }

  // GET /classes/:name
  const classMatch = path.match(/^\/classes\/(.+)$/);
  if (classMatch && req.method === 'GET') {
    const className = classMatch[1];
    const classInfo = pjsrSchema.coreClasses?.[className] ||
                      pjsrSchema.uiControls?.[className] ||
                      pjsrSchema.globalObjects?.[className];
    if (classInfo) {
      return sendJSON(res, 200, { name: className, ...classInfo });
    }
    return sendJSON(res, 404, {
      error: `Class not found: ${className}`,
      availableClasses: [
        ...Object.keys(pjsrSchema.coreClasses || {}),
        ...Object.keys(pjsrSchema.uiControls || {})
      ]
    });
  }

  // GET /constants/:category
  const constantsMatch = path.match(/^\/constants\/(.+)$/);
  if (constantsMatch && req.method === 'GET') {
    const category = constantsMatch[1];
    const constants = pjsrSchema.constants?.[category];
    if (constants) {
      return sendJSON(res, 200, { category, constants });
    }
    return sendJSON(res, 404, {
      error: `Category not found: ${category}`,
      availableCategories: Object.keys(pjsrSchema.constants || {})
    });
  }

  return null;
}

/**
 * Create and start the server
 */
const server = createServer(async (req, res) => {
  // Handle CORS preflight
  if (req.method === 'OPTIONS') {
    res.writeHead(204, {
      'Access-Control-Allow-Origin': '*',
      'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
      'Access-Control-Allow-Headers': 'Content-Type'
    });
    return res.end();
  }

  const url = new URL(req.url, `http://localhost:${PORT}`);
  const path = url.pathname;
  const routeKey = `${req.method} ${path}`;

  // Check static routes
  if (routes[routeKey]) {
    return routes[routeKey](req, res);
  }

  // Check dynamic routes
  const handled = handleDynamicRoute(req, res, path);
  if (handled !== null) {
    return;
  }

  // 404
  sendJSON(res, 404, { error: 'Not found', path });
});

server.listen(PORT, () => {
  console.log(`PJSR Parser Server running on http://localhost:${PORT}`);
  console.log('\nAvailable endpoints:');
  console.log('  GET  /              - Server info and API documentation');
  console.log('  POST /analyze       - Analyze PJSR code');
  console.log('  POST /completions   - Get code completions');
  console.log('  POST /hover         - Get symbol documentation');
  console.log('  POST /validate      - Validate PJSR code');
  console.log('  GET  /templates     - List code templates');
  console.log('  GET  /templates/:n  - Get specific template');
  console.log('  GET  /classes       - List all classes');
  console.log('  GET  /classes/:n    - Get class info');
  console.log('  GET  /constants     - List constant categories');
  console.log('  GET  /constants/:c  - Get constants by category');
  console.log('  GET  /schema        - Full PJSR schema');
});
