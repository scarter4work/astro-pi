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

  test('suggests V8 migration on SM-default scripts and reports no errors', () => {
    const code = 'function main() {}';
    const result = analyzer.analyze(code);

    const migrationHint = result.diagnostics.find(d => d.ruleId === 'sm-consider-migrating-to-v8');
    assert.ok(migrationHint, 'expected sm-consider-migrating-to-v8 hint');
    assert.strictEqual(migrationHint.severity, 'hint');

    const errors = result.diagnostics.filter(d => d.severity === 'error');
    assert.strictEqual(errors.length, 0, `clean SM script should have no errors, got: ${JSON.stringify(errors)}`);
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

describe('V8 Engine Awareness', () => {
  const analyzer = new PJSRAnalyzer();

  test('detects #engine v8 directive', () => {
    const r = analyzer.analyze('#engine v8\nlet x = 1;');
    assert.strictEqual(r.engine, 'v8');
    assert.strictEqual(r.engineDirective, 'v8');
  });

  test('detects #engine v8-new variant', () => {
    const r = analyzer.analyze('#engine v8-new\nlet x = 1;');
    assert.strictEqual(r.engine, 'v8');
    assert.strictEqual(r.engineDirective, 'v8-new');
  });

  test('detects #engine sm explicitly', () => {
    const r = analyzer.analyze('#engine sm\nfunction main(){}');
    assert.strictEqual(r.engine, 'sm');
  });

  test('absence of #engine defaults to sm', () => {
    const r = analyzer.analyze('function main(){}');
    assert.strictEqual(r.engine, null);
    assert.strictEqual(analyzer.resolveEngine(r), 'sm');
  });

  test('flags __base__ inheritance as error under v8', () => {
    const code = '#engine v8\nfunction Foo() { this.__base__ = Dialog; }';
    const r = analyzer.analyze(code);
    const d = r.diagnostics.find(x => x.ruleId === 'v8-no-base-inheritance');
    assert.ok(d, 'expected v8-no-base-inheritance diagnostic');
    assert.strictEqual(d.severity, 'error');
  });

  test('does not flag __base__ under sm', () => {
    const code = 'function Foo() { this.__base__ = Dialog; }';
    const r = analyzer.analyze(code);
    const d = r.diagnostics.find(x => x.ruleId === 'v8-no-base-inheritance');
    assert.strictEqual(d, undefined);
  });

  test('flags gc() as deprecated warning under v8', () => {
    const code = '#engine v8\nCoreApplication.ensureMinimumVersion(1,9,4);\ngc();';
    const r = analyzer.analyze(code);
    const d = r.diagnostics.find(x => x.ruleId === 'v8-deprecated-gc');
    assert.ok(d, 'expected v8-deprecated-gc diagnostic');
    assert.strictEqual(d.severity, 'warning');
  });

  test('flags VectorGraphics as removed under v8', () => {
    const code = '#engine v8\nCoreApplication.ensureMinimumVersion(1,9,4);\nlet g = new VectorGraphics(bitmap);';
    const r = analyzer.analyze(code);
    const d = r.diagnostics.find(x =>
      x.ruleId === 'v8-removed-VectorGraphics' || (x.excerpt === 'VectorGraphics' && x.severity === 'error'));
    assert.ok(d, 'expected VectorGraphics removed diagnostic');
    assert.strictEqual(d.severity, 'error');
  });

  test('flags ImageStatistics as removed under v8 via schema lifecycle', () => {
    const code = '#engine v8\nCoreApplication.ensureMinimumVersion(1,9,4);\nlet s = new ImageStatistics(image);';
    const r = analyzer.analyze(code);
    const d = r.diagnostics.find(x => x.excerpt === 'ImageStatistics' && x.severity === 'error');
    assert.ok(d, 'expected ImageStatistics removed diagnostic');
  });

  test('flags flat StdIcon_* constants under v8', () => {
    const code = '#engine v8\nCoreApplication.ensureMinimumVersion(1,9,4);\nlet x = StdIcon_Warning;';
    const r = analyzer.analyze(code);
    const d = r.diagnostics.find(x => x.ruleId === 'v8-deprecated-flat-constants');
    assert.ok(d, 'expected v8-deprecated-flat-constants diagnostic');
    assert.strictEqual(d.severity, 'warning');
  });

  test('flags <pjsr/...> includes under v8', () => {
    const code = '#engine v8\nCoreApplication.ensureMinimumVersion(1,9,4);\n#include <pjsr/StdIcon.jsh>';
    const r = analyzer.analyze(code);
    const d = r.diagnostics.find(x => x.ruleId === 'v8-deprecated-pjsr-include');
    assert.ok(d, 'expected v8-deprecated-pjsr-include diagnostic');
  });

  test('recommends ensureMinimumVersion when missing under v8', () => {
    const code = '#engine v8\nlet x = 1;';
    const r = analyzer.analyze(code);
    const d = r.diagnostics.find(x => x.ruleId === 'v8-recommend-ensureMinimumVersion');
    assert.ok(d, 'expected v8-recommend-ensureMinimumVersion hint');
    assert.strictEqual(d.severity, 'hint');
  });

  test('function-call rule ignores method calls (e.g. obj.gc() is not flagged)', () => {
    // gc() as a method call on an object is not the deprecated global gc().
    const code = '#engine v8\nCoreApplication.ensureMinimumVersion(1,9,4);\nlet obj = {gc: () => {}};\nobj.gc();';
    const r = analyzer.analyze(code);
    const d = r.diagnostics.find(x => x.ruleId === 'v8-deprecated-gc');
    assert.strictEqual(d, undefined, 'method-style .gc() should not trigger v8-deprecated-gc');
  });

  test('invalid schema regex surfaces as a diagnostic, not a crash', () => {
    // Drive the safe-regex path: temporarily inject a bad rule via a private cache,
    // then run analyze on minimal input.
    const localAnalyzer = new PJSRAnalyzer();
    const original = localAnalyzer.schema.lintRules;
    localAnalyzer.schema.lintRules = [
      { id: 'bad-rule', appliesToEngines: ['v8'], severity: 'error',
        match: { type: 'code-pattern', regex: '(unclosed' }, message: 'should never see this' }
    ];
    try {
      const r = localAnalyzer.analyze('#engine v8\nlet x = 1;');
      const d = r.diagnostics.find(x => x.ruleId === 'schema-invalid-regex');
      assert.ok(d, 'expected schema-invalid-regex diagnostic from malformed pattern');
      assert.strictEqual(d.severity, 'error');
    } finally {
      localAnalyzer.schema.lintRules = original;
    }
  });

  test('method-level removedIn metadata surfaces via lifecycle pass', () => {
    // Image.forEachSample has removedIn:"v8" at method level. The
    // v8-removed-image-foreach lint rule covers the method-access pattern;
    // additionally, an identifier-only reference to forEachSample should
    // surface via the lifecycle table since it recurses into methods.
    const code = '#engine v8\nCoreApplication.ensureMinimumVersion(1,9,4);\nlet fn = forEachSample;';
    const r = analyzer.analyze(code);
    const d = r.diagnostics.find(x =>
      (x.ruleId === 'v8-removed-symbol' || x.ruleId === 'v8-removed-image-foreach') &&
      x.excerpt && x.excerpt.includes('forEachSample'));
    assert.ok(d, 'expected method-level removedIn metadata to surface');
  });

  test('clean V8 script produces no errors', () => {
    const code = `#engine v8

#feature-id MyScript

CoreApplication.ensureMinimumVersion(1, 9, 4);

(() => {
  let image = ImageWindow.activeWindow.mainView.image;
  let D = new StarDetector;
  let stars = D.stars(image);
  console.writeln(stars.length);
})();
`;
    const r = analyzer.analyze(code);
    const errors = r.diagnostics.filter(d => d.severity === 'error');
    assert.strictEqual(errors.length, 0, `expected no errors, got: ${JSON.stringify(errors)}`);
  });
});

describe('V8 Schema Additions', () => {
  test('engines section present', () => {
    assert.ok(pjsrSchema.engines, 'expected engines section');
    assert.ok(pjsrSchema.engines.v8);
    assert.ok(pjsrSchema.engines.sm);
    assert.strictEqual(pjsrSchema.engines.v8.since, '1.9.4');
  });

  test('lintRules present and well-formed', () => {
    assert.ok(Array.isArray(pjsrSchema.lintRules));
    assert.ok(pjsrSchema.lintRules.length > 5);
    for (const r of pjsrSchema.lintRules) {
      assert.ok(r.id, `rule missing id: ${JSON.stringify(r)}`);
      assert.ok(Array.isArray(r.appliesToEngines), `rule ${r.id} missing appliesToEngines`);
      assert.ok(r.severity);
      assert.ok(r.match && r.match.type);
      assert.ok(r.message);
    }
  });

  test('V8 core classes added', () => {
    assert.ok(pjsrSchema.coreClasses.FMath);
    assert.ok(pjsrSchema.coreClasses.Stat);
    assert.ok(pjsrSchema.coreClasses.ImageIterator);
    assert.ok(pjsrSchema.coreClasses.PSF);
    assert.ok(pjsrSchema.coreClasses.BRQuadTree);
    assert.ok(pjsrSchema.coreClasses.XMLDocument);
    assert.ok(pjsrSchema.coreClasses.System);
    assert.ok(pjsrSchema.coreClasses.CoreApplication);
  });

  test('removed-in-v8 classes documented', () => {
    assert.strictEqual(pjsrSchema.coreClasses.VectorGraphics.removedIn, 'v8');
    assert.strictEqual(pjsrSchema.coreClasses.ImageStatistics.removedIn, 'v8');
  });

  test('V8 templates added', () => {
    assert.ok(pjsrSchema.commonPatterns.scriptTemplateV8);
    assert.ok(pjsrSchema.commonPatterns.dialogTemplateV8);
    assert.ok(pjsrSchema.commonPatterns.imageProcessingV8);
    assert.ok(pjsrSchema.commonPatterns.starDetectionV8);
    assert.ok(pjsrSchema.commonPatterns.scriptTemplateV8.template.includes('#engine v8'));
  });

  test('#engine preprocessor directive documented', () => {
    assert.ok(pjsrSchema.preprocessorDirectives['#engine']);
  });
});

describe('Target Version Detection (1.9.4 awareness)', () => {
  const analyzer = new PJSRAnalyzer();

  test('detects target version from CoreApplication.ensureMinimumVersion', () => {
    const code = '#engine v8\nCoreApplication.ensureMinimumVersion(1, 9, 4);\nlet x = 1;';
    const r = analyzer.analyze(code);
    assert.strictEqual(r.targetVersion, '1.9.4');
  });

  test('detects target version from #ifge __PI_VERSION__ fallback', () => {
    const code = '#engine v8\n#ifge __PI_VERSION__ 01.09.04\nlet x = 1;\n#endif';
    const r = analyzer.analyze(code);
    assert.strictEqual(r.targetVersion, '1.9.4');
  });

  test('targetVersion is null when no declaration found', () => {
    const code = '#engine v8\nlet x = 1;';
    const r = analyzer.analyze(code);
    assert.strictEqual(r.targetVersion, null);
  });

  test('compareVersions orders releases correctly', () => {
    assert.strictEqual(PJSRAnalyzer.compareVersions('1.9.4', '1.9.4'), 0);
    assert.strictEqual(PJSRAnalyzer.compareVersions('1.9.3', '1.9.4'), -1);
    assert.strictEqual(PJSRAnalyzer.compareVersions('1.9.4', '1.9.3'), 1);
    assert.strictEqual(PJSRAnalyzer.compareVersions('1.9.4.1', '1.9.4'), 1);
  });

  test('>=1.9.4 rule fires when target is 1.9.4', () => {
    const code = '#engine sm\nCoreApplication.ensureMinimumVersion(1, 9, 4);\nlet x = 1;';
    const r = analyzer.analyze(code);
    const d = r.diagnostics.find(x => x.ruleId === 'v1.9.4-sm-engine-removed');
    assert.ok(d, 'expected v1.9.4-sm-engine-removed error');
    assert.strictEqual(d.severity, 'error');
  });

  test('>=1.9.4 rule suppressed when target unknown', () => {
    const code = '#engine sm\nlet x = 1;';
    const r = analyzer.analyze(code);
    const d = r.diagnostics.find(x => x.ruleId === 'v1.9.4-sm-engine-removed');
    assert.strictEqual(d, undefined, 'rule should not fire without a declared target version');
  });

  test('no-engine-directive escalates to error when target is 1.9.4', () => {
    // Engine resolver defaults to sm when no #engine; the version-conditional
    // rule kicks in because ensureMinimumVersion declares 1.9.4.
    const code = 'CoreApplication.ensureMinimumVersion(1, 9, 4);\nfunction main(){}';
    const r = analyzer.analyze(code);
    const errRule = r.diagnostics.find(x => x.ruleId === 'v1.9.4-no-engine-directive-error');
    assert.ok(errRule, 'expected hard error for missing #engine under 1.9.4 target');
    assert.strictEqual(errRule.severity, 'error');
  });

  test('migration hint suppressed when target is 1.9.4 (avoids dup with hard error)', () => {
    const code = 'CoreApplication.ensureMinimumVersion(1, 9, 4);\nfunction main(){}';
    const r = analyzer.analyze(code);
    const hint = r.diagnostics.find(x => x.ruleId === 'sm-consider-migrating-to-v8');
    assert.strictEqual(hint, undefined, 'lenient hint should not fire when target is known >=1.9.4');
  });

  test('migration hint still fires when target is unknown', () => {
    const code = 'function main(){}';
    const r = analyzer.analyze(code);
    const hint = r.diagnostics.find(x => x.ruleId === 'sm-consider-migrating-to-v8');
    assert.ok(hint, 'expected migration hint on legacy/unknown-target script');
  });

  test('ABE instantiation under 1.9.4 emits hint', () => {
    const code = '#engine v8\nCoreApplication.ensureMinimumVersion(1, 9, 4);\nlet P = new ProcessInstance("AutomaticBackgroundExtractor");';
    const r = analyzer.analyze(code);
    const d = r.diagnostics.find(x => x.ruleId === 'v1.9.4-abe-property-changes');
    assert.ok(d, 'expected ABE property-change hint');
    assert.strictEqual(d.severity, 'hint');
  });

  test('SubframeSelector instantiation under 1.9.4 emits hint', () => {
    const code = '#engine v8\nCoreApplication.ensureMinimumVersion(1, 9, 4);\nlet P = new ProcessInstance("SubframeSelector");';
    const r = analyzer.analyze(code);
    const d = r.diagnostics.find(x => x.ruleId === 'v1.9.4-sfs-property-changes');
    assert.ok(d, 'expected SubframeSelector property-change hint');
  });

  test('ABE hint suppressed when no target version declared', () => {
    const code = '#engine v8\nlet P = new ProcessInstance("AutomaticBackgroundExtractor");';
    const r = analyzer.analyze(code);
    const d = r.diagnostics.find(x => x.ruleId === 'v1.9.4-abe-property-changes');
    assert.strictEqual(d, undefined);
  });
});

describe('New V8 Lint Rules', () => {
  const analyzer = new PJSRAnalyzer();

  test('flags import/export as not supported under v8', () => {
    const code = '#engine v8\nimport { foo } from "bar";';
    const r = analyzer.analyze(code);
    const d = r.diagnostics.find(x => x.ruleId === 'v8-no-js-modules');
    assert.ok(d, 'expected v8-no-js-modules error');
    assert.strictEqual(d.severity, 'error');
  });

  test('flags ImageWindow.purge(true) as error under v8', () => {
    const code = '#engine v8\nCoreApplication.ensureMinimumVersion(1,9,4);\nwindow.purge(true);';
    const r = analyzer.analyze(code);
    const d = r.diagnostics.find(x => x.ruleId === 'v8-imagewindow-purge-no-bool-args');
    assert.ok(d, 'expected v8-imagewindow-purge-no-bool-args error');
  });

  test('flags Compression.compress()[0][0] shape access', () => {
    const code = '#engine v8\nCoreApplication.ensureMinimumVersion(1,9,4);\nlet b = result.compress(data)[0][0];';
    const r = analyzer.analyze(code);
    const d = r.diagnostics.find(x => x.ruleId === 'v8-compression-array-shape');
    assert.ok(d, 'expected compression shape diagnostic');
  });

  test('flags .viewById(id).isNull check', () => {
    const code = '#engine v8\nCoreApplication.ensureMinimumVersion(1,9,4);\nif (view.viewById("foo").isNull) {}';
    const r = analyzer.analyze(code);
    const d = r.diagnostics.find(x => x.ruleId === 'v8-null-vs-isnull-view');
    assert.ok(d, 'expected v8-null-vs-isnull-view diagnostic');
  });

  test('flags v8-default as discouraged', () => {
    const code = '#engine v8-default\nlet x = 1;';
    const r = analyzer.analyze(code);
    const d = r.diagnostics.find(x => x.ruleId === 'v8-discourage-v8-default');
    assert.ok(d, 'expected v8-discourage-v8-default info');
  });

  test('clean explicit V8 script with target produces no errors', () => {
    const code = `#engine v8

CoreApplication.ensureMinimumVersion(1, 9, 4);

(() => {
  let image = ImageWindow.activeWindow.mainView.image;
  console.writeln(image.width);
})();
`;
    const r = analyzer.analyze(code);
    const errors = r.diagnostics.filter(d => d.severity === 'error');
    assert.strictEqual(errors.length, 0, `expected no errors, got: ${JSON.stringify(errors)}`);
  });
});

describe('1.9.4 Schema Additions', () => {
  test('versions block present with 1.9.4 metadata', () => {
    assert.ok(pjsrSchema.versions, 'expected versions block');
    assert.ok(pjsrSchema.versions['1.9.4'], 'expected 1.9.4 entry');
    assert.strictEqual(pjsrSchema.versions['1.9.4'].smEngineRemoved, true);
  });

  test('sm engine marked removedIn 1.9.4', () => {
    assert.strictEqual(pjsrSchema.engines.sm.removedIn, '1.9.4');
    assert.strictEqual(pjsrSchema.engines.sm.default, false);
  });

  test('new preprocessor directives present', () => {
    const d = pjsrSchema.preprocessorDirectives;
    assert.ok('#ifeq' in d);
    assert.ok('#ifneq' in d);
    assert.ok('#ifoneof' in d);
    assert.ok('#ifnoneof' in d);
    assert.ok('#undef' in d);
    assert.ok('#error' in d);
    assert.ok('#warning' in d);
  });

  test('Bitmap.clear (v8 explicit-free helper) documented', () => {
    const bm = pjsrSchema.coreClasses.Bitmap;
    assert.ok(bm.methods.clear, 'expected Bitmap.clear method');
    assert.deepStrictEqual(bm.methods.clear.availableIn, ['v8']);
  });

  test('new 1.9.4-conditional lint rules carry requiresVersionRange', () => {
    const rules = pjsrSchema.lintRules;
    const conditional = rules.filter(r => r.requiresVersionRange);
    assert.ok(conditional.length >= 4, `expected several version-conditional rules, got ${conditional.length}`);
    // 1.9.4 hard error
    const smRemoved = rules.find(r => r.id === 'v1.9.4-sm-engine-removed');
    assert.ok(smRemoved);
    assert.strictEqual(smRemoved.requiresVersionRange, '>=1.9.4');
    assert.strictEqual(smRemoved.severity, 'error');
  });
});

// Run tests
console.log('Running PJSR Parser tests...\n');
