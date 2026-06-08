/**
 * PCL Parser Tests
 */

import { test, describe } from 'node:test';
import assert from 'node:assert';
import { PCLLexer, PCLAnalyzer, TokenType } from './pcl-parser.js';

describe('PCLLexer', () => {
  test('tokenizes simple variable declaration', () => {
    const lexer = new PCLLexer('int x = 42;');
    const tokens = lexer.tokenize();

    assert.strictEqual(tokens[0].type, 'keyword');
    assert.strictEqual(tokens[0].value, 'int');
    assert.strictEqual(tokens[1].type, 'identifier');
    assert.strictEqual(tokens[1].value, 'x');
    assert.strictEqual(tokens[2].type, 'operator');
    assert.strictEqual(tokens[2].value, '=');
    assert.strictEqual(tokens[3].type, 'number');
    assert.strictEqual(tokens[3].value, '42');
  });

  test('tokenizes preprocessor directives', () => {
    const lexer = new PCLLexer('#include <pcl/Image.h>');
    const tokens = lexer.tokenize();

    assert.strictEqual(tokens[0].type, 'preprocessor');
    assert.ok(tokens[0].value.includes('#include'));
    assert.ok(tokens[0].value.includes('pcl/Image.h'));
  });

  test('tokenizes namespace usage', () => {
    const lexer = new PCLLexer('pcl::Image img;');
    const tokens = lexer.tokenize();

    assert.strictEqual(tokens[0].type, 'identifier');
    assert.strictEqual(tokens[0].value, 'pcl');
    assert.strictEqual(tokens[1].type, 'operator');
    assert.strictEqual(tokens[1].value, '::');
    assert.strictEqual(tokens[2].type, 'identifier');
    assert.strictEqual(tokens[2].value, 'Image');
  });

  test('tokenizes string literals', () => {
    const lexer = new PCLLexer('const char* s = "Hello, PCL!";');
    const tokens = lexer.tokenize();

    const stringToken = tokens.find(t => t.type === 'string');
    assert.ok(stringToken);
    assert.strictEqual(stringToken.value, '"Hello, PCL!"');
  });

  test('tokenizes comments', () => {
    const lexer = new PCLLexer('// Single line comment\nint x;');
    const tokens = lexer.tokenize();

    assert.strictEqual(tokens[0].type, 'comment');
    assert.ok(tokens[0].value.includes('Single line'));
  });

  test('tokenizes multi-line comments', () => {
    const lexer = new PCLLexer('/* Multi\nline\ncomment */ int x;');
    const tokens = lexer.tokenize();

    assert.strictEqual(tokens[0].type, 'comment');
    assert.ok(tokens[0].value.includes('Multi'));
    assert.ok(tokens[0].value.includes('line'));
  });

  test('tokenizes hex numbers', () => {
    const lexer = new PCLLexer('int color = 0xFF00FF;');
    const tokens = lexer.tokenize();

    const numToken = tokens.find(t => t.type === 'number');
    assert.ok(numToken);
    assert.strictEqual(numToken.value, '0xFF00FF');
  });

  test('tokenizes class declaration', () => {
    const lexer = new PCLLexer('class MyProcess : public pcl::MetaProcess { };');
    const tokens = lexer.tokenize();

    assert.strictEqual(tokens[0].type, 'keyword');
    assert.strictEqual(tokens[0].value, 'class');
    assert.strictEqual(tokens[1].type, 'identifier');
    assert.strictEqual(tokens[1].value, 'MyProcess');
  });

  test('tokenizes template syntax', () => {
    const lexer = new PCLLexer('Array<int> arr;');
    const tokens = lexer.tokenize();

    assert.strictEqual(tokens[0].type, 'identifier');
    assert.strictEqual(tokens[0].value, 'Array');
    assert.strictEqual(tokens[1].type, 'operator');
    assert.strictEqual(tokens[1].value, '<');
  });

  test('tokenizes arrow operator', () => {
    const lexer = new PCLLexer('ptr->method();');
    const tokens = lexer.tokenize();

    const arrowToken = tokens.find(t => t.value === '->');
    assert.ok(arrowToken);
    assert.strictEqual(arrowToken.type, 'operator');
  });
});

describe('PCLAnalyzer', () => {
  const analyzer = new PCLAnalyzer();

  test('analyzes includes', () => {
    const code = `
#include <pcl/Image.h>
#include <pcl/Console.h>
#include <vector>
    `;
    const result = analyzer.analyze(code);

    assert.strictEqual(result.includes.length, 3);
    assert.ok(result.includes.some(i => i.file === 'pcl/Image.h' && i.isPCL));
    assert.ok(result.includes.some(i => i.file === 'pcl/Console.h' && i.isPCL));
    assert.ok(result.includes.some(i => i.file === 'vector' && !i.isPCL));
  });

  test('analyzes defines', () => {
    const code = `
#define MODULE_VERSION_MAJOR 1
#define MODULE_VERSION_MINOR 0
    `;
    const result = analyzer.analyze(code);

    assert.strictEqual(result.defines.length, 2);
    assert.ok(result.defines.some(d => d.name === 'MODULE_VERSION_MAJOR'));
  });

  test('detects PCL class usage', () => {
    const code = `
#include <pcl/Image.h>
using namespace pcl;
Image img;
Console console;
    `;
    const result = analyzer.analyze(code);

    assert.ok(result.pclObjects.length > 0);
  });

  test('analyzes class declarations', () => {
    const code = `
class MyModule : public pcl::MetaModule {
public:
    const char* Version() const override;
};
    `;
    const result = analyzer.analyze(code);

    assert.ok(result.classes.length > 0);
    assert.strictEqual(result.classes[0].name, 'MyModule');
    assert.ok(result.classes[0].baseClasses.includes('MetaModule'));
  });

  test('validates code', () => {
    const code = `
Image img;
Console console;
    `;
    const result = analyzer.validate(code);

    assert.ok('valid' in result);
    assert.ok('diagnostics' in result);
    assert.ok('summary' in result);
  });

  test('provides class info', () => {
    const info = analyzer.getClassInfo('Image');

    assert.ok(info);
    assert.ok(info.description);
    assert.ok(info.methods);
  });

  test('lists classes', () => {
    const classes = analyzer.listClasses();

    assert.ok(classes.length > 0);
    assert.ok(classes.some(c => c.name === 'Image'));
    assert.ok(classes.some(c => c.name === 'Console'));
  });

  test('provides templates', () => {
    const templates = analyzer.getTemplates();

    assert.ok(templates.moduleDefinition);
    assert.ok(templates.imageProcessing);
    assert.ok(templates.dialogTemplate);
  });

  test('provides best practices', () => {
    const practices = analyzer.getBestPractices('general');

    assert.ok(practices.includes('PCL'));
    assert.ok(practices.length > 100);
  });

  test('lists PCL headers', () => {
    const headers = analyzer.getPCLHeaders();

    assert.ok(headers.length > 0);
    assert.ok(headers.some(h => h.includes('Image.h')));
    assert.ok(headers.some(h => h.includes('Console.h')));
  });

  test('gets hover info for class', () => {
    const info = analyzer.getHoverInfo('Image');

    assert.ok(info);
    assert.ok(info.includes('Image'));
    assert.ok(info.includes('#'));  // Markdown header
  });

  test('gets completions after pcl::', () => {
    const code = 'pcl::';
    const completions = analyzer.getCompletions(code, 1, 6);

    assert.ok(completions.length > 0);
  });
});

describe('Integration', () => {
  test('full module code analysis', () => {
    const code = `
#define MODULE_VERSION_MAJOR     1
#define MODULE_VERSION_MINOR     0
#define MODULE_VERSION_REVISION  0

#include <pcl/MetaModule.h>
#include <pcl/Console.h>

namespace MyModule
{

class MyModule : public pcl::MetaModule
{
public:
    MyModule();
    const char* Version() const override;
    IsoString Name() const override;
};

void ProcessImage(pcl::Image& image)
{
    pcl::Console console;
    console.WriteLn("Processing...");
}

} // namespace MyModule
    `;

    const analyzer = new PCLAnalyzer();
    const result = analyzer.analyze(code);

    // Check includes
    assert.strictEqual(result.includes.length, 2);
    assert.ok(result.includes.every(i => i.isPCL));

    // Check defines
    assert.strictEqual(result.defines.length, 3);

    // Check namespace
    assert.ok(result.namespaces.some(n => n.name === 'MyModule'));

    // Check class
    assert.ok(result.classes.some(c => c.name === 'MyModule'));

    // Check PCL usage
    assert.ok(result.pclObjects.length > 0);
  });
});

console.log('Running PCL Parser tests...');
