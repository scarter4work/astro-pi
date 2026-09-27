// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef PICopilot_TextSafety_h
#define PICopilot_TextSafety_h

#include <pcl/String.h>

namespace pcl
{

// Unicode general category Cf (format characters), Unicode 15.1.
bool IsFormatChar( uint32 c );

// Why code point c must not be shown raw in a message (the chat log, a tool
// result): it is a control character (tab and line feed included), a
// bidirectional text control, an unpaired surrogate, an invisible format
// character, a line/paragraph separator, a non-ASCII space, or an invisible
// filler / variation selector / combining character. nullptr when it is an
// ordinary visible character. The same classes PjsrRunner refuses in script
// text (RefusedKind), for text that is only DISPLAYED.
const char* UnsafeDisplayKind( uint32 c );

// The Unicode name of a few common unsafe characters ("RIGHT-TO-LEFT
// OVERRIDE"), else nullptr.
const char* UnsafeDisplayName( uint32 c );

// The code point at s[i] (a valid surrogate pair is combined; `units` is set
// to 1 or 2).
uint32 CodePointAt( const String& s, size_type i, size_type& units );

// One character for a message: an ordinary one quoted ('-'), an unsafe one
// spelled out and never raw: U+202E RIGHT-TO-LEFT OVERRIDE (a bidirectional
// text control).
String DisplayCharacter( uint32 c );

// A model- or user-supplied text for a message, quoted: ordinary characters
// kept, every unsafe one replaced by <U+XXXX>, and anything beyond maxChars
// characters cut with "… (N characters)".
String DisplayText( const String& s, size_type maxChars = 80 );

} // namespace pcl

#endif // PICopilot_TextSafety_h
