export function createOllamaBackend({ model, url = 'http://localhost:11434', fetchImpl = fetch } = {}) {
  if (!model) throw new Error('createOllamaBackend: model is required');
  return {
    name: 'ollama',
    model,
    async chat(messages, tools) {
      const res = await fetchImpl(`${url}/api/chat`, {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({ model, messages, tools, stream: false }),
      });
      if (!res.ok) {
        const body = await res.text();
        throw new Error(`Ollama HTTP ${res.status}: ${body}`);
      }
      const data = await res.json();
      const msg = data.message || {};
      const toolCalls = (msg.tool_calls || []).map(tc => ({
        name: tc.function?.name,
        arguments: tc.function?.arguments || {},
      }));
      return { content: msg.content || '', toolCalls };
    },
  };
}
