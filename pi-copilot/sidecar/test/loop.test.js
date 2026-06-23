import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createSessionStore, chat, cont } from '../src/agent/loop.js';

// Mock backend returns scripted {content, toolCalls} in order.
function mockBackend(scripted) {
  let i = 0;
  return { name: 'mock', model: 'mock', async chat() {
    if (i >= scripted.length) throw new Error('mock backend exhausted');
    return scripted[i++];
  } };
}
const baseGrounding = {
  validate: () => ({ valid: true, errors: [] }),
  listProcesses: () => [{ id: 'SCNR', summary: 's' }],
  describeProcess: (id) => ({ id, summary: 's', params: {}, template: 't' }),
};
const deps = (backend, grounding = baseGrounding) => ({ backend, grounding, systemPrompt: 'sys' });

test('plain assistant message ends the turn', async () => {
  const store = createSessionStore();
  const r = await chat(store, 's1', 'hello', deps(mockBackend([{ content: 'hi', toolCalls: [] }])));
  assert.deepEqual(r, { type: 'message', text: 'hi' });
});

test('grounding tool (describe_process) is resolved in-process, then turn continues', async () => {
  let called = null;
  const grounding = { ...baseGrounding, describeProcess: (id) => { called = id; return { id, params: {}, template: 't' }; } };
  const backend = mockBackend([
    { content: '', toolCalls: [{ name: 'describe_process', arguments: { id: 'SCNR' } }] },
    { content: 'done', toolCalls: [] },
  ]);
  const store = createSessionStore();
  const r = await chat(store, 's2', 'tell me about SCNR', deps(backend, grounding));
  assert.equal(called, 'SCNR');
  assert.deepEqual(r, { type: 'message', text: 'done' });
});

test('valid run_pjsr is surfaced to the client', async () => {
  const backend = mockBackend([{ content: '', toolCalls: [{ name: 'run_pjsr', arguments: { code: 'CODE' } }] }]);
  const store = createSessionStore();
  const r = await chat(store, 's3', 'do it', deps(backend));
  assert.deepEqual(r, { type: 'tool_use', tool: 'run_pjsr', code: 'CODE' });
});

test('invalid run_pjsr self-corrects then surfaces valid code', async () => {
  let n = 0;
  const grounding = { ...baseGrounding, validate: () => (n++ === 0 ? { valid: false, errors: [{ message: 'bad', line: 1 }] } : { valid: true, errors: [] }) };
  const backend = mockBackend([
    { content: '', toolCalls: [{ name: 'run_pjsr', arguments: { code: 'BAD' } }] },
    { content: '', toolCalls: [{ name: 'run_pjsr', arguments: { code: 'GOOD' } }] },
  ]);
  const store = createSessionStore();
  const r = await chat(store, 's4', 'do it', deps(backend, grounding));
  assert.deepEqual(r, { type: 'tool_use', tool: 'run_pjsr', code: 'GOOD' });
});

test('retry exhaustion surfaces the real validation error as a message', async () => {
  const grounding = { ...baseGrounding, validate: () => ({ valid: false, errors: [{ message: 'still bad', line: 3 }] }) };
  const backend = mockBackend([
    { content: '', toolCalls: [{ name: 'run_pjsr', arguments: { code: 'BAD1' } }] },
    { content: '', toolCalls: [{ name: 'run_pjsr', arguments: { code: 'BAD2' } }] },
  ]);
  const store = createSessionStore();
  const r = await chat(store, 's5', 'do it', { ...deps(backend, grounding), maxRetries: 1 });
  assert.equal(r.type, 'message');
  assert.ok(r.text.includes('still bad'));
});

test('get_view_context pauses, and cont feeds the result back', async () => {
  const backend = mockBackend([
    { content: '', toolCalls: [{ name: 'get_view_context', arguments: {} }] },
    { content: 'thanks', toolCalls: [] },
  ]);
  const store = createSessionStore();
  const r1 = await chat(store, 's6', 'inspect', deps(backend));
  assert.deepEqual(r1, { type: 'tool_use', tool: 'get_view_context' });
  const r2 = await cont(store, 's6', { viewId: 'V', channelStats: {} }, deps(backend));
  assert.deepEqual(r2, { type: 'message', text: 'thanks' });
});
