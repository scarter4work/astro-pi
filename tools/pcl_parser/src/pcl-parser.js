/**
 * PCL Parser - PixInsight Class Library C++ Parser
 *
 * A lexer and semantic analyzer for PCL-based C++ code used in
 * PixInsight module development.
 *
 * PCL Documentation: https://gitlab.com/pixinsight/PCL
 * Reference: https://pixinsight.com/developer/pcl/doc/html/
 */

import { readFileSync } from 'fs';
import { fileURLToPath } from 'url';
import { dirname, join } from 'path';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

// Load schemas
let pclCoreSchema = {};
let pclExtendedSchema = {};

try {
  pclCoreSchema = JSON.parse(readFileSync(join(__dirname, '../schemas/pcl-core.json'), 'utf8'));
  pclExtendedSchema = JSON.parse(readFileSync(join(__dirname, '../schemas/pcl-extended.json'), 'utf8'));
} catch (e) {
  // Schemas will be loaded when available
}

/**
 * Token types for C++ lexing
 */
const TokenType = {
  KEYWORD: 'keyword',
  IDENTIFIER: 'identifier',
  NUMBER: 'number',
  STRING: 'string',
  CHAR: 'char',
  OPERATOR: 'operator',
  PUNCTUATION: 'punctuation',
  COMMENT: 'comment',
  PREPROCESSOR: 'preprocessor',
  WHITESPACE: 'whitespace',
  TEMPLATE: 'template',
  NAMESPACE: 'namespace',
  EOF: 'eof'
};

/**
 * C++ keywords
 */
const CPP_KEYWORDS = new Set([
  // Standard C++ keywords
  'alignas', 'alignof', 'and', 'and_eq', 'asm', 'auto', 'bitand', 'bitor',
  'bool', 'break', 'case', 'catch', 'char', 'char8_t', 'char16_t', 'char32_t',
  'class', 'compl', 'concept', 'const', 'consteval', 'constexpr', 'constinit',
  'const_cast', 'continue', 'co_await', 'co_return', 'co_yield', 'decltype',
  'default', 'delete', 'do', 'double', 'dynamic_cast', 'else', 'enum',
  'explicit', 'export', 'extern', 'false', 'float', 'for', 'friend', 'goto',
  'if', 'inline', 'int', 'long', 'mutable', 'namespace', 'new', 'noexcept',
  'not', 'not_eq', 'nullptr', 'operator', 'or', 'or_eq', 'private', 'protected',
  'public', 'register', 'reinterpret_cast', 'requires', 'return', 'short',
  'signed', 'sizeof', 'static', 'static_assert', 'static_cast', 'struct',
  'switch', 'template', 'this', 'thread_local', 'throw', 'true', 'try',
  'typedef', 'typeid', 'typename', 'union', 'unsigned', 'using', 'virtual',
  'void', 'volatile', 'wchar_t', 'while', 'xor', 'xor_eq',
  // PCL-specific macros that act like keywords
  'override', 'final', 'PCL_CLASS', 'PCL_FUNC', 'PCL_MEMBER'
]);

/**
 * PCL namespace identifiers
 */
const PCL_NAMESPACES = new Set([
  'pcl', 'pi'
]);

/**
 * Multi-character operators
 */
const MULTI_CHAR_OPERATORS = [
  ':::', '::', '->*', '->', '<<', '>>', '<=', '>=', '==', '!=',
  '&&', '||', '++', '--', '+=', '-=', '*=', '/=', '%=', '&=',
  '|=', '^=', '<<=', '>>=', '<=>', '...'
];

/**
 * PCL Lexer - Tokenizes C++ source code
 */
class PCLLexer {
  constructor(code) {
    this.code = code;
    this.pos = 0;
    this.line = 1;
    this.column = 1;
    this.tokens = [];
  }

  /**
   * Main tokenization entry point
   */
  tokenize() {
    while (this.pos < this.code.length) {
      const token = this.nextToken();
      if (token && token.type !== TokenType.WHITESPACE) {
        this.tokens.push(token);
      }
    }
    this.tokens.push(this.makeToken(TokenType.EOF, ''));
    return this.tokens;
  }

  makeToken(type, value) {
    return {
      type,
      value,
      line: this.line,
      column: this.column - value.length
    };
  }

  peek(offset = 0) {
    return this.code[this.pos + offset] || '';
  }

  advance() {
    const ch = this.code[this.pos++];
    if (ch === '\n') {
      this.line++;
      this.column = 1;
    } else {
      this.column++;
    }
    return ch;
  }

  nextToken() {
    const ch = this.peek();

    // Whitespace
    if (/\s/.test(ch)) {
      return this.readWhitespace();
    }

    // Preprocessor directive
    if (ch === '#' && (this.column === 1 || this.isStartOfLine())) {
      return this.readPreprocessor();
    }

    // Comments
    if (ch === '/') {
      if (this.peek(1) === '/') {
        return this.readSingleLineComment();
      }
      if (this.peek(1) === '*') {
        return this.readMultiLineComment();
      }
    }

    // String literals
    if (ch === '"') {
      return this.readString();
    }

    // Raw string literals
    if (ch === 'R' && this.peek(1) === '"') {
      return this.readRawString();
    }

    // Character literals
    if (ch === "'") {
      return this.readChar();
    }

    // Numbers
    if (/\d/.test(ch) || (ch === '.' && /\d/.test(this.peek(1)))) {
      return this.readNumber();
    }

    // Identifiers and keywords
    if (/[a-zA-Z_]/.test(ch)) {
      return this.readIdentifier();
    }

    // Multi-character operators
    for (const op of MULTI_CHAR_OPERATORS) {
      if (this.matchAhead(op)) {
        const startCol = this.column;
        for (let i = 0; i < op.length; i++) this.advance();
        return { type: TokenType.OPERATOR, value: op, line: this.line, column: startCol };
      }
    }

    // Single character operators and punctuation
    if (/[+\-*/%&|^~!<>=?:]/.test(ch)) {
      this.advance();
      return this.makeToken(TokenType.OPERATOR, ch);
    }

    if (/[{}()\[\];,.]/.test(ch)) {
      this.advance();
      return this.makeToken(TokenType.PUNCTUATION, ch);
    }

    // Unknown character - advance anyway
    this.advance();
    return this.makeToken(TokenType.PUNCTUATION, ch);
  }

  matchAhead(str) {
    for (let i = 0; i < str.length; i++) {
      if (this.peek(i) !== str[i]) return false;
    }
    return true;
  }

  isStartOfLine() {
    // Check if we're at the start of a line (only whitespace before #)
    let i = this.pos - 1;
    while (i >= 0 && this.code[i] !== '\n') {
      if (!/\s/.test(this.code[i])) return false;
      i--;
    }
    return true;
  }

  readWhitespace() {
    const start = this.pos;
    while (this.pos < this.code.length && /\s/.test(this.peek())) {
      this.advance();
    }
    return this.makeToken(TokenType.WHITESPACE, this.code.slice(start, this.pos));
  }

  readPreprocessor() {
    const start = this.pos;
    const startLine = this.line;

    // Read until end of line (handling line continuations)
    while (this.pos < this.code.length) {
      if (this.peek() === '\n') {
        // Check for line continuation
        if (this.pos > 0 && this.code[this.pos - 1] === '\\') {
          this.advance();
          continue;
        }
        break;
      }
      this.advance();
    }

    return {
      type: TokenType.PREPROCESSOR,
      value: this.code.slice(start, this.pos).trim(),
      line: startLine,
      column: 1
    };
  }

  readSingleLineComment() {
    const start = this.pos;
    while (this.pos < this.code.length && this.peek() !== '\n') {
      this.advance();
    }
    return this.makeToken(TokenType.COMMENT, this.code.slice(start, this.pos));
  }

  readMultiLineComment() {
    const start = this.pos;
    const startLine = this.line;
    const startCol = this.column;

    this.advance(); // /
    this.advance(); // *

    while (this.pos < this.code.length) {
      if (this.peek() === '*' && this.peek(1) === '/') {
        this.advance();
        this.advance();
        break;
      }
      this.advance();
    }

    return {
      type: TokenType.COMMENT,
      value: this.code.slice(start, this.pos),
      line: startLine,
      column: startCol
    };
  }

  readString() {
    const start = this.pos;
    const startCol = this.column;

    this.advance(); // Opening quote

    while (this.pos < this.code.length) {
      const ch = this.peek();
      if (ch === '\\') {
        this.advance();
        this.advance(); // Skip escaped character
      } else if (ch === '"') {
        this.advance();
        break;
      } else if (ch === '\n') {
        break; // Unterminated string
      } else {
        this.advance();
      }
    }

    return {
      type: TokenType.STRING,
      value: this.code.slice(start, this.pos),
      line: this.line,
      column: startCol
    };
  }

  readRawString() {
    const start = this.pos;
    const startCol = this.column;

    this.advance(); // R
    this.advance(); // Opening "

    // Read delimiter
    let delimiter = '';
    while (this.pos < this.code.length && this.peek() !== '(') {
      delimiter += this.advance();
    }
    this.advance(); // (

    // Read until )delimiter"
    const endPattern = ')' + delimiter + '"';
    while (this.pos < this.code.length) {
      if (this.matchAhead(endPattern)) {
        for (let i = 0; i < endPattern.length; i++) this.advance();
        break;
      }
      this.advance();
    }

    return {
      type: TokenType.STRING,
      value: this.code.slice(start, this.pos),
      line: this.line,
      column: startCol
    };
  }

  readChar() {
    const start = this.pos;
    const startCol = this.column;

    this.advance(); // Opening quote

    if (this.peek() === '\\') {
      this.advance();
      this.advance(); // Escaped character
    } else {
      this.advance();
    }

    if (this.peek() === "'") {
      this.advance();
    }

    return {
      type: TokenType.CHAR,
      value: this.code.slice(start, this.pos),
      line: this.line,
      column: startCol
    };
  }

  readNumber() {
    const start = this.pos;
    const startCol = this.column;

    // Handle hex, binary, octal
    if (this.peek() === '0') {
      this.advance();
      if (this.peek() === 'x' || this.peek() === 'X') {
        this.advance();
        while (/[0-9a-fA-F']/.test(this.peek())) this.advance();
      } else if (this.peek() === 'b' || this.peek() === 'B') {
        this.advance();
        while (/[01']/.test(this.peek())) this.advance();
      } else {
        while (/[0-7']/.test(this.peek())) this.advance();
      }
    } else {
      // Decimal
      while (/[0-9']/.test(this.peek())) this.advance();
    }

    // Decimal point
    if (this.peek() === '.' && /\d/.test(this.peek(1))) {
      this.advance();
      while (/[0-9']/.test(this.peek())) this.advance();
    }

    // Exponent
    if (this.peek() === 'e' || this.peek() === 'E') {
      this.advance();
      if (this.peek() === '+' || this.peek() === '-') this.advance();
      while (/[0-9']/.test(this.peek())) this.advance();
    }

    // Suffix (f, F, l, L, u, U, ll, LL, etc.)
    while (/[fFlLuU]/.test(this.peek())) this.advance();

    return {
      type: TokenType.NUMBER,
      value: this.code.slice(start, this.pos),
      line: this.line,
      column: startCol
    };
  }

  readIdentifier() {
    const start = this.pos;
    const startCol = this.column;

    while (this.pos < this.code.length && /[a-zA-Z0-9_]/.test(this.peek())) {
      this.advance();
    }

    const value = this.code.slice(start, this.pos);
    const type = CPP_KEYWORDS.has(value) ? TokenType.KEYWORD : TokenType.IDENTIFIER;

    return {
      type,
      value,
      line: this.line,
      column: startCol
    };
  }
}

/**
 * PCL Analyzer - Semantic analysis of PCL C++ code
 */
class PCLAnalyzer {
  constructor() {
    this.coreSchema = pclCoreSchema;
    this.extendedSchema = pclExtendedSchema;
  }

  /**
   * Analyze PCL C++ code
   */
  analyze(code) {
    const lexer = new PCLLexer(code);
    const tokens = lexer.tokenize();

    const result = {
      tokens,
      includes: [],
      defines: [],
      namespaces: [],
      classes: [],
      functions: [],
      variables: [],
      pclObjects: [],
      diagnostics: []
    };

    // Extract preprocessor directives
    for (const token of tokens) {
      if (token.type === TokenType.PREPROCESSOR) {
        this.analyzePreprocessor(token, result);
      }
    }

    // Analyze code structure
    this.analyzeStructure(tokens, result);

    // Detect PCL usage
    this.detectPCLUsage(tokens, result);

    // Run diagnostics
    this.runDiagnostics(tokens, result);

    return result;
  }

  analyzePreprocessor(token, result) {
    const value = token.value;

    if (value.startsWith('#include')) {
      const match = value.match(/#include\s*[<"](.+?)[>"]/);
      if (match) {
        result.includes.push({
          file: match[1],
          isPCL: match[1].startsWith('pcl/'),
          line: token.line
        });
      }
    } else if (value.startsWith('#define')) {
      const match = value.match(/#define\s+(\w+)(?:\s+(.*))?/);
      if (match) {
        result.defines.push({
          name: match[1],
          value: match[2] || '',
          line: token.line
        });
      }
    }
  }

  analyzeStructure(tokens, result) {
    let i = 0;
    let currentNamespace = null;
    let braceDepth = 0;

    while (i < tokens.length) {
      const token = tokens[i];

      // Track namespace
      if (token.type === TokenType.KEYWORD && token.value === 'namespace') {
        const next = tokens[i + 1];
        if (next && next.type === TokenType.IDENTIFIER) {
          currentNamespace = next.value;
          result.namespaces.push({
            name: next.value,
            line: token.line
          });
        }
      }

      // Track class/struct declarations
      if (token.type === TokenType.KEYWORD &&
          (token.value === 'class' || token.value === 'struct')) {
        const classInfo = this.parseClassDeclaration(tokens, i);
        if (classInfo) {
          classInfo.namespace = currentNamespace;
          result.classes.push(classInfo);
        }
      }

      // Track function declarations
      if (token.type === TokenType.IDENTIFIER) {
        const funcInfo = this.parseFunctionDeclaration(tokens, i);
        if (funcInfo) {
          result.functions.push(funcInfo);
        }
      }

      // Track brace depth for namespace scope
      if (token.type === TokenType.PUNCTUATION) {
        if (token.value === '{') braceDepth++;
        if (token.value === '}') {
          braceDepth--;
          if (braceDepth === 0) currentNamespace = null;
        }
      }

      i++;
    }
  }

  parseClassDeclaration(tokens, startIndex) {
    let i = startIndex + 1;

    // Skip forward declaration markers
    while (i < tokens.length && tokens[i].type === TokenType.KEYWORD) {
      i++;
    }

    if (i >= tokens.length || tokens[i].type !== TokenType.IDENTIFIER) {
      return null;
    }

    const className = tokens[i].value;
    let baseClasses = [];

    i++;

    // Check for inheritance
    if (i < tokens.length && tokens[i].type === TokenType.OPERATOR && tokens[i].value === ':') {
      i++;
      while (i < tokens.length) {
        if (tokens[i].type === TokenType.PUNCTUATION && tokens[i].value === '{') break;
        if (tokens[i].type === TokenType.IDENTIFIER) {
          baseClasses.push(tokens[i].value);
        }
        i++;
      }
    }

    return {
      name: className,
      baseClasses,
      line: tokens[startIndex].line,
      isStruct: tokens[startIndex].value === 'struct'
    };
  }

  parseFunctionDeclaration(tokens, index) {
    // Look for pattern: returnType functionName(
    if (index + 2 >= tokens.length) return null;

    const current = tokens[index];
    const next = tokens[index + 1];

    if (next && next.type === TokenType.PUNCTUATION && next.value === '(') {
      // This might be a function call or declaration
      // Look backwards for return type
      let returnType = '';
      let j = index - 1;
      while (j >= 0 && (tokens[j].type === TokenType.IDENTIFIER ||
                         tokens[j].type === TokenType.KEYWORD ||
                         (tokens[j].type === TokenType.OPERATOR &&
                          (tokens[j].value === '*' || tokens[j].value === '&' || tokens[j].value === '::')))) {
        returnType = tokens[j].value + ' ' + returnType;
        j--;
      }

      if (returnType.trim()) {
        return {
          name: current.value,
          returnType: returnType.trim(),
          line: current.line
        };
      }
    }

    return null;
  }

  detectPCLUsage(tokens, result) {
    const pclClasses = this.getAllPCLClasses();
    const usedClasses = new Set();

    for (let i = 0; i < tokens.length; i++) {
      const token = tokens[i];

      // Check for pcl:: namespace usage
      if (token.type === TokenType.IDENTIFIER && token.value === 'pcl') {
        if (i + 2 < tokens.length &&
            tokens[i + 1].type === TokenType.OPERATOR &&
            tokens[i + 1].value === '::') {
          const className = tokens[i + 2].value;
          if (pclClasses.has(className)) {
            usedClasses.add(className);
          }
        }
      }

      // Check for direct class usage (after using namespace pcl)
      if (token.type === TokenType.IDENTIFIER && pclClasses.has(token.value)) {
        usedClasses.add(token.value);
      }
    }

    result.pclObjects = Array.from(usedClasses).map(name => ({
      name,
      category: this.getClassCategory(name)
    }));
  }

  getAllPCLClasses() {
    const classes = new Set();

    if (this.coreSchema.classes) {
      Object.keys(this.coreSchema.classes).forEach(c => classes.add(c));
    }
    if (this.extendedSchema.classes) {
      Object.keys(this.extendedSchema.classes).forEach(c => classes.add(c));
    }

    return classes;
  }

  getClassCategory(className) {
    if (this.coreSchema.classes?.[className]?.category) {
      return this.coreSchema.classes[className].category;
    }
    if (this.extendedSchema.classes?.[className]?.category) {
      return this.extendedSchema.classes[className].category;
    }
    return 'unknown';
  }

  runDiagnostics(tokens, result) {
    // Check for common issues

    // Missing PCL includes
    const hasPCLUsage = result.pclObjects.length > 0;
    const hasPCLIncludes = result.includes.some(inc => inc.isPCL);

    if (hasPCLUsage && !hasPCLIncludes) {
      result.diagnostics.push({
        severity: 'warning',
        message: 'PCL classes are used but no PCL headers are included',
        line: 1
      });
    }

    // Check for namespace usage
    const hasUsingPCL = tokens.some((t, i) =>
      t.value === 'using' &&
      tokens[i + 1]?.value === 'namespace' &&
      tokens[i + 2]?.value === 'pcl'
    );

    if (!hasUsingPCL && result.pclObjects.length > 5) {
      result.diagnostics.push({
        severity: 'hint',
        message: 'Consider using "using namespace pcl;" to reduce verbosity',
        line: 1
      });
    }

    // Check for MetaModule requirement
    const hasMetaModule = result.classes.some(c =>
      c.baseClasses.includes('MetaModule')
    );
    const hasModuleFile = result.includes.some(inc =>
      inc.file.includes('MetaModule')
    );

    if (hasMetaModule && !hasModuleFile) {
      result.diagnostics.push({
        severity: 'error',
        message: 'Class inherits from MetaModule but pcl/MetaModule.h is not included',
        line: 1
      });
    }
  }

  /**
   * Get code completions at a position
   */
  getCompletions(code, line, column) {
    const lexer = new PCLLexer(code);
    const tokens = lexer.tokenize();

    // Find context at position
    const context = this.getContextAtPosition(tokens, line, column);
    const completions = [];

    if (context.afterScope) {
      // After :: - suggest class members
      const className = context.scopeClass;
      const classInfo = this.getClassInfo(className);

      if (classInfo) {
        // Add methods
        if (classInfo.methods) {
          Object.entries(classInfo.methods).forEach(([name, info]) => {
            completions.push({
              label: name,
              kind: 'method',
              detail: info.signature || '',
              documentation: info.description || ''
            });
          });
        }

        // Add static members
        if (classInfo.staticMethods) {
          Object.entries(classInfo.staticMethods).forEach(([name, info]) => {
            completions.push({
              label: name,
              kind: 'method',
              detail: info.signature || '',
              documentation: info.description || ''
            });
          });
        }
      }
    } else if (context.afterDot || context.afterArrow) {
      // After . or -> - suggest instance members
      // Would need type inference for proper suggestions
      completions.push({
        label: '(member access)',
        kind: 'info',
        detail: 'Type inference not available'
      });
    } else if (context.afterInclude) {
      // After #include - suggest PCL headers
      this.getPCLHeaders().forEach(header => {
        completions.push({
          label: header,
          kind: 'file',
          detail: 'PCL header'
        });
      });
    } else if (context.afterNew) {
      // After new - suggest classes
      this.getAllPCLClasses().forEach(className => {
        completions.push({
          label: className,
          kind: 'class',
          detail: this.getClassCategory(className)
        });
      });
    } else {
      // Global context - suggest namespaces, classes, keywords
      completions.push({
        label: 'pcl',
        kind: 'namespace',
        detail: 'PixInsight Class Library namespace'
      });

      // Add common PCL classes
      const commonClasses = [
        'Image', 'ImageWindow', 'View', 'Console', 'ProcessInterface',
        'ProcessImplementation', 'MetaProcess', 'MetaModule', 'String',
        'Array', 'Vector', 'Matrix', 'File', 'Thread'
      ];

      commonClasses.forEach(className => {
        if (this.coreSchema.classes?.[className] || this.extendedSchema.classes?.[className]) {
          completions.push({
            label: className,
            kind: 'class',
            detail: 'PCL class'
          });
        }
      });
    }

    return completions;
  }

  getContextAtPosition(tokens, line, column) {
    const context = {
      afterScope: false,
      afterDot: false,
      afterArrow: false,
      afterInclude: false,
      afterNew: false,
      scopeClass: null
    };

    // Find the token at or just before the position
    let prevToken = null;
    let prevPrevToken = null;

    for (const token of tokens) {
      if (token.line > line || (token.line === line && token.column > column)) {
        break;
      }
      prevPrevToken = prevToken;
      prevToken = token;
    }

    if (prevToken) {
      if (prevToken.value === '::') {
        context.afterScope = true;
        if (prevPrevToken && prevPrevToken.type === TokenType.IDENTIFIER) {
          context.scopeClass = prevPrevToken.value;
        }
      } else if (prevToken.value === '.') {
        context.afterDot = true;
      } else if (prevToken.value === '->') {
        context.afterArrow = true;
      } else if (prevToken.type === TokenType.PREPROCESSOR &&
                 prevToken.value.includes('#include')) {
        context.afterInclude = true;
      } else if (prevToken.value === 'new') {
        context.afterNew = true;
      }
    }

    return context;
  }

  /**
   * Get hover information for a symbol
   */
  getHoverInfo(symbol) {
    // Check classes
    let classInfo = this.coreSchema.classes?.[symbol] || this.extendedSchema.classes?.[symbol];
    if (classInfo) {
      return this.formatClassHover(symbol, classInfo);
    }

    // Check constants
    for (const category of Object.keys(this.coreSchema.constants || {})) {
      const constants = this.coreSchema.constants[category];
      if (constants[symbol]) {
        return this.formatConstantHover(symbol, constants[symbol], category);
      }
    }

    // Check typedefs
    if (this.coreSchema.typedefs?.[symbol]) {
      return this.formatTypedefHover(symbol, this.coreSchema.typedefs[symbol]);
    }

    return null;
  }

  formatClassHover(name, info) {
    let md = `## ${name}\n\n`;

    if (info.description) {
      md += `${info.description}\n\n`;
    }

    if (info.header) {
      md += `**Header:** \`#include <pcl/${info.header}>\`\n\n`;
    }

    if (info.baseClass) {
      md += `**Inherits:** ${info.baseClass}\n\n`;
    }

    if (info.constructors && info.constructors.length > 0) {
      md += '### Constructors\n\n';
      info.constructors.forEach(ctor => {
        md += `- \`${ctor.signature}\`\n`;
        if (ctor.description) {
          md += `  ${ctor.description}\n`;
        }
      });
      md += '\n';
    }

    if (info.methods && Object.keys(info.methods).length > 0) {
      md += '### Methods\n\n';
      Object.entries(info.methods).forEach(([methodName, methodInfo]) => {
        md += `- \`${methodInfo.signature || methodName + '()'}\`\n`;
        if (methodInfo.description) {
          md += `  ${methodInfo.description}\n`;
        }
      });
    }

    return md;
  }

  formatConstantHover(name, info, category) {
    let md = `## ${name}\n\n`;
    md += `**Category:** ${category}\n\n`;

    if (typeof info === 'object') {
      if (info.value !== undefined) {
        md += `**Value:** \`${info.value}\`\n\n`;
      }
      if (info.description) {
        md += info.description + '\n';
      }
    } else {
      md += `**Value:** \`${info}\`\n`;
    }

    return md;
  }

  formatTypedefHover(name, info) {
    let md = `## ${name}\n\n`;
    md += '**Type alias**\n\n';

    if (info.definition) {
      md += `\`\`\`cpp\ntypedef ${info.definition} ${name};\n\`\`\`\n\n`;
    }

    if (info.description) {
      md += info.description + '\n';
    }

    return md;
  }

  /**
   * Validate PCL code
   */
  validate(code) {
    const analysis = this.analyze(code);
    return {
      valid: analysis.diagnostics.filter(d => d.severity === 'error').length === 0,
      diagnostics: analysis.diagnostics,
      summary: {
        errors: analysis.diagnostics.filter(d => d.severity === 'error').length,
        warnings: analysis.diagnostics.filter(d => d.severity === 'warning').length,
        hints: analysis.diagnostics.filter(d => d.severity === 'hint').length
      }
    };
  }

  /**
   * Get class information
   */
  getClassInfo(className) {
    return this.coreSchema.classes?.[className] ||
           this.extendedSchema.classes?.[className] ||
           null;
  }

  /**
   * List all PCL classes
   */
  listClasses() {
    const classes = [];

    if (this.coreSchema.classes) {
      Object.entries(this.coreSchema.classes).forEach(([name, info]) => {
        classes.push({
          name,
          category: info.category || 'core',
          description: info.description || ''
        });
      });
    }

    if (this.extendedSchema.classes) {
      Object.entries(this.extendedSchema.classes).forEach(([name, info]) => {
        classes.push({
          name,
          category: info.category || 'extended',
          description: info.description || ''
        });
      });
    }

    return classes.sort((a, b) => a.name.localeCompare(b.name));
  }

  /**
   * Get PCL headers
   */
  getPCLHeaders() {
    return [
      'pcl/AbstractImage.h', 'pcl/Action.h', 'pcl/Array.h',
      'pcl/AstrometricMetadata.h', 'pcl/Bitmap.h', 'pcl/Button.h',
      'pcl/CheckBox.h', 'pcl/CodeEditor.h', 'pcl/Color.h',
      'pcl/ComboBox.h', 'pcl/Console.h', 'pcl/Control.h',
      'pcl/Convolution.h', 'pcl/Dialog.h', 'pcl/Edit.h',
      'pcl/ErrorHandler.h', 'pcl/Exception.h', 'pcl/FFTConvolution.h',
      'pcl/File.h', 'pcl/FileDialog.h', 'pcl/FileFormat.h',
      'pcl/Font.h', 'pcl/Frame.h', 'pcl/Graphics.h',
      'pcl/GroupBox.h', 'pcl/Histogram.h', 'pcl/Image.h',
      'pcl/ImageWindow.h', 'pcl/Label.h', 'pcl/Math.h',
      'pcl/Matrix.h', 'pcl/MetaModule.h', 'pcl/MetaParameter.h',
      'pcl/MetaProcess.h', 'pcl/MorphologicalTransformation.h',
      'pcl/NumericControl.h', 'pcl/Pen.h', 'pcl/PixelInterpolation.h',
      'pcl/Point.h', 'pcl/ProcessImplementation.h', 'pcl/ProcessInterface.h',
      'pcl/ProcessInstance.h', 'pcl/PushButton.h', 'pcl/RadioButton.h',
      'pcl/Rectangle.h', 'pcl/Resample.h', 'pcl/ScrollBox.h',
      'pcl/SectionBar.h', 'pcl/Settings.h', 'pcl/Sizer.h',
      'pcl/Slider.h', 'pcl/SpinBox.h', 'pcl/StarDetector.h',
      'pcl/StatusMonitor.h', 'pcl/String.h', 'pcl/StringList.h',
      'pcl/TabBox.h', 'pcl/TextBox.h', 'pcl/Thread.h',
      'pcl/Timer.h', 'pcl/ToolButton.h', 'pcl/TreeBox.h',
      'pcl/Vector.h', 'pcl/View.h', 'pcl/ViewList.h',
      'pcl/XISF.h', 'pcl/XML.h'
    ];
  }

  /**
   * Get code templates
   */
  getTemplates() {
    return {
      moduleDefinition: {
        name: 'Module Definition',
        description: 'Basic PixInsight module structure',
        code: `#define MODULE_VERSION_MAJOR     1
#define MODULE_VERSION_MINOR     0
#define MODULE_VERSION_REVISION  0
#define MODULE_VERSION_BUILD     1
#define MODULE_VERSION_LANGUAGE  "eng"

#define MODULE_RELEASE_YEAR      2024
#define MODULE_RELEASE_MONTH     1
#define MODULE_RELEASE_DAY       1

#include <pcl/MetaModule.h>

namespace MyModule
{

class MyModule : public pcl::MetaModule
{
public:
   MyModule();

   const char* Version() const override;
   IsoString Name() const override;
   String Description() const override;
   String Company() const override;
   String Author() const override;
   String Copyright() const override;
   String TradeMarks() const override;
   String OriginalFileName() const override;
   void GetReleaseDate( int& year, int& month, int& day ) const override;
};

} // namespace MyModule`
      },
      processInterface: {
        name: 'Process Interface',
        description: 'GUI interface for a process',
        code: `#include <pcl/ProcessInterface.h>
#include <pcl/Sizer.h>
#include <pcl/Label.h>
#include <pcl/NumericControl.h>
#include <pcl/PushButton.h>

namespace MyProcess
{

class MyProcessInterface : public pcl::ProcessInterface
{
public:
   MyProcessInterface();
   virtual ~MyProcessInterface();

   IsoString Id() const override;
   MetaProcess* Process() const override;
   IsoString IconImageSVG() const override;
   InterfaceFeatures Features() const override;

   void ApplyInstance() const override;
   void ResetInstance() override;

   bool Launch( const MetaProcess&, const ProcessImplementation*, bool& dynamic, unsigned& flags ) override;

   ProcessImplementation* NewProcess() const override;

   bool ValidateProcess( const ProcessImplementation&, String& whyNot ) const override;
   bool RequiresInstanceValidation() const override;

   bool ImportProcess( const ProcessImplementation& ) override;

private:
   struct GUIData
   {
      GUIData( MyProcessInterface& );

      pcl::VerticalSizer   Global_Sizer;
      pcl::NumericControl  Parameter1_NumericControl;
      pcl::HorizontalSizer Buttons_Sizer;
      pcl::PushButton      Apply_Button;
      pcl::PushButton      Reset_Button;
   };

   GUIData* GUI = nullptr;

   void UpdateControls();
   void e_Click( Button& sender, bool checked );
   void e_ValueUpdated( NumericEdit& sender, double value );
};

} // namespace MyProcess`
      },
      imageProcessing: {
        name: 'Image Processing',
        description: 'Basic image processing with progress monitoring',
        code: `#include <pcl/Image.h>
#include <pcl/Console.h>
#include <pcl/StandardStatus.h>

void ProcessImage( pcl::Image& image )
{
   pcl::Console console;
   console.WriteLn( "Processing image..." );

   pcl::StandardStatus status;
   image.SetStatusCallback( &status );

   int numberOfChannels = image.NumberOfChannels();

   for ( int c = 0; c < numberOfChannels; ++c )
   {
      image.SelectChannel( c );

      for ( int y = 0; y < image.Height(); ++y )
      {
         for ( int x = 0; x < image.Width(); ++x )
         {
            // Access pixel at (x, y)
            double value = image( x, y );

            // Apply transformation
            value = pcl::Range( value * 1.5, 0.0, 1.0 );

            // Set new value
            image( x, y ) = value;
         }
      }
   }

   image.ResetSelections();
   console.WriteLn( "Done." );
}`
      },
      dialogTemplate: {
        name: 'Dialog Template',
        description: 'Custom dialog with controls',
        code: `#include <pcl/Dialog.h>
#include <pcl/Sizer.h>
#include <pcl/Label.h>
#include <pcl/Edit.h>
#include <pcl/SpinBox.h>
#include <pcl/CheckBox.h>
#include <pcl/PushButton.h>

class MyDialog : public pcl::Dialog
{
public:
   MyDialog()
   {
      Title_Label.SetText( "My Dialog" );
      Title_Label.SetTextAlignment( pcl::TextAlign::Center );

      Input_Edit.SetMinWidth( 300 );
      Input_Edit.OnEditCompleted( (Edit::edit_event_handler)&MyDialog::e_EditCompleted, *this );

      Value_SpinBox.SetRange( 0, 100 );
      Value_SpinBox.SetValue( 50 );

      Enable_CheckBox.SetText( "Enable feature" );
      Enable_CheckBox.SetChecked( true );

      OK_Button.SetText( "OK" );
      OK_Button.SetDefault();
      OK_Button.OnClick( (Button::click_event_handler)&MyDialog::e_Click, *this );

      Cancel_Button.SetText( "Cancel" );
      Cancel_Button.OnClick( (Button::click_event_handler)&MyDialog::e_Click, *this );

      Buttons_Sizer.SetSpacing( 8 );
      Buttons_Sizer.AddStretch();
      Buttons_Sizer.Add( OK_Button );
      Buttons_Sizer.Add( Cancel_Button );

      Global_Sizer.SetMargin( 8 );
      Global_Sizer.SetSpacing( 6 );
      Global_Sizer.Add( Title_Label );
      Global_Sizer.Add( Input_Edit );
      Global_Sizer.Add( Value_SpinBox );
      Global_Sizer.Add( Enable_CheckBox );
      Global_Sizer.AddSpacing( 8 );
      Global_Sizer.Add( Buttons_Sizer );

      SetSizer( Global_Sizer );
      AdjustToContents();
      SetFixedSize();
      SetWindowTitle( "My Dialog" );
   }

private:
   pcl::VerticalSizer    Global_Sizer;
   pcl::Label            Title_Label;
   pcl::Edit             Input_Edit;
   pcl::SpinBox          Value_SpinBox;
   pcl::CheckBox         Enable_CheckBox;
   pcl::HorizontalSizer  Buttons_Sizer;
   pcl::PushButton       OK_Button;
   pcl::PushButton       Cancel_Button;

   void e_EditCompleted( Edit& sender )
   {
      // Handle edit completion
   }

   void e_Click( Button& sender, bool checked )
   {
      if ( sender == OK_Button )
         Ok();
      else if ( sender == Cancel_Button )
         Cancel();
   }
};`
      },
      fileIO: {
        name: 'File I/O',
        description: 'Reading and writing files with XISF',
        code: `#include <pcl/XISF.h>
#include <pcl/Console.h>
#include <pcl/ErrorHandler.h>

void ReadXISF( const pcl::String& filePath, pcl::Image& image )
{
   pcl::Console console;

   try
   {
      pcl::XISFReader reader;
      reader.Open( filePath );

      if ( reader.NumberOfImages() == 0 )
         throw pcl::Error( "No images in file" );

      reader.SelectImage( 0 );
      reader.ReadImage( image );

      console.WriteLn( "Read: " + filePath );
      console.WriteLn( pcl::String().Format(
         "Dimensions: %d x %d x %d",
         image.Width(), image.Height(), image.NumberOfChannels() ) );

      reader.Close();
   }
   catch ( const pcl::Exception& e )
   {
      console.CriticalLn( "Error: " + e.Message() );
      throw;
   }
}

void WriteXISF( const pcl::String& filePath, const pcl::Image& image )
{
   pcl::Console console;

   try
   {
      pcl::XISFWriter writer;
      writer.Create( filePath );

      pcl::ImageOptions options;
      options.bitsPerSample = 32;
      options.ieeefpSampleFormat = true;

      writer.SetOptions( options );
      writer.WriteImage( image );

      console.WriteLn( "Wrote: " + filePath );

      writer.Close();
   }
   catch ( const pcl::Exception& e )
   {
      console.CriticalLn( "Error: " + e.Message() );
      throw;
   }
}`
      },
      threadedProcessing: {
        name: 'Threaded Processing',
        description: 'Parallel image processing with threads',
        code: `#include <pcl/Thread.h>
#include <pcl/Image.h>
#include <pcl/ReferenceArray.h>

class ProcessingThread : public pcl::Thread
{
public:
   ProcessingThread( pcl::Image& image, int startRow, int endRow )
      : m_image( image )
      , m_startRow( startRow )
      , m_endRow( endRow )
   {
   }

   void Run() override
   {
      for ( int y = m_startRow; y < m_endRow; ++y )
      {
         for ( int x = 0; x < m_image.Width(); ++x )
         {
            for ( int c = 0; c < m_image.NumberOfChannels(); ++c )
            {
               double value = m_image( x, y, c );
               // Process pixel
               value = pcl::Pow( value, 0.5 ); // Example: apply gamma
               m_image( x, y, c ) = value;
            }
         }
      }
   }

private:
   pcl::Image& m_image;
   int m_startRow;
   int m_endRow;
};

void ParallelProcess( pcl::Image& image )
{
   int numberOfThreads = pcl::Thread::NumberOfThreads( image.Height(), 16 );
   int rowsPerThread = image.Height() / numberOfThreads;

   pcl::ReferenceArray<ProcessingThread> threads;

   for ( int i = 0; i < numberOfThreads; ++i )
   {
      int startRow = i * rowsPerThread;
      int endRow = (i == numberOfThreads - 1) ? image.Height() : startRow + rowsPerThread;

      threads.Add( new ProcessingThread( image, startRow, endRow ) );
   }

   // Start all threads
   for ( auto& thread : threads )
      thread.Start();

   // Wait for completion
   for ( auto& thread : threads )
      thread.Wait();

   threads.Destroy();
}`
      }
    };
  }

  /**
   * Get best practices guide
   */
  getBestPractices(topic = 'general') {
    const practices = {
      general: `# PCL Best Practices

## Code Organization
- Use the \`pcl\` namespace or qualify with \`pcl::\`
- Separate interface and implementation files
- Follow PixInsight module structure conventions

## Memory Management
- PCL uses reference counting for many objects
- Use \`AutoPointer\` for automatic memory management
- Be careful with \`Image\` copies - they share pixel data by default

## Error Handling
- Use \`pcl::Exception\` for errors
- Implement proper cleanup in destructors
- Use \`ErrorHandler\` for global error handling

## Performance
- Use \`Thread\` for parallel processing
- Consider \`AbstractImage::Parallel\` for automatic parallelization
- Use \`FFTConvolution\` for large kernels`,

      imageProcessing: `# Image Processing Best Practices

## Pixel Access
- Use \`Image::Sample()\` for single pixel access
- Use iterators for sequential access
- Use \`Image::Apply()\` for bulk operations

## Color Spaces
- Check \`Image::ColorSpace()\` before processing
- Use \`Image::SetColorSpace()\` to convert
- Be aware of RGB vs grayscale differences

## Selections and Channels
- Use \`SelectChannel()\` for single-channel operations
- Call \`ResetSelections()\` when done
- Use \`FirstSelectedChannel()\` / \`LastSelectedChannel()\``,

      threading: `# Threading Best Practices

## Thread Usage
- Inherit from \`pcl::Thread\`
- Override \`Run()\` method
- Call \`Start()\` then \`Wait()\`

## Parallelization
- Use \`Thread::NumberOfThreads()\` to determine optimal count
- Divide work evenly among threads
- Avoid shared state when possible

## Synchronization
- Use \`Mutex\` for critical sections
- Use \`AutoLock\` for RAII locking
- Consider \`ReadWriteMutex\` for read-heavy scenarios`,

      ui: `# UI Development Best Practices

## Dialog Design
- Use \`Sizer\` classes for layout
- Call \`AdjustToContents()\` after setup
- Implement proper event handlers

## Controls
- Set meaningful tooltips
- Use \`NumericControl\` for numeric input
- Group related controls with \`GroupBox\`

## Process Interfaces
- Implement all required virtual methods
- Support drag-and-drop of process icons
- Provide real-time preview when appropriate`
    };

    return practices[topic] || practices.general;
  }
}

export { PCLLexer, PCLAnalyzer, TokenType };
