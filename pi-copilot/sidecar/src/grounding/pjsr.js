import { PJSRAnalyzer } from 'pjsr-parser-server/src/pjsr-parser.js';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const DEFAULT_CATALOG = join(here, '..', '..', 'data', 'process-catalog.json');

export function createGrounding({ catalogPath = DEFAULT_CATALOG } = {}) {
  const analyzer = new PJSRAnalyzer();
  const catalog = JSON.parse(readFileSync(catalogPath, 'utf8'));

  return {
    validate(code) {
      const { valid, diagnostics } = analyzer.validate(code);
      const errors = diagnostics
        .filter(d => d.severity === 'error')
        .map(d => ({ message: d.message, line: d.line }));
      return { valid, errors };
    },

    listProcesses() {
      return Object.values(catalog.processes).map(p => ({ id: p.id, summary: p.summary || '' }));
    },

    describeProcess(id) {
      const p = catalog.processes[id];
      if (!p) return { error: `Unknown process: ${id}` };
      return { id: p.id, summary: p.summary || '', params: p.params, template: p.template };
    },
  };
}
