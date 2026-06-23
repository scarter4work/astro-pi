export const TOOLS = [
  { type: 'function', function: { name: 'list_processes',
      description: 'List available PixInsight processes.',
      parameters: { type: 'object', properties: {} } } },
  { type: 'function', function: { name: 'describe_process',
      description: 'Get parameters and a code template for a process by id.',
      parameters: { type: 'object', properties: { id: { type: 'string' } }, required: ['id'] } } },
  { type: 'function', function: { name: 'get_view_context',
      description: 'Get the active view id, geometry, channel stats, FITS keywords, and process history.',
      parameters: { type: 'object', properties: {} } } },
  { type: 'function', function: { name: 'run_pjsr',
      description: 'Run PJSR against the active view. Requires user confirmation.',
      parameters: { type: 'object', properties: { code: { type: 'string' } }, required: ['code'] } } },
];

export function createSessionStore() { return new Map(); }

function getSession(store, sessionId, systemPrompt) {
  let s = store.get(sessionId);
  if (!s) {
    s = { messages: [{ role: 'system', content: systemPrompt }], retries: 0, pending: null };
    store.set(sessionId, s);
  }
  return s;
}

export async function chat(store, sessionId, message, deps) {
  const s = getSession(store, sessionId, deps.systemPrompt);
  s.messages.push({ role: 'user', content: message });
  s.retries = 0;
  return runUntilClient(s, deps);
}

export async function cont(store, sessionId, toolResult, deps) {
  const s = store.get(sessionId);
  if (!s) throw new Error(`Unknown session: ${sessionId}`);
  if (!s.pending) throw new Error('No pending tool call to continue');
  s.messages.push({ role: 'tool', name: s.pending, content: JSON.stringify(toolResult) });
  s.pending = null;
  return runUntilClient(s, deps);
}

async function runUntilClient(s, deps) {
  const { backend, grounding, maxRetries = 3 } = deps;
  for (let guard = 0; guard < 20; guard++) {
    const resp = await backend.chat(s.messages, TOOLS);
    const hasCalls = resp.toolCalls && resp.toolCalls.length > 0;
    const assistantMsg = { role: 'assistant', content: resp.content || '' };
    if (hasCalls) assistantMsg.tool_calls = resp.toolCalls.map(tc => ({ function: { name: tc.name, arguments: tc.arguments } }));
    s.messages.push(assistantMsg);

    if (!hasCalls) {
      return { type: 'message', text: resp.content || '' };
    }

    const call = resp.toolCalls[0]; // v1: one tool per step

    if (call.name === 'list_processes') {
      s.messages.push({ role: 'tool', name: 'list_processes', content: JSON.stringify(grounding.listProcesses()) });
      continue;
    }
    if (call.name === 'describe_process') {
      s.messages.push({ role: 'tool', name: 'describe_process', content: JSON.stringify(grounding.describeProcess(call.arguments.id)) });
      continue;
    }
    if (call.name === 'get_view_context') {
      s.pending = 'get_view_context';
      return { type: 'tool_use', tool: 'get_view_context' };
    }
    if (call.name === 'run_pjsr') {
      const code = call.arguments.code || '';
      const { valid, errors } = grounding.validate(code);
      if (valid) {
        s.pending = 'run_pjsr';
        s.retries = 0;
        return { type: 'tool_use', tool: 'run_pjsr', code };
      }
      if (s.retries >= maxRetries) {
        const errText = errors.map(e => `line ${e.line}: ${e.message}`).join('; ');
        return { type: 'message', text: `I couldn't produce valid PJSR after ${maxRetries} attempt(s). Last validation error: ${errText}` };
      }
      s.retries += 1;
      s.messages.push({ role: 'tool', name: 'run_pjsr', content: JSON.stringify({ validationFailed: true, errors }) });
      s.messages.push({ role: 'user', content: 'The PJSR you produced failed validation. Fix the errors above and call run_pjsr again with corrected code.' });
      continue;
    }

    // Unknown tool: feed an error result back to the model and continue.
    s.messages.push({ role: 'tool', name: call.name, content: JSON.stringify({ error: `Unknown tool: ${call.name}` }) });
  }
  return { type: 'message', text: 'Stopped: too many internal steps without a result.' };
}
