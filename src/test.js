/**
 * Tests for PJSR Parser
 */

import { test, describe } from 'node:test';
import assert from 'node:assert';
import { PJSRLexer, PJSRAnalyzer, TokenType, pjsrSchema } from './pjsr-parser.js';

describe('PJSRLexer', () => {
  test('tokenizes simple variable declaration', () => {
    const lexer = new PJSRLexer('var x = 5;');
    const tokens = lexer.tokenize();

    assert.ok(tokens.length > 0);
    assert.strictEqual(tokens[0].type, TokenType.KEYWORD);
    assert.strictEqual(tokens[0].value, 'var');
    assert.strictEqual(tokens[1].type, TokenType.IDENTIFIER);
    assert.strictEqual(tokens[1].value, 'x');
  });

  test('tokenizes preprocessor directives', () => {
    const lexer = new PJSRLexer('#include <pjsr/DataType.jsh>');
    const tokens = lexer.tokenize();

    assert.ok(tokens.some(t => t.type === TokenType.PREPROCESSOR));
  });

  test('tokenizes strings', () => {
    const lexer = new PJSRLexer('var s = "hello world";');
    const tokens = lexer.tokenize();

    assert.ok(tokens.some(t => t.type === TokenType.STRING && t.value === '"hello world"'));
  });

  test('tokenizes numbers', () => {
    const lexer = new PJSRLexer('var n = 3.14159;');
    const tokens = lexer.tokenize();

    assert.ok(tokens.some(t => t.type === TokenType.NUMBER && t.value === '3.14159'));
  });

  test('tokenizes comments', () => {
    const lexer = new PJSRLexer('// this is a comment\nvar x;');
    const tokens = lexer.tokenize();

    assert.ok(tokens.some(t => t.type === TokenType.COMMENT));
  });

  test('tokenizes multi-line comments', () => {
    const lexer = new PJSRLexer('/* multi\nline */');
    const tokens = lexer.tokenize();

    assert.ok(tokens.some(t => t.type === TokenType.COMMENT && t.value.includes('multi')));
  });
});

describe('PJSRAnalyzer', () => {
  const analyzer = new PJSRAnalyzer();

  test('analyzes includes', () => {
    const code = '#include <pjsr/DataType.jsh>\n#include <pjsr/StdButton.jsh>';
    const result = analyzer.analyze(code);

    assert.strictEqual(result.includes.length, 2);
    assert.strictEqual(result.includes[0].path, 'pjsr/DataType.jsh');
  });

  test('analyzes defines', () => {
    const code = '#define VERSION "1.0.0"';
    const result = analyzer.analyze(code);

    assert.strictEqual(result.defines.length, 1);
    assert.strictEqual(result.defines[0].name, 'VERSION');
  });

  test('analyzes feature declarations', () => {
    const code = '#feature-id MyScript\n#feature-info A test script.';
    const result = analyzer.analyze(code);

    assert.strictEqual(result.features.id, 'MyScript');
    assert.strictEqual(result.features.info, 'A test script.');
  });

  test('identifies PJSR objects', () => {
    const code = 'var window = ImageWindow.activeWindow;\nvar view = new View();';
    const result = analyzer.analyze(code);

    assert.ok(result.usedPJSRObjects.includes('ImageWindow'));
    assert.ok(result.usedPJSRObjects.includes('View'));
  });

  test('detects beginProcess/endProcess mismatch', () => {
    const code = 'view.beginProcess();\nview.beginProcess();';
    const result = analyzer.analyze(code);

    const mismatchDiagnostic = result.diagnostics.find(d =>
      d.message.includes('beginProcess/endProcess')
    );
    assert.ok(mismatchDiagnostic);
    assert.strictEqual(mismatchDiagnostic.severity, 'error');
  });

  test('suggests jsAutoGC', () => {
    const code = 'function main() {}';
    const result = analyzer.analyze(code);

    const gcHint = result.diagnostics.find(d =>
      d.message.includes('jsAutoGC')
    );
    assert.ok(gcHint);
    assert.strictEqual(gcHint.severity, 'hint');
  });

  test('validates code', () => {
    const code = 'view.beginProcess();\nview.endProcess();';
    const result = analyzer.validate(code);

    assert.strictEqual(result.valid, true);
  });

  test('gets hover info for classes', () => {
    const info = analyzer.getHoverInfo('ImageWindow');

    assert.ok(info);
    assert.ok(info.includes('ImageWindow'));
    assert.ok(info.includes('image window'));
  });

  test('gets hover info for constants', () => {
    const info = analyzer.getHoverInfo('ColorSpace_RGB');

    assert.ok(info);
    assert.ok(info.includes('RGB'));
  });

  test('returns null for unknown symbols', () => {
    const info = analyzer.getHoverInfo('UnknownSymbol123');
    assert.strictEqual(info, null);
  });

  test('gets templates', () => {
    const templates = analyzer.getTemplates();

    assert.ok('scriptTemplate' in templates);
    assert.ok('dialogTemplate' in templates);
    assert.ok('imageProcessing' in templates);
  });
});

describe('PJSR Schema', () => {
  test('has core classes', () => {
    assert.ok(pjsrSchema.coreClasses);
    assert.ok('Image' in pjsrSchema.coreClasses);
    assert.ok('ImageWindow' in pjsrSchema.coreClasses);
    assert.ok('View' in pjsrSchema.coreClasses);
  });

  test('has UI controls', () => {
    assert.ok(pjsrSchema.uiControls);
    assert.ok('Dialog' in pjsrSchema.coreClasses);
    assert.ok('Label' in pjsrSchema.uiControls);
    assert.ok('PushButton' in pjsrSchema.uiControls);
  });

  test('has constants', () => {
    assert.ok(pjsrSchema.constants);
    assert.ok('colorSpaces' in pjsrSchema.constants);
    assert.ok('dataTypes' in pjsrSchema.constants);
    assert.ok('imageOperations' in pjsrSchema.constants);
  });

  test('has preprocessor directives', () => {
    assert.ok(pjsrSchema.preprocessorDirectives);
    assert.ok('#include' in pjsrSchema.preprocessorDirectives);
    assert.ok('#define' in pjsrSchema.preprocessorDirectives);
  });

  test('has common patterns', () => {
    assert.ok(pjsrSchema.commonPatterns);
    assert.ok('scriptTemplate' in pjsrSchema.commonPatterns);
  });

  test('Image class has expected structure', () => {
    const image = pjsrSchema.coreClasses.Image;

    assert.ok(image.description);
    assert.ok(image.constructors);
    assert.ok(image.properties);
    assert.ok(image.methods);
    assert.ok(image.properties.width);
    assert.ok(image.methods.sample);
  });

  test('Dialog class has event handlers', () => {
    const dialog = pjsrSchema.coreClasses.Dialog;

    assert.ok(dialog.eventHandlers);
    assert.ok('onExecute' in dialog.eventHandlers);
  });
});

describe('Code Completions', () => {
  const analyzer = new PJSRAnalyzer();

  test('provides completions after new keyword', () => {
    const code = 'var x = new ';
    const completions = analyzer.getCompletions(code, 1, 13);

    assert.ok(completions.length > 0);
    assert.ok(completions.some(c => c.label === 'ImageWindow'));
    assert.ok(completions.some(c => c.label === 'Dialog'));
  });

  test('provides global completions', () => {
    const code = '';
    const completions = analyzer.getCompletions(code, 1, 1);

    assert.ok(completions.some(c => c.kind === 'keyword'));
    assert.ok(completions.some(c => c.label === 'console'));
    assert.ok(completions.some(c => c.label === 'ImageWindow'));
  });
});

// Run tests
console.log('Running PJSR Parser tests...\n');
