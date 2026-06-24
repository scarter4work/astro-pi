import { createServer as httpCreateServer } from 'node:http';
import { createGrounding } from './grounding/pjsr.js';
import { createOllamaBackend } from './backends/ollama.js';
import { buildSystemPrompt } from './prompt/system.js';
import { createSessionStore, chat, cont } from './agent/loop.js';

export function createServer(overrides = {}) {
  const grounding = overrides.grounding || createGrounding();
  const backend = overrides.backend || createOllamaBackend({
    model: process.env.OLLAMA_MODEL || 'qwen3-coder:30b',
    url: process.env.OLLAMA_URL || undefined,
  });
  const systemPrompt = overrides.systemPrompt || buildSystemPrompt(grounding);
  const store = createSessionStore();
  const deps = { backend, grounding, systemPrompt };

  return httpCreateServer(async (req, res) => {
    const send = (code, obj) => {
      res.writeHead(code, { 'content-type': 'application/json' });
      res.end(JSON.stringify(obj));
    };
    try {
      if (req.method === 'GET' && req.url === '/health') {
        return send(200, { ok: true, model: backend.model, backend: backend.name });
      }
      if (req.method === 'POST' && req.url === '/chat') {
        const body = await readJson(req);
        return send(200, await chat(store, body.sessionId, body.message, deps));
      }
      if (req.method === 'POST' && req.url === '/continue') {
        const body = await readJson(req);
        return send(200, await cont(store, body.sessionId, body.toolResult, deps));
      }
      send(404, { error: 'not found' });
    } catch (e) {
      send(e.statusCode || 500, { error: String((e && e.message) || e) });
    }
  });
}

const MAX_BODY = 1_000_000; // 1 MB

function readJson(req) {
  return new Promise((resolve, reject) => {
    let data = '';
    let rejected = false;
    req.on('data', (c) => {
      if (rejected) return;
      data += c;
      if (data.length > MAX_BODY) {
        rejected = true;
        const err = new Error('Request body too large');
        err.statusCode = 413;
        // Drain remaining data so the socket stays open for the 413 response.
        req.resume();
        reject(err);
      }
    });
    req.on('end', () => {
      if (rejected) return;
      try { resolve(data ? JSON.parse(data) : {}); } catch (e) { reject(e); }
    });
    req.on('error', reject);
  });
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const port = Number(process.env.PORT || 8765);
  createServer().listen(port, '127.0.0.1', () => console.log(`PI Copilot sidecar on http://localhost:${port}`));
}
