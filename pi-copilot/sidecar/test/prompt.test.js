import { test } from 'node:test';
import assert from 'node:assert/strict';
import { buildSystemPrompt } from '../src/prompt/system.js';

const fakeGrounding = {
  listProcesses: () => [
    { id: 'SCNR', summary: 'remove green cast' },
    { id: 'HistogramTransformation', summary: 'stretch' },
  ],
};

test('prompt lists catalog processes and core idioms', () => {
  const p = buildSystemPrompt(fakeGrounding);
  assert.ok(p.includes('SCNR'));
  assert.ok(p.includes('HistogramTransformation'));
  assert.ok(p.includes('run_pjsr'));
  assert.ok(p.includes('describe_process'));
  assert.ok(p.includes('executeOn'));
  assert.ok(p.includes('activeWindow'));
});
