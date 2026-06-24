// Phase 0 GATE — drives the real model over a prompt battery.
// Success = each prompt yields a validated run_pjsr.
// Requires: Ollama running + OLLAMA_MODEL pulled (default qwen3-coder:30b).
// Run: node test/battery.js
import { createGrounding } from '../src/grounding/pjsr.js';
import { createOllamaBackend } from '../src/backends/ollama.js';
import { buildSystemPrompt } from '../src/prompt/system.js';
import { createSessionStore, chat, cont } from '../src/agent/loop.js';

const PROMPTS = [
  'Remove the green color cast from the active image.',
  'Apply a gentle nonlinear stretch to the active image.',
  'Subtract the background gradient from the active image.',
  'Neutralize the background color cast.',
  'Reduce noise with a multiscale transform.',
];

// Canned context for any get_view_context the model requests (no PI in Phase 0).
const FAKE_CONTEXT = {
  viewId: 'integration_RGB',
  geometry: { width: 4000, height: 3000, channels: 3 },
  channelStats: [{ median: 0.12, mad: 0.01 }, { median: 0.15, mad: 0.01 }, { median: 0.10, mad: 0.01 }],
  fitsKeywords: { IMAGETYP: 'Light Frame' },
  history: [],
};

async function runPrompt(deps, prompt) {
  const store = createSessionStore();
  const sid = 'battery';
  let r = await chat(store, sid, prompt, deps);
  for (let i = 0; i < 8; i++) {
    if (r.type === 'message') return { ok: false, reason: `ended with message: ${r.text.slice(0, 120)}` };
    if (r.type === 'tool_use' && r.tool === 'run_pjsr') return { ok: true, code: r.code };
    if (r.type === 'tool_use' && r.tool === 'get_view_context') {
      r = await cont(store, sid, FAKE_CONTEXT, deps);
      continue;
    }
    return { ok: false, reason: `unexpected result: ${JSON.stringify(r)}` };
  }
  return { ok: false, reason: 'no run_pjsr after 8 steps' };
}

async function main() {
  const grounding = createGrounding();
  const backend = createOllamaBackend({ model: process.env.OLLAMA_MODEL || 'qwen3-coder:30b', url: process.env.OLLAMA_URL || undefined });
  const deps = { backend, grounding, systemPrompt: buildSystemPrompt(grounding) };

  let pass = 0;
  for (const prompt of PROMPTS) {
    process.stdout.write(`- "${prompt}" ... `);
    try {
      const res = await runPrompt(deps, prompt);
      if (res.ok) { pass++; console.log('PASS'); }
      else console.log(`FAIL (${res.reason})`);
    } catch (e) {
      console.log(`ERROR (${e.message})`);
    }
  }
  console.log(`\n${pass}/${PROMPTS.length} prompts produced validated PJSR.`);
  process.exit(pass === PROMPTS.length ? 0 : 1);
}

main();
