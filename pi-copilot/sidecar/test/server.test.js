import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createServer } from '../src/server.js';

const stubGrounding = {
  validate: () => ({ valid: true, errors: [] }),
  listProcesses: () => [{ id: 'SCNR', summary: 's' }],
  describeProcess: (id) => ({ id, params: {}, template: 't' }),
};

function start(deps) {
  return new Promise((resolve) => {
    const srv = createServer(deps);
    srv.listen(0, '127.0.0.1', () => resolve({ srv, port: srv.address().port }));
  });
}

test('GET /health reports model and backend', async () => {
  const backend = { name: 'mock', model: 'qwen3-coder:30b', async chat() { return { content: '', toolCalls: [] }; } };
  const { srv, port } = await start({ backend, grounding: stubGrounding, systemPrompt: 'sys' });
  const res = await fetch(`http://127.0.0.1:${port}/health`);
  const body = await res.json();
  assert.deepEqual(body, { ok: true, model: 'qwen3-coder:30b', backend: 'mock' });
  srv.close();
});

test('POST /chat returns a message Result', async () => {
  const backend = { name: 'mock', model: 'm', async chat() { return { content: 'hi there', toolCalls: [] }; } };
  const { srv, port } = await start({ backend, grounding: stubGrounding, systemPrompt: 'sys' });
  const res = await fetch(`http://127.0.0.1:${port}/chat`, {
    method: 'POST', headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ sessionId: 'a', message: 'yo' }),
  });
  assert.deepEqual(await res.json(), { type: 'message', text: 'hi there' });
  srv.close();
});

test('unknown route returns 404', async () => {
  const backend = { name: 'mock', model: 'm', async chat() { return { content: '', toolCalls: [] }; } };
  const { srv, port } = await start({ backend, grounding: stubGrounding, systemPrompt: 'sys' });
  const res = await fetch(`http://127.0.0.1:${port}/nope`);
  assert.equal(res.status, 404);
  srv.close();
});
