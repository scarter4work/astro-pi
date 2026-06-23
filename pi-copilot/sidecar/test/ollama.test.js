import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createOllamaBackend } from '../src/backends/ollama.js';

function fakeFetch(captured, response) {
  return async (url, opts) => {
    captured.url = url;
    captured.body = JSON.parse(opts.body);
    return { ok: true, status: 200, async json() { return response; } };
  };
}

test('chat posts model+messages+tools with stream:false', async () => {
  const captured = {};
  const fetchImpl = fakeFetch(captured, { message: { content: 'hi', tool_calls: [] } });
  const b = createOllamaBackend({ model: 'qwen3-coder:30b', url: 'http://x:1', fetchImpl });
  await b.chat([{ role: 'user', content: 'yo' }], [{ type: 'function' }]);
  assert.equal(captured.url, 'http://x:1/api/chat');
  assert.equal(captured.body.model, 'qwen3-coder:30b');
  assert.equal(captured.body.stream, false);
  assert.equal(captured.body.messages[0].content, 'yo');
  assert.ok(Array.isArray(captured.body.tools));
});

test('chat normalizes tool_calls to {name, arguments}', async () => {
  const response = { message: { content: '', tool_calls: [
    { function: { name: 'run_pjsr', arguments: { code: 'x' } } },
  ] } };
  const b = createOllamaBackend({ model: 'm', fetchImpl: fakeFetch({}, response) });
  const out = await b.chat([], []);
  assert.equal(out.content, '');
  assert.deepEqual(out.toolCalls, [{ name: 'run_pjsr', arguments: { code: 'x' } }]);
});

test('chat throws on non-ok HTTP with the body in the message', async () => {
  const fetchImpl = async () => ({ ok: false, status: 500, async text() { return 'boom'; } });
  const b = createOllamaBackend({ model: 'm', fetchImpl });
  await assert.rejects(() => b.chat([], []), /Ollama HTTP 500: boom/);
});
