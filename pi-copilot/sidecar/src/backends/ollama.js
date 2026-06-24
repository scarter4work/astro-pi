// Recover tool calls a local model emits as TEXT when Ollama's structured
// parser misses them. qwen3-coder occasionally renders a call in its native
// textual form instead of populating message.tool_calls; without this a real
// tool call would silently vanish into a dead message (no silent failures).
export function parseTextualToolCalls(content) {
  const calls = [];
  if (!content) return calls;

  // Form A: <function=NAME> ...optional JSON args... </function>
  const reA = /<function=([A-Za-z0-9_]+)>([\s\S]*?)<\/function>/g;
  let m;
  while ((m = reA.exec(content)) !== null) {
    let args = {};
    const inner = m[2].trim();
    // Non-JSON inner text is intentionally dropped to {}: the loop then dispatches
    // the tool with empty args, gets a clear error back, and the model self-corrects
    // — preferable to throwing here and losing the recovered tool name.
    if (inner) { try { args = JSON.parse(inner); } catch (e) { /* leave {} */ } }
    calls.push({ name: m[1], arguments: args });
  }
  if (calls.length) return calls;

  // Form B: <tool_call>{ "name": ..., "arguments": ... }</tool_call>
  const reB = /<tool_call>\s*({[\s\S]*?})\s*<\/tool_call>/g;
  while ((m = reB.exec(content)) !== null) {
    try {
      const obj = JSON.parse(m[1]);
      if (obj && obj.name) calls.push({ name: obj.name, arguments: obj.arguments || {} });
    } catch (e) { /* ignore malformed */ }
  }
  return calls;
}

export function createOllamaBackend({ model, url = 'http://localhost:11434', fetchImpl = fetch } = {}) {
  if (!model) throw new Error('createOllamaBackend: model is required');
  return {
    name: 'ollama',
    model,
    async chat(messages, tools) {
      const res = await fetchImpl(`${url}/api/chat`, {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        // temperature 0 → deterministic generation + more reliable tool-calling.
        body: JSON.stringify({ model, messages, tools, stream: false, options: { temperature: 0 } }),
      });
      if (!res.ok) {
        const body = await res.text();
        throw new Error(`Ollama HTTP ${res.status}: ${body}`);
      }
      const data = await res.json();
      const msg = data.message || {};
      let toolCalls = (msg.tool_calls || []).map(tc => ({
        name: tc.function?.name,
        arguments: tc.function?.arguments || {},
      }));
      let content = msg.content || '';
      // Fallback: structured parse found nothing but the text carries a tool call.
      if (toolCalls.length === 0 && content) {
        const textual = parseTextualToolCalls(content);
        if (textual.length) { toolCalls = textual; content = ''; }
      }
      return { content, toolCalls };
    },
  };
}
