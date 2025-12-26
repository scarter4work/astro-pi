/**
 * PJSR Parser - Parses and analyzes PixInsight JavaScript Runtime code
 */

import { readFileSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

// Load the PJSR schema
const schemaPath = join(__dirname, '..', 'schemas', 'pjsr-core.json');
let pjsrSchema;
try {
  pjsrSchema = JSON.parse(readFileSync(schemaPath, 'utf8'));
} catch (e) {
  console.error('Failed to load PJSR schema:', e.message);
  pjsrSchema = {};
}

/**
 * Token types for PJSR lexer
 */
const TokenType = {
  KEYWORD: 'KEYWORD',
  IDENTIFIER: 'IDENTIFIER',
  NUMBER: 'NUMBER',
  STRING: 'STRING',
  OPERATOR: 'OPERATOR',
  PUNCTUATION: 'PUNCTUATION',
  COMMENT: 'COMMENT',
  PREPROCESSOR: 'PREPROCESSOR',
  WHITESPACE: 'WHITESPACE',
  EOF: 'EOF'
};

/**
 * PJSR Keywords (JavaScript ES5 + PJSR-specific)
 */
const KEYWORDS = new Set([
  // JavaScript keywords
  'break', 'case', 'catch', 'continue', 'debugger', 'default', 'delete',
  'do', 'else', 'finally', 'for', 'function', 'if', 'in', 'instanceof',
  'new', 'return', 'switch', 'this', 'throw', 'try', 'typeof', 'var',
  'void', 'while', 'with', 'let', 'const', 'class', 'extends', 'super',
  // Boolean/Null literals
  'true', 'false', 'null', 'undefined',
  // Future reserved
  'enum', 'export', 'import', 'implements', 'interface', 'package',
  'private', 'protected', 'public', 'static', 'yield'
]);

/**
 * PJSR Preprocessor directives
 */
const PREPROCESSOR_DIRECTIVES = new Set([
  '#include', '#define', '#ifdef', '#ifndef', '#if', '#iflt', '#ifle',
  '#ifgt', '#ifge', '#else', '#endif', '#feature-id', '#feature-info',
  '#error', '#warning'
]);

/**
 * Lexer for PJSR code
 */
class PJSRLexer {
  constructor(source) {
    this.source = source;
    this.pos = 0;
    this.line = 1;
    this.column = 1;
    this.tokens = [];
  }

  peek(offset = 0) {
    return this.source[this.pos + offset] || '';
  }

  advance() {
    const ch = this.source[this.pos++];
    if (ch === '\n') {
      this.line++;
      this.column = 1;
    } else {
      this.column++;
    }
    return ch;
  }

  isEOF() {
    return this.pos >= this.source.length;
  }

  isDigit(ch) {
    return /[0-9]/.test(ch);
  }

  isAlpha(ch) {
    return /[a-zA-Z_$]/.test(ch);
  }

  isAlphaNumeric(ch) {
    return /[a-zA-Z0-9_$]/.test(ch);
  }

  isWhitespace(ch) {
    return /[\s\t\r\n]/.test(ch);
  }

  tokenize() {
    while (!this.isEOF()) {
      const startLine = this.line;
      const startColumn = this.column;
      const startPos = this.pos;

      const ch = this.peek();

      // Whitespace
      if (this.isWhitespace(ch)) {
        this.skipWhitespace();
        continue;
      }

      // Preprocessor directives
      if (ch === '#' && (startColumn === 1 || this.source[startPos - 1] === '\n')) {
        this.tokens.push(this.readPreprocessor(startLine, startColumn));
        continue;
      }

      // Comments
      if (ch === '/' && (this.peek(1) === '/' || this.peek(1) === '*')) {
        this.tokens.push(this.readComment(startLine, startColumn));
        continue;
      }

      // Strings
      if (ch === '"' || ch === "'") {
        this.tokens.push(this.readString(startLine, startColumn));
        continue;
      }

      // Numbers
      if (this.isDigit(ch) || (ch === '.' && this.isDigit(this.peek(1)))) {
        this.tokens.push(this.readNumber(startLine, startColumn));
        continue;
      }

      // Identifiers and keywords
      if (this.isAlpha(ch)) {
        this.tokens.push(this.readIdentifier(startLine, startColumn));
        continue;
      }

      // Operators and punctuation
      this.tokens.push(this.readOperator(startLine, startColumn));
    }

    this.tokens.push({
      type: TokenType.EOF,
      value: '',
      line: this.line,
      column: this.column
    });

    return this.tokens;
  }

  skipWhitespace() {
    while (!this.isEOF() && this.isWhitespace(this.peek())) {
      this.advance();
    }
  }

  readPreprocessor(startLine, startColumn) {
    let value = '';
    while (!this.isEOF() && this.peek() !== '\n') {
      value += this.advance();
    }
    return {
      type: TokenType.PREPROCESSOR,
      value: value.trim(),
      line: startLine,
      column: startColumn
    };
  }

  readComment(startLine, startColumn) {
    let value = '';
    if (this.peek(1) === '/') {
      // Single-line comment
      while (!this.isEOF() && this.peek() !== '\n') {
        value += this.advance();
      }
    } else {
      // Multi-line comment
      value += this.advance(); // /
      value += this.advance(); // *
      while (!this.isEOF()) {
        if (this.peek() === '*' && this.peek(1) === '/') {
          value += this.advance(); // *
          value += this.advance(); // /
          break;
        }
        value += this.advance();
      }
    }
    return {
      type: TokenType.COMMENT,
      value,
      line: startLine,
      column: startColumn
    };
  }

  readString(startLine, startColumn) {
    const quote = this.advance();
    let value = quote;
    while (!this.isEOF()) {
      const ch = this.advance();
      value += ch;
      if (ch === '\\' && !this.isEOF()) {
        value += this.advance();
      } else if (ch === quote) {
        break;
      }
    }
    return {
      type: TokenType.STRING,
      value,
      line: startLine,
      column: startColumn
    };
  }

  readNumber(startLine, startColumn) {
    let value = '';
    // Hex
    if (this.peek() === '0' && (this.peek(1) === 'x' || this.peek(1) === 'X')) {
      value += this.advance();
      value += this.advance();
      while (!this.isEOF() && /[0-9a-fA-F]/.test(this.peek())) {
        value += this.advance();
      }
    } else {
      // Decimal/float
      while (!this.isEOF() && (this.isDigit(this.peek()) || this.peek() === '.')) {
        value += this.advance();
      }
      // Exponent
      if (this.peek() === 'e' || this.peek() === 'E') {
        value += this.advance();
        if (this.peek() === '+' || this.peek() === '-') {
          value += this.advance();
        }
        while (!this.isEOF() && this.isDigit(this.peek())) {
          value += this.advance();
        }
      }
    }
    return {
      type: TokenType.NUMBER,
      value,
      line: startLine,
      column: startColumn
    };
  }

  readIdentifier(startLine, startColumn) {
    let value = '';
    while (!this.isEOF() && this.isAlphaNumeric(this.peek())) {
      value += this.advance();
    }
    return {
      type: KEYWORDS.has(value) ? TokenType.KEYWORD : TokenType.IDENTIFIER,
      value,
      line: startLine,
      column: startColumn
    };
  }

  readOperator(startLine, startColumn) {
    const ch = this.advance();
    const multiCharOps = ['===', '!==', '>>>', '&&=', '||=', '??=',
      '==', '!=', '<=', '>=', '&&', '||', '??', '++', '--',
      '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<', '>>', '=>'];

    for (const op of multiCharOps) {
      if (ch + this.source.slice(this.pos, this.pos + op.length - 1) === op) {
        for (let i = 1; i < op.length; i++) this.advance();
        return {
          type: TokenType.OPERATOR,
          value: op,
          line: startLine,
          column: startColumn
        };
      }
    }

    const punctuation = '{}[]();:,.?';
    return {
      type: punctuation.includes(ch) ? TokenType.PUNCTUATION : TokenType.OPERATOR,
      value: ch,
      line: startLine,
      column: startColumn
    };
  }
}

/**
 * PJSR Code Analyzer
 */
class PJSRAnalyzer {
  constructor() {
    this.schema = pjsrSchema;
  }

  /**
   * Analyze PJSR code and return information
   */
  analyze(code) {
    const lexer = new PJSRLexer(code);
    const tokens = lexer.tokenize();

    const analysis = {
      tokens: tokens,
      includes: [],
      defines: [],
      features: {},
      classes: [],
      functions: [],
      variables: [],
      processInstances: [],
      diagnostics: [],
      usedPJSRObjects: new Set()
    };

    // Analyze tokens
    for (let i = 0; i < tokens.length; i++) {
      const token = tokens[i];

      // Preprocessor directives
      if (token.type === TokenType.PREPROCESSOR) {
        this.analyzePreprocessor(token, analysis);
      }

      // Identify PJSR objects being used
      if (token.type === TokenType.IDENTIFIER) {
        if (this.isPJSRClass(token.value)) {
          analysis.usedPJSRObjects.add(token.value);
        }
      }

      // Function definitions
      if (token.type === TokenType.KEYWORD && token.value === 'function') {
        const funcInfo = this.extractFunction(tokens, i);
        if (funcInfo) {
          analysis.functions.push(funcInfo);
        }
      }

      // Variable declarations
      if (token.type === TokenType.KEYWORD && (token.value === 'var' || token.value === 'let' || token.value === 'const')) {
        const varInfo = this.extractVariable(tokens, i);
        if (varInfo) {
          analysis.variables.push(varInfo);
        }
      }

      // ProcessInstance usage
      if (token.type === TokenType.IDENTIFIER && token.value === 'ProcessInstance') {
        const processInfo = this.extractProcessInstance(tokens, i);
        if (processInfo) {
          analysis.processInstances.push(processInfo);
        }
      }
    }

    // Run diagnostics
    this.runDiagnostics(code, tokens, analysis);

    // Convert Set to Array for serialization
    analysis.usedPJSRObjects = Array.from(analysis.usedPJSRObjects);

    return analysis;
  }

  analyzePreprocessor(token, analysis) {
    const value = token.value;

    if (value.startsWith('#include')) {
      const match = value.match(/#include\s+[<"]([^>"]+)[>"]/);
      if (match) {
        analysis.includes.push({
          path: match[1],
          line: token.line
        });
      }
    } else if (value.startsWith('#define')) {
      const match = value.match(/#define\s+(\w+)(?:\s+(.*))?/);
      if (match) {
        analysis.defines.push({
          name: match[1],
          value: match[2] || '',
          line: token.line
        });
      }
    } else if (value.startsWith('#feature-id')) {
      const match = value.match(/#feature-id\s+(.+)/);
      if (match) {
        analysis.features.id = match[1].trim();
      }
    } else if (value.startsWith('#feature-info')) {
      const match = value.match(/#feature-info\s+(.+)/);
      if (match) {
        analysis.features.info = match[1].trim();
      }
    }
  }

  isPJSRClass(name) {
    return this.schema.coreClasses && name in this.schema.coreClasses ||
           this.schema.uiControls && name in this.schema.uiControls ||
           this.schema.globalObjects && name in this.schema.globalObjects;
  }

  extractFunction(tokens, index) {
    // Look for function name
    let i = index + 1;
    while (i < tokens.length && tokens[i].type === TokenType.WHITESPACE) i++;

    if (i < tokens.length && tokens[i].type === TokenType.IDENTIFIER) {
      return {
        name: tokens[i].value,
        line: tokens[index].line,
        column: tokens[index].column
      };
    }
    return null;
  }

  extractVariable(tokens, index) {
    let i = index + 1;
    while (i < tokens.length && tokens[i].type === TokenType.WHITESPACE) i++;

    if (i < tokens.length && tokens[i].type === TokenType.IDENTIFIER) {
      const varName = tokens[i].value;
      let assignedType = null;

      // Look for 'new ClassName' pattern
      while (i < tokens.length && tokens[i].value !== ';' && tokens[i].value !== '\n') {
        if (tokens[i].type === TokenType.KEYWORD && tokens[i].value === 'new') {
          i++;
          while (i < tokens.length && tokens[i].type === TokenType.WHITESPACE) i++;
          if (i < tokens.length && tokens[i].type === TokenType.IDENTIFIER) {
            assignedType = tokens[i].value;
          }
          break;
        }
        i++;
      }

      return {
        name: varName,
        type: assignedType,
        line: tokens[index].line,
        column: tokens[index].column
      };
    }
    return null;
  }

  extractProcessInstance(tokens, index) {
    // Look for new ProcessInstance("ProcessName") pattern
    let i = index + 1;
    while (i < tokens.length) {
      if (tokens[i].type === TokenType.STRING) {
        return {
          processId: tokens[i].value.replace(/["']/g, ''),
          line: tokens[index].line
        };
      }
      if (tokens[i].value === ';' || tokens[i].value === ')') break;
      i++;
    }
    return null;
  }

  runDiagnostics(code, tokens, analysis) {
    // Check for common issues

    // Missing jsAutoGC
    if (!code.includes('jsAutoGC')) {
      analysis.diagnostics.push({
        severity: 'hint',
        message: 'Consider enabling jsAutoGC = true for automatic garbage collection',
        line: 1
      });
    }

    // Check for deprecated patterns
    if (code.includes('__base__') && !code.includes('prototype')) {
      analysis.diagnostics.push({
        severity: 'info',
        message: 'Using __base__ inheritance pattern - ensure prototype is properly set',
        line: 1
      });
    }

    // Missing version check
    if (!code.includes('__PI_VERSION__') && analysis.includes.length > 0) {
      analysis.diagnostics.push({
        severity: 'warning',
        message: 'Consider adding a PixInsight version check with #iflt __PI_VERSION__',
        line: 1
      });
    }

    // Check beginProcess/endProcess pairing
    const beginCount = (code.match(/\.beginProcess\s*\(/g) || []).length;
    const endCount = (code.match(/\.endProcess\s*\(/g) || []).length;
    if (beginCount !== endCount) {
      analysis.diagnostics.push({
        severity: 'error',
        message: `Mismatched beginProcess/endProcess calls (${beginCount} begins, ${endCount} ends)`,
        line: 1
      });
    }
  }

  /**
   * Get completion suggestions at a given position
   */
  getCompletions(code, line, column) {
    const lexer = new PJSRLexer(code);
    const tokens = lexer.tokenize();

    // Find context at position
    const context = this.getContextAtPosition(tokens, line, column);
    const completions = [];

    // After 'new' keyword - suggest classes
    if (context.afterNew) {
      for (const className of Object.keys(this.schema.coreClasses || {})) {
        const classInfo = this.schema.coreClasses[className];
        completions.push({
          label: className,
          kind: 'class',
          detail: classInfo.description,
          insertText: className
        });
      }
      for (const className of Object.keys(this.schema.uiControls || {})) {
        const classInfo = this.schema.uiControls[className];
        completions.push({
          label: className,
          kind: 'class',
          detail: classInfo.description,
          insertText: className
        });
      }
    }

    // After dot - suggest members
    if (context.afterDot && context.objectName) {
      const classInfo = this.getClassInfo(context.objectName);
      if (classInfo) {
        // Methods
        for (const [name, info] of Object.entries(classInfo.methods || {})) {
          completions.push({
            label: name,
            kind: 'method',
            detail: info.signature || info.description,
            insertText: name
          });
        }
        // Properties
        for (const [name, info] of Object.entries(classInfo.properties || {})) {
          completions.push({
            label: name,
            kind: 'property',
            detail: `${info.type} - ${info.description}`,
            insertText: name
          });
        }
        // Event handlers
        for (const [name, info] of Object.entries(classInfo.eventHandlers || {})) {
          completions.push({
            label: name,
            kind: 'event',
            detail: info.description,
            insertText: name
          });
        }
      }
    }

    // Global completions
    if (!context.afterDot && !context.afterNew) {
      // Keywords
      for (const kw of KEYWORDS) {
        completions.push({
          label: kw,
          kind: 'keyword',
          insertText: kw
        });
      }

      // Global objects
      for (const [name, info] of Object.entries(this.schema.globalObjects || {})) {
        completions.push({
          label: name,
          kind: 'variable',
          detail: info.description,
          insertText: name
        });
      }

      // Classes
      for (const className of Object.keys(this.schema.coreClasses || {})) {
        completions.push({
          label: className,
          kind: 'class',
          insertText: className
        });
      }

      // Constants
      for (const [category, constants] of Object.entries(this.schema.constants || {})) {
        for (const [name, info] of Object.entries(constants)) {
          completions.push({
            label: name,
            kind: 'constant',
            detail: info.description,
            insertText: name
          });
        }
      }
    }

    return completions;
  }

  getContextAtPosition(tokens, line, column) {
    const context = {
      afterDot: false,
      afterNew: false,
      objectName: null,
      inString: false,
      inComment: false
    };

    let prevToken = null;
    let prevPrevToken = null;

    for (const token of tokens) {
      if (token.line > line || (token.line === line && token.column > column)) {
        break;
      }

      if (token.type !== TokenType.WHITESPACE) {
        prevPrevToken = prevToken;
        prevToken = token;
      }

      if (token.line === line && token.column <= column) {
        if (token.type === TokenType.STRING) {
          context.inString = true;
        } else if (token.type === TokenType.COMMENT) {
          context.inComment = true;
        }
      }
    }

    if (prevToken) {
      if (prevToken.value === '.') {
        context.afterDot = true;
        if (prevPrevToken && prevPrevToken.type === TokenType.IDENTIFIER) {
          context.objectName = prevPrevToken.value;
        }
      } else if (prevToken.value === 'new') {
        context.afterNew = true;
      }
    }

    return context;
  }

  getClassInfo(className) {
    return this.schema.coreClasses?.[className] ||
           this.schema.uiControls?.[className] ||
           this.schema.globalObjects?.[className];
  }

  /**
   * Get hover information for a symbol
   */
  getHoverInfo(symbol) {
    // Check classes
    const classInfo = this.getClassInfo(symbol);
    if (classInfo) {
      let markdown = `## ${symbol}\n\n${classInfo.description}\n`;

      if (classInfo.constructors) {
        markdown += '\n### Constructors\n';
        for (const ctor of classInfo.constructors) {
          markdown += `- \`${ctor.signature}\` - ${ctor.description}\n`;
        }
      }

      if (classInfo.properties) {
        markdown += '\n### Properties\n';
        for (const [name, info] of Object.entries(classInfo.properties)) {
          markdown += `- \`${name}: ${info.type}\` - ${info.description}\n`;
        }
      }

      if (classInfo.methods) {
        markdown += '\n### Methods\n';
        for (const [name, info] of Object.entries(classInfo.methods)) {
          markdown += `- \`${info.signature || name}\` - ${info.description}\n`;
        }
      }

      return markdown;
    }

    // Check constants
    for (const [category, constants] of Object.entries(this.schema.constants || {})) {
      if (symbol in constants) {
        const info = constants[symbol];
        return `**${symbol}** = ${info.value}\n\n${info.description}`;
      }
    }

    // Check preprocessor directives
    if (symbol.startsWith('#')) {
      const directive = this.schema.preprocessorDirectives?.[symbol];
      if (directive) {
        return `**${symbol}**\n\n${directive.description}\n\nExample: \`${directive.example}\``;
      }
    }

    return null;
  }

  /**
   * Get code templates/snippets
   */
  getTemplates() {
    return this.schema.commonPatterns || {};
  }

  /**
   * Validate PJSR code
   */
  validate(code) {
    const analysis = this.analyze(code);
    return {
      valid: analysis.diagnostics.filter(d => d.severity === 'error').length === 0,
      diagnostics: analysis.diagnostics
    };
  }
}

export { PJSRLexer, PJSRAnalyzer, TokenType, pjsrSchema };
