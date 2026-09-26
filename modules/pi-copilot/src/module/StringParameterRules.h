// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_StringParameterRules_h
#define PICopilot_StringParameterRules_h

#include <pcl/ProcessParameter.h>
#include <pcl/String.h>

namespace pcl
{

/*
 * The character rule a String process parameter declares, and the check
 * apply_process makes against it BEFORE a value reaches the core. The core's
 * SetParameterValue() does NOT enforce the declared characters (measured: it
 * accepts "bad id" and "a-b" for PixelMath.newImageId and reads them back),
 * so this check is the only one there is.
 *
 * Core defect (measured on PixInsight 1.9.x, task-pmid-report.md): the API
 * function GetParameterAllowedCharacters() sets the caller's buffer capacity
 * to 0 on entry, before copying, so the copy always fails for any NON-EMPTY
 * declared set -- ProcessParameter::AllowedCharacters() then throws
 * "GetParameterAllowedCharacters(): API function error" -- while the size
 * query (null buffer) still reports the declared LENGTH correctly. An empty
 * set (no restriction) is read fine. So a throw means "a restriction IS
 * declared and cannot be read", never "no restriction".
 *
 * Resolution order:
 *   1. the declared length, from the core's size query;
 *   2. ProcessParameter::AllowedCharacters() -- used whenever it works
 *      (every unrestricted parameter; every restricted one on a fixed core);
 *   3. else a compiled-in copy of the set, keyed by the parameter path, used
 *      ONLY when its length equals the declared length. The copies are taken
 *      from the PCL SDK sources of the processes (~/PCL/src/modules); six
 *      closed-source view-identifier parameters use the PixInsight identifier
 *      rule (their value can only ever name a view);
 *   4. else unresolved: the value is refused with a message saying why.
 * A set equal to the identifier set [A-Za-z0-9_] is a view identifier: a
 * non-empty value must also be a valid PixInsight identifier (not starting
 * with a digit; measured: PixelMath createNewImage with newImageId "1bad"
 * fails at execution and creates no window).
 */
enum class StringCharacterRuleKind { None, CharacterSet, Identifier };

struct StringCharacterRule
{
   bool                    ok = false;       // false: unresolved; `error` says why (no value may be set)
   StringCharacterRuleKind kind = StringCharacterRuleKind::None;
   String                  allowed;          // CharacterSet / Identifier: the allowed characters
   size_type               declaredLength = 0;   // what the core's size query reports
   String                  source;           // "core", "compiled-in", "" (None)
   String                  error;            // unresolved only; starts with the parameter path
};

// "Process.parameter" or "Process.table.column" (the compiled-in table's key).
String StringParameterPath( const ProcessParameter& p );

// The rule of a String parameter (see above). Root thread. Never throws.
StringCharacterRule ResolveStringCharacterRule( const ProcessParameter& p );

// "" when `s` satisfies `rule`, else the precise, model-facing problem,
// prefixed with `name` (e.g. "PixelMath.newImageId" or
// "ChannelCombination.channels[0].id"). An unresolved rule is always a problem.
String StringCharacterProblem( const StringCharacterRule& rule, const String& s, const String& name );

// Number of compiled-in entries (self-test: every one must match an installed
// parameter whose declared length it equals).
size_type CompiledStringCharacterRuleCount();
String    CompiledStringCharacterRulePath( size_type i );

} // namespace pcl

#endif // PICopilot_StringParameterRules_h
