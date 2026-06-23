import { test } from 'node:test';
import assert from 'node:assert/strict';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { createGrounding } from '../src/grounding/pjsr.js';

const here = dirname(fileURLToPath(import.meta.url));
const CATALOG = join(here, 'fixtures', 'process-catalog.json');
const make = () => createGrounding({ catalogPath: CATALOG });

test('validate accepts well-formed PJSR', () => {
  const { valid, errors } = make().validate('var P = new SCNR;\nP.amount = 1;\nP.executeOn(view, false);');
  assert.equal(valid, true);
  assert.deepEqual(errors, []);
});

test('validate rejects mismatched beginProcess/endProcess and returns errors', () => {
  const { valid, errors } = make().validate('view.beginProcess();');
  assert.equal(valid, false);
  assert.ok(errors.length >= 1);
  assert.ok(typeof errors[0].message === 'string');
});

test('listProcesses returns {id, summary} entries including SCNR', () => {
  const list = make().listProcesses();
  assert.ok(Array.isArray(list));
  assert.ok(list.some(p => p.id === 'SCNR' && typeof p.summary === 'string'));
});

test('describeProcess returns params + template for a known id', () => {
  const d = make().describeProcess('SCNR');
  assert.equal(d.id, 'SCNR');
  assert.ok(d.params && typeof d.params === 'object');
  assert.ok(typeof d.template === 'string' && d.template.includes('new SCNR'));
});

test('describeProcess returns an error for an unknown id', () => {
  const d = make().describeProcess('NotARealProcess');
  assert.ok(d.error);
});
