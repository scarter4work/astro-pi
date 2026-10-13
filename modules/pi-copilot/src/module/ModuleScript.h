// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_ModuleScript_h
#define PICopilot_ModuleScript_h

#include <pcl/String.h>

#include <nlohmann/json.hpp>

#include <string>

namespace pcl
{

/*
 * The module's OWN fixed JavaScript (CPD rule 5.4, last sentence: an
 * interpreter a product uses internally to run its own code, distributed
 * under the program). PCL has no C++ API for a few core facts -- a view's
 * processing history, workspace process icons, script signatures -- so the
 * module reads them with short scripts whose TEXT is fixed in this signed
 * binary. Nothing a model, a user or a file supplies is ever executed:
 *
 *  - every wrapper below is a compile-time string of this module;
 *  - variable data (a view id, a file path, a JSON blob) enters ONLY as a
 *    ScriptLiteral(): an ASCII-only JSON string literal, i.e. a DATA value
 *    in an expression position. No text inside it can close the literal or
 *    the wrapper (self-tested against quote, backslash, comment, </script>,
 *    NUL/control, U+2028/2029, surrogate and string-escape breakouts);
 *  - there is no entry point that takes source code: PI Copilot 0.2.2.0
 *    removed run_pjsr and everything that evaluated model-written code.
 */

// The text as an ASCII-only JSON string literal: ", \ and every control
// character escaped, and every non-ASCII character (U+2028/2029 included)
// \u-escaped (a non-BMP character as its surrogate pair; an unpaired
// surrogate, which is not text, as U+FFFD). Never throws on text from a
// String, except std::bad_alloc.
std::string ScriptLiteral( const String& text );

// Runs one of the module's own fixed wrappers and parses its JSON result.
// `body` is ASCII-only JavaScript statements -- fixed module text plus
// ScriptLiteral() data -- that end in `return pcAscii( JSON.stringify( <value> ) );`.
// It runs inside a function with the helpers pcWell/pcAscii/pcCut in scope,
// while an EvalDepthGuard is held (EvalGuard.h). Everything crosses the
// boundary as ASCII, so no text is re-encoded by the engine's narrow-string
// conversion (inc-5 Task 1, finding 1). Wrap every user-data string in
// pcWell() so an unpaired surrogate cannot produce an escape a strict JSON
// parser rejects. Root thread only. Throws (a pcl::Error or a
// nlohmann::json exception) when the result is not JSON.
nlohmann::json EvaluateAsciiJson( const std::string& body );

} // namespace pcl

#endif // PICopilot_ModuleScript_h
