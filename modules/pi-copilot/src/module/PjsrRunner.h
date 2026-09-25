// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_PjsrRunner_h
#define PICopilot_PjsrRunner_h

#include <pcl/String.h>

#include <string>

namespace pcl
{

constexpr size_type PICopilotMaxScriptChars        = 20000;
constexpr size_type PICopilotMaxScriptConsoleChars = 8000;   // the TAIL is kept
constexpr size_type PICopilotMaxScriptValueChars   = 4000;
constexpr size_type PICopilotMaxScriptErrorChars   = 2000;   // thrown value / console failure text
constexpr size_type PICopilotMaxScriptPurposeChars = 300;    // the one sentence shown in the dialog

// The code as an ASCII-only JSON string literal: ", \ and every control
// character escaped, and every non-ASCII character (U+2028/2029 included)
// \u-escaped (a non-BMP character as its surrogate pair; an unpaired
// surrogate, which is not text, as U+FFFD). The wrapper scripts below embed
// the model's code ONLY as this literal -- as DATA passed to new Function --
// so no text in it can close the wrapper and run (the breakout an inline
// "(function(){ <code> })" wrapper would allow). Every String encodes (U8()
// always yields strict UTF-8), so the only possible exception is an
// allocation failure (std::bad_alloc).
std::string ScriptLiteral( const String& code );

// The code with every CR LF pair turned into LF (what the dialog shows and
// what runs are then the same line structure).
String NormalizeScriptNewlines( const String& code );

// Trojan-Source guard, applied to the NORMALISED code before the syntax check
// and the dialog. Empty when the code is acceptable; otherwise a precise,
// model-correctable refusal naming the first offending character (U+XXXX,
// line, column). Refused: every C0 control except TAB and LF (so NUL and a
// lone CR), DEL, the C1 controls, U+2028/U+2029 (JavaScript line
// terminators), unpaired surrogates, and every Unicode format character
// (category Cf: zero-width characters, bidi embeddings/overrides/isolates,
// U+FEFF, soft hyphen, tag characters, ...) -- characters that are invisible
// or reorder text, so the script the user reads could differ from the one
// that runs.
String ScriptCharProblem( const String& code );

// How many lines the engine's synthesized `new Function` source puts before
// the body ("function anonymous(targetViewId\n) {\n": 2 on the proven
// platform). Measured once per session from a runtime error thrown on a known
// body line and cached; -1 while it cannot be measured (then every line
// reported is 0 = unknown, never a guess).
int PjsrLineOffset();

struct PjsrCheck
{
   bool   ok = false;
   String error;      // "SyntaxError: <message>"
   int    line = 0;   // always 0 (unknown): under EvaluateScript a new Function()
                      // SyntaxError carries no position at all (inc-5 Task 1)
};

// Parses the code as the body of function( targetViewId ) with the core's own
// parser (new Function), WITHOUT running it. Root thread only. Never throws.
PjsrCheck CheckPjsrSyntax( const String& code );

struct PjsrRun
{
   bool   ok = false;
   String error;               // the thrown value, e.g. "Error: ..." / "TypeError: ..." (at most PICopilotMaxScriptErrorChars)
   bool   errorTruncated = false;
   int    line = 0;            // 1-based line in the model's code (0 = unknown)
   String value;               // JSON.stringify( returned value ) (else String( value )); empty if undefined
   bool   valueTruncated = false;
   String console;             // what was written to the console while it ran (tail)
   bool   consoleTruncated = false;
   double elapsedMs = 0;
};

// Runs the code as the body of function( targetViewId ) via
// MetaModule::EvaluateScript, capturing the console with beginLog()/endLog()
// in the same evaluation (endLog in a finally, so the pair stays balanced
// when the script throws). The console bytes cross back as base64 and the
// result as ASCII-only JSON, so no text is re-encoded on the way. Root thread
// only. Cannot be interrupted: an endless loop hangs PixInsight (the approval
// dialog says so). Never throws.
PjsrRun RunPjsr( const String& code, const IsoString& targetViewId );

} // namespace pcl

#endif // PICopilot_PjsrRunner_h
