// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ProcessApply.h"
#include "GlobalRunFiles.h"   // NulTextProblem, DuplicateKeyProblem
#include "HistoryReader.h"
#include "ProcessCatalog.h"
#include "ProcessSafety.h"
#include "StringParameterRules.h"
#include "Utf8.h"

#include <pcl/AutoViewLock.h>
#include <pcl/Exception.h>
#include <pcl/ImageWindow.h>
#include <pcl/Process.h>
#include <pcl/ProcessInstance.h>
#include <pcl/ProcessParameter.h>
#include <pcl/StringList.h>
#include <pcl/Variant.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <set>
#include <string>

namespace pcl
{

namespace
{

// Row index for a scalar (non-table) parameter. NOT the header default
// ~size_type(0): ProcessInstance passes rowIndex straight to the core, which
// rejects that sentinel with a MODAL "Invalid table row index" dialog
// (Task 1). Scalars are row 0.
constexpr size_type kScalarRow = 0;

InstanceBuildObserver g_instanceObserver;   // self-test only
bool g_inProcessAppliesExpected = false;     // self-test only (SetInProcessAppliesExpectedForSelfTest)
int  g_inProcessUnrecorded = 0;              // self-test only
std::function<void()> g_beforeExecuteHook;  // self-test only (SetBeforeExecuteHookForSelfTest)

void NoteInstanceBuild( const IsoString& processId, const char* stage )
{
   if ( g_instanceObserver )
      g_instanceObserver( processId, stage );
}

// A model-correctable failure. Thrown only inside ApplyProcess() / RunGlobalProcess()
// (and the helpers they call) and caught there.
struct ApplyError
{
   String message;
};

// Length-aware (Utf8.h): an embedded NUL is kept (and then refused by
// ToVariant), never a silent end of the text.
String S16( const std::string& utf8 )
{
   return FromU8( utf8 );
}

String JsonShort( const nlohmann::json& v )
{
   std::string s = v.dump();
   if ( s.size() > 80 )
      s = s.substr( 0, 77 ) + "...";
   return S16( s );
}

String JoinIds( const ProcessParameter::enumeration_element_list& elements )
{
   String s;
   for ( const ProcessParameter::EnumerationElement& e : elements )
   {
      if ( !s.IsEmpty() )
         s += ", ";
      s += String( e.id );
   }
   return s;
}

String ColumnIds( const ProcessParameter::parameter_list& columns )
{
   String s;
   for ( const ProcessParameter& c : columns )
   {
      if ( !s.IsEmpty() )
         s += ", ";
      s += String( c.Id() );
   }
   return s;
}

String RowCount( size_type n )
{
   return String().Format( "%u row", unsigned( n ) ) + (n == 1 ? "" : "s");
}

String VariantText( const Variant& v )
{
   try
   {
      return v.IsValid() ? v.ToString() : String( "(invalid)" );
   }
   catch ( ... )
   {
      return "(unprintable)";
   }
}

Variant EnumVariant( const ProcessParameter& p, const nlohmann::json& v, const String& name )
{
   // Same resolution as describe_process (native ids, else PJSR introspection).
   const ProcessParameter::enumeration_element_list* elements = nullptr;
   try
   {
      elements = &EnumerationInfoOf( p ).elements;
   }
   catch ( const pcl::Exception& x )
   {
      throw ApplyError{ x.Message() };   // already names the parameter and both failures
   }

   if ( v.is_string() )
   {
      const std::string& want = v.get_ref<const std::string&>();
      for ( const ProcessParameter::EnumerationElement& e : *elements )
      {
         if ( want == e.id.c_str() )
            return Variant( e.value );
         for ( const IsoString& a : e.aliases )
            if ( want == a.Trimmed().c_str() )
               return Variant( e.value );
      }
      throw ApplyError{ name + ": '" + S16( v.get<std::string>() ) + "' is not a valid value; use one of: "
                        + JoinIds( *elements ) };
   }
   if ( v.is_number_integer() )
   {
      for ( const ProcessParameter::EnumerationElement& e : *elements )
         if ( e.value == v.get<int64_t>() )
            return Variant( e.value );
      throw ApplyError{ name + ": " + JsonShort( v ) + " is not a valid element value; use one of: " + JoinIds( *elements ) };
   }
   throw ApplyError{ name + ": expected one of " + JoinIds( *elements ) + " (as a string), got " + JsonShort( v ) };
}

// The storage range of an integer/real parameter type. The core's declared
// range (GetNumericRange) can be wider or absent; a value outside what the
// type can hold must never reach SetParameterValue().
void TypeRange( const ProcessParameter& p, double& lo, double& hi )
{
   switch ( p.Type() )
   {
   case ProcessParameterType::UInt8:  lo = 0;          hi = 255;        break;
   case ProcessParameterType::Int8:   lo = -128;       hi = 127;        break;
   case ProcessParameterType::UInt16: lo = 0;          hi = 65535;      break;
   case ProcessParameterType::Int16:  lo = -32768;     hi = 32767;      break;
   case ProcessParameterType::UInt32: lo = 0;          hi = 4294967295.0; break;
   case ProcessParameterType::Int32:  lo = -2147483648.0; hi = 2147483647.0; break;
   case ProcessParameterType::UInt64: lo = 0;          hi = 9007199254740992.0; break;   // exact in a double
   case ProcessParameterType::Int64:  lo = -9007199254740992.0; hi = 9007199254740992.0; break;
   case ProcessParameterType::Float:  lo = -double( FLT_MAX ); hi = double( FLT_MAX ); break;
   default:                           lo = -DBL_MAX;    hi = DBL_MAX;    break;
   }
}

Variant ToVariant( const ProcessParameter& p, const nlohmann::json& v, const String& name )
{
   // JSON allows U+0000; the core's C strings would cut the text there.
   if ( v.is_string() )
   {
      const String t = S16( v.get<std::string>() );
      for ( size_type i = 0; i < t.Length(); ++i )
         if ( t[i] == 0 )
            throw ApplyError{ name + String().Format( ": the text contains a NUL character (U+0000) at position %u; "
                                                      "remove it", unsigned( i ) ) };
   }
   if ( p.IsBlock() )
      throw ApplyError{ name + ": block (binary) parameters cannot be set by apply_process" };
   if ( p.IsBoolean() )
   {
      if ( !v.is_boolean() )
         throw ApplyError{ name + ": expected true or false, got " + JsonShort( v ) };
      return Variant( v.get<bool>() );
   }
   if ( p.IsEnumeration() )
      return EnumVariant( p, v, name );
   if ( p.IsString() )
   {
      if ( !v.is_string() )
         throw ApplyError{ name + ": expected a string, got " + JsonShort( v ) };
      const String s = S16( v.get<std::string>() );
      size_type minLen = 0, maxLen = 0;
      p.GetLengthLimits( minLen, maxLen );
      if ( maxLen > 0 && s.Length() > maxLen )
         throw ApplyError{ name + String().Format( ": text is %u characters; the maximum is %u",
                                                   unsigned( s.Length() ), unsigned( maxLen ) ) };
      if ( s.Length() < minLen )
         throw ApplyError{ name + String().Format( ": text is %u characters; the minimum is %u",
                                                   unsigned( s.Length() ), unsigned( minLen ) ) };
      // Declared characters (the core does not enforce them on set). Not
      // p.AllowedCharacters() directly: the core cannot copy a non-empty
      // declared set and it throws (StringParameterRules.h).
      const String problem = StringCharacterProblem( ResolveStringCharacterRule( p ), s, name );
      if ( !problem.IsEmpty() )
         throw ApplyError{ problem };
      return Variant( s );
   }
   if ( p.IsNumeric() )
   {
      if ( !v.is_number() )
         throw ApplyError{ name + ": expected a number, got " + JsonShort( v ) };
      const double d = v.get<double>();
      if ( p.IsInteger() && d != std::floor( d ) )
         throw ApplyError{ name + ": expected an integer, got " + JsonShort( v ) };
      double lo = 0, hi = 0;
      p.GetNumericRange( lo, hi );
      if ( lo < hi && (d < lo || d > hi) )
      {
         // The core reports an unbounded side as +/-DBL_MAX; don't print it.
         const bool hasLo = lo > -DBL_MAX, hasHi = hi < DBL_MAX;
         if ( hasLo && hasHi )
            throw ApplyError{ name + String().Format( ": %.10g is out of range [%.10g, %.10g]", d, lo, hi ) };
         if ( d < lo )
            throw ApplyError{ name + String().Format( ": %.10g is below the minimum %.10g", d, lo ) };
         throw ApplyError{ name + String().Format( ": %.10g is above the maximum %.10g", d, hi ) };
      }
      double tlo = 0, thi = 0;
      TypeRange( p, tlo, thi );
      if ( d < tlo || d > thi )
         throw ApplyError{ name + String().Format( ": %.10g does not fit the parameter type %s [%.10g, %.10g]",
                                                   d, p.IsInteger() ? "integer" : "real", tlo, thi ) };
      if ( p.IsInteger() )
         return Variant( int64( d ) );
      return Variant( d );
   }
   throw ApplyError{ name + ": unsupported parameter type" };
}

bool SameValue( const ProcessParameter& p, const Variant& a, const Variant& b )
{
   if ( !b.IsValid() )
      return false;
   if ( p.IsString() )
      return a.ToString() == b.ToString();
   if ( p.IsBoolean() )
      return a.ToBoolean() == b.ToBoolean();
   if ( p.IsEnumeration() || p.IsInteger() )
      return a.ToInt64() == b.ToInt64();
   const double x = a.ToDouble(), y = b.ToDouble();
   return std::fabs( x - y ) <= 1e-6*std::max( 1.0, std::fabs( x ) );   // Float params store 32 bits
}

// SetParameterValue + mandatory read-back: a value that does not stick is an
// error, never a silent success (this also catches any enum value/index
// mismatch in the core API).
void SetChecked( ProcessInstance& instance, const ProcessParameter& p, const Variant& value,
                 size_type row, const String& name )
{
   bool set = false;
   try
   {
      set = instance.SetParameterValue( value, p, row );
   }
   catch ( const pcl::Exception& x )
   {
      throw ApplyError{ name + ": " + x.Message() };
   }
   if ( !set )
      throw ApplyError{ name + ": PixInsight rejected the value " + VariantText( value ) };
   const Variant back = instance.ParameterValue( p, row );
   if ( !SameValue( p, value, back ) )
      throw ApplyError{ name + ": the value did not stick (set " + VariantText( value )
                        + ", read back " + VariantText( back ) + ")" };
}

ProcessParameter FindParameter( const Process& P, const std::string& id, const String& name )
{
   try
   {
      return ProcessParameter( P, IsoString( id.c_str() ) );
   }
   catch ( const pcl::Exception& )
   {
      throw ApplyError{ "unknown parameter " + name + "; call describe_process for the valid parameter ids" };
   }
}

// Non-blocking busy probe (global constraint): LockForWrite()/ExecuteOn() on
// a view a running process holds HANGS PixInsight instead of failing fast.
bool ViewBusy( View& view )
{
   try
   {
      return !view.CanRead() || !view.CanWrite();
   }
   catch ( ... )
   {
      return true;
   }
}

String BusyMessage( const String& viewId )
{
   return "view " + viewId + " is busy (locked by a running process); try again when it finishes";
}

// Sets `parameters` (scalars) and `tableParameters` (whole tables) on a
// DEFAULT instance, checking everything the core would reject with a modal
// and reading every value back. Throws ApplyError with a precise message.
// Shared by ApplyProcess and RunGlobalProcess.
//
// instance == nullptr: METADATA ONLY, for a process no instance of which may
// be built before the user approved (NoInstanceBeforeApproval): keys, types,
// ranges, text limits, parameterValues, pinned keys and table shapes are
// checked from Process/ProcessParameter metadata alone. Enumeration values
// are NOT converted (the PJSR route of EnumerationInfoOf() builds an
// instance in script), and nothing is set or read back: those checks run
// after approval, on the real instance.
void SetParameters( const Process& P, ProcessInstance* instance, const String& processId,
                    const nlohmann::json& parameters, const nlohmann::json& tableParameters,
                    nlohmann::json& parametersSet, nlohmann::json& pinnedSet,
                    const std::vector<PinnedParameter>* resolvedPinned )
{
   if ( !parameters.is_null() && !parameters.is_object() )
      throw ApplyError{ String( "parameters must be an object {parameterId: value}" ) };
   if ( !tableParameters.is_null() && !tableParameters.is_object() )
      throw ApplyError{ String( "table_parameters must be an object {tableId: [[row values]...]}" ) };
   // Keys resolve through the core (an alias is its parameter): canonical +
   // alias together would be "last one wins", so they are refused.
   {
      String dup = DuplicateKeyProblem( P, parameters, "parameter" );
      if ( dup.IsEmpty() )
         dup = DuplicateKeyProblem( P, tableParameters, "table" );
      if ( !dup.IsEmpty() )
         throw ApplyError{ dup };
   }
   // Pinned parameters (ProcessSafety.h): refused from the model. Their
   // values were resolved ONCE by the caller (the tools, before any dialog:
   // what the user was shown is what runs) and are only checked for
   // completeness here; a direct caller without them resolves now.
   std::vector<PinnedParameter> pinned;
   if ( resolvedPinned != nullptr )
   {
      const String e = CheckResolvedPinnedParameters( P.Id(), parameters, tableParameters, *resolvedPinned );
      if ( !e.IsEmpty() )
         throw ApplyError{ e };
      pinned = *resolvedPinned;
   }
   else
   {
      const String e = ResolvePinnedParameters( P.Id(), parameters, tableParameters, pinned );
      if ( !e.IsEmpty() )
         throw ApplyError{ e };
   }
   if ( parameters.is_object() )
      for ( auto it = parameters.begin(); it != parameters.end(); ++it )
      {
         const String name = processId + "." + S16( it.key() );
         const ProcessParameter p = FindParameter( P, it.key(), name );
         if ( p.IsTable() )
            throw ApplyError{ name + " is a table parameter; pass it in table_parameters as [[row values]...]" };
         if ( p.IsReadOnly() )
            throw ApplyError{ name + " is read-only" };
         if ( instance == nullptr && p.IsEnumeration() )
            continue;   // checked after approval (see above)
         const Variant value = ToVariant( p, it.value(), name );
         // Policy parameterValues (e.g. values that reach a command line).
         if ( p.IsString() )
         {
            const String e = ParameterValueProblem( P.Id(), p.Id(), value.ToString() );
            if ( !e.IsEmpty() )
               throw ApplyError{ e };
         }
         if ( instance == nullptr )
            continue;
         SetChecked( *instance, p, value, kScalarRow, name );
         parametersSet[it.key()] = it.value();
      }

   if ( tableParameters.is_object() )
      for ( auto it = tableParameters.begin(); it != tableParameters.end(); ++it )
      {
         const String name = processId + "." + S16( it.key() );
         const ProcessParameter p = FindParameter( P, it.key(), name );
         if ( !p.IsTable() )
            throw ApplyError{ name + " is not a table parameter; pass it in parameters" };
         // Output tables (e.g. PixelMath.outputData) are written by the
         // process, never by us: refuse before AllocateTableRows().
         if ( p.IsReadOnly() )
            throw ApplyError{ name + " is read-only (an output of the process); it cannot be set" };
         const ProcessParameter::parameter_list columns = p.TableColumns();
         for ( const ProcessParameter& c : columns )
            if ( c.IsReadOnly() )
               throw ApplyError{ name + "." + String( c.Id() )
                                 + " is a read-only column (an output of the process); this table cannot be set" };
         const nlohmann::json& rows = it.value();
         if ( !rows.is_array() )
            throw ApplyError{ name + ": expected an array of rows [[...], ...]" };
         for ( size_type i = 0; i < rows.size(); ++i )
            if ( !rows[i].is_array() || rows[i].size() != columns.Length() )
               throw ApplyError{ name + String().Format( ": row %u has %u values; expected %u (columns: ",
                                                         unsigned( i ), unsigned( rows[i].is_array() ? rows[i].size() : 0 ),
                                                         unsigned( columns.Length() ) )
                                 + ColumnIds( columns ) + ")" };
         // Row count BEFORE AllocateTableRows(): the core answers a length
         // outside the table's limits with a MODAL "Invalid parameter
         // allocation length" dialog (seen live for HistogramTransformation.H,
         // which takes 4-5 rows), not with a catchable failure.
         //
         // A max length of 0 means UNLIMITED (MetaParameter::MaxLength(),
         // the default; the core reports 0 for e.g. CurvesTransformation.K
         // and min 1 / max 0 for MorphologicalTransformation.structureWayTable).
         // ~0 is also treated as unlimited (ProcessParameter.h documents it).
         size_type minRows = 0, maxRows = 0;
         p.GetLengthLimits( minRows, maxRows );
         const bool unlimited = maxRows == 0 || maxRows == ~size_type( 0 );
         if ( rows.size() < minRows || (!unlimited && rows.size() > maxRows) )
         {
            String need;
            if ( unlimited )
               need = "at least " + RowCount( minRows );
            else if ( minRows == maxRows )
               need = "exactly " + RowCount( minRows );
            else
               need = String().Format( "between %u and %u rows", unsigned( minRows ), unsigned( maxRows ) );
            throw ApplyError{ name + ": " + RowCount( rows.size() ) + " given; this table needs "
                              + need + " (columns: " + ColumnIds( columns ) + ")" };
         }
         if ( instance == nullptr )
         {
            for ( size_type i = 0; i < rows.size(); ++i )
               for ( size_type k = 0; k < columns.Length(); ++k )
                  if ( !columns[k].IsEnumeration() )
                     ToVariant( columns[k], rows[i][k],
                                name + String().Format( "[%u].", unsigned( i ) ) + String( columns[k].Id() ) );
            continue;
         }
         if ( !instance->AllocateTableRows( p, rows.size() ) )
            throw ApplyError{ name + String().Format( ": PixInsight refused a table of %u rows", unsigned( rows.size() ) ) };
         for ( size_type i = 0; i < rows.size(); ++i )
            for ( size_type k = 0; k < columns.Length(); ++k )
            {
               const String cell = name + String().Format( "[%u].", unsigned( i ) ) + String( columns[k].Id() );
               SetChecked( *instance, columns[k], ToVariant( columns[k], rows[i][k], cell ), i, cell );
            }
         parametersSet[it.key()] = rows;
      }

   // Last, so nothing the model passed can overwrite them; read back like
   // every other value (a value that does not stick is an error).
   if ( instance == nullptr )
      return;
   for ( const PinnedParameter& q : pinned )
   {
      const String name = processId + "." + S16( q.parameter );
      const ProcessParameter p = FindParameter( P, q.parameter, name );
      SetChecked( *instance, p, Variant( q.value ), kScalarRow, name );
      pinnedSet[q.parameter] = U8( q.value );
   }
}

// DETECT's second signal (step 7), for a completed history-updating run on
// a main view whose ModifyCount did not advance: is there a NEW active step of
// this process right after the pre-run position? Recorded: the active count
// grew by exactly one and that step is `processId`. NotRecorded: it did not
// (the hazard: PixInsight was still inside another process execution).
// Unknown: a read was busy (EvalGuard) or failed -- never guessed either way.
struct StepCheck
{
   enum Verdict { Recorded, NotRecorded, Unknown } verdict = Unknown;
   String reason;
};

StepCheck CheckNewHistoryStep( const View& view, const HistorySnapshot& before, const IsoString& processId )
{
   StepCheck c;
   if ( !before.ok )
   {
      c.reason = before.busy ? String( "another script evaluation was running before the run" )
                             : "reading the History before the run failed: " + before.error;
      return c;
   }
   const HistorySnapshot after = ReadViewHistory( view.FullId(), before.ActiveCount() );
   if ( !after.ok )
   {
      c.reason = after.busy ? String( "another script evaluation was running" )
                            : "reading the History failed: " + after.error;
      return c;
   }
   const bool grew = after.ActiveCount() == before.ActiveCount() + 1;
   const bool ours = !after.steps.empty() && after.steps.front().combinedIndex == before.ActiveCount()
                  && after.steps.front().processId == std::string( processId.c_str() );
   c.verdict = grew && ours ? StepCheck::Recorded : StepCheck::NotRecorded;
   return c;
}

// The same for a PREVIEW (measured, T-hist round 2): a preview's History
// holds ONE step -- each new step REPLACES the previous one (length stays 1,
// historyIndex 1) and is computed from the MAIN image's pixels, not from the
// earlier preview step (0.6 then $T*0.5 gives 0.4); Undo returns the preview
// to the main image's pixels; the main image (pixels inside and outside the
// preview rectangle), its ModifyCount and its History never change. In the
// hazard window nothing is recorded (fresh preview stays at 0 steps, a prior
// step stays in place) though the preview pixels change. Recorded: the
// preview's active step is `processId` and is not any step read before the
// run (a step's identity includes its start time).
StepCheck CheckNewPreviewStep( const View& view, const HistorySnapshot& before, const IsoString& processId )
{
   StepCheck c;
   if ( !before.ok )
   {
      c.reason = before.busy ? String( "another script evaluation was running before the run" )
                             : "reading the History before the run failed: " + before.error;
      return c;
   }
   const HistorySnapshot after = ReadViewHistory( view.FullId(), 0 );
   if ( !after.ok )
   {
      c.reason = after.busy ? String( "another script evaluation was running" )
                            : "reading the History failed: " + after.error;
      return c;
   }
   const int active = after.ActiveCount();
   bool recorded = active >= 1 && size_t( active ) <= after.steps.size()
                && after.steps[active - 1].processId == std::string( processId.c_str() );
   if ( recorded )
      for ( const HistoryStep& b : before.steps )
         if ( b.identity == after.steps[active - 1].identity )
            recorded = false;
   c.verdict = recorded ? StepCheck::Recorded : StepCheck::NotRecorded;
   return c;
}

// ImageContentDigest's mixer: four independent 64-bit lanes over 8-byte words
// (one multiply each, so the read pass stays close to memory bandwidth), then
// a final avalanche. Not cryptographic -- it only has to tell "identical" from
// "changed".
class ContentHasher
{
public:
   void Word( uint64 w )
   {
      uint64& h = m_lane[m_next];
      m_next = (m_next + 1) & 3;
      h ^= w;
      h *= 0x9E3779B97F4A7C15ull;
      h ^= h >> 29;
   }

   void Bytes( const void* data, size_t n )
   {
      const uint8* p = static_cast<const uint8*>( data );
      Word( uint64( n ) );
      size_t i = 0;
      // Four words per step, one per lane (the loop the compiler can keep in registers).
      for ( ; i + 32 <= n; i += 32 )
      {
         uint64 w[4];
         std::memcpy( w, p + i, 32 );
         for ( int k = 0; k < 4; ++k )
         {
            uint64& h = m_lane[k];
            h ^= w[k];
            h *= 0x9E3779B97F4A7C15ull;
            h ^= h >> 29;
         }
      }
      for ( ; i + 8 <= n; i += 8 )
      {
         uint64 w;
         std::memcpy( &w, p + i, 8 );
         Word( w );
      }
      if ( i < n )
      {
         uint64 w = 0;
         std::memcpy( &w, p + i, n - i );
         Word( w ^ 0xA5ull );
      }
   }

   uint64 Final() const
   {
      uint64 h = 0x243F6A8885A308D3ull;
      for ( uint64 l : m_lane )
      {
         h ^= l + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
         h ^= h >> 33;
         h *= 0xFF51AFD7ED558CCDull;
         h ^= h >> 33;
      }
      return h;
   }

private:
   uint64 m_lane[4] = { 0x6A09E667F3BCC908ull, 0xBB67AE8584CAA73Bull, 0x3C6EF372FE94F82Bull, 0xA54FF53A5F1D36F1ull };
   int    m_next = 0;
};

// Row by row over `r` (the whole image or a region), so an image and the same
// pixels as a region of a larger image hash identically (a preview's baseline).
template <class P>
void HashChannels( const GenericImage<P>& img, const Rect& r, ContentHasher& h )
{
   const size_t rowBytes = size_t( r.Width() )*sizeof( typename P::sample );
   for ( int c = 0; c < img.NumberOfChannels(); ++c )   // nominal + alpha
   {
      h.Word( uint64( c ) );
      for ( int y = r.y0; y < r.y1; ++y )
         h.Bytes( img.PixelAddress( r.x0, y, c ), rowBytes );
   }
}

// The target's content digest. Callers probe ViewBusy() first: a write lock
// on a view a running process holds hangs PixInsight (ViewCapture.cpp).
uint64 ViewContentDigest( View& view )
{
   AutoViewWriteLock lock( view );
   return ImageContentDigest( view.Image() );
}

// What a PREVIEW starts from when a process runs on it: the MAIN image's
// pixels in the preview rectangle (measured, T-hist round 2 and T-graxpert
// fix round 1: a no-op GraXpert run on a preview whose step was PixelMath
// $T*0.5 leaves the preview showing the main image's pixels, mean
// 0.0875 -> 0.175). Comparing against the preview's CURRENT pixels would call
// that revert a change. `busy` when the main view is locked.
uint64 PreviewBaselineDigest( View& preview, bool& busy )
{
   ImageWindow w = preview.Window();
   View main = w.MainView();
   busy = ViewBusy( main );
   if ( busy )
      return 0;
   const Rect r = w.PreviewRect( preview.Id() );
   AutoViewWriteLock lock( main );
   return ImageContentDigest( main.Image(), r );
}

} // namespace

// ---- External-program bridges (ProcessApply.h) -----------------------------------

const std::vector<ExternalProgramBridge>& ExternalProgramBridges()
{
   static const std::vector<ExternalProgramBridge> table = {
      { "GraXpert", "replaceImage", "GraXpert",
        "launches the external GraXpert program (appPath, pinned in the policy) with the image in a fixed shared "
        "temp file (/tmp/PixInsight.xisf -> /tmp/PixInsight_GraXpert.xisf) and reports success even when the "
        "program failed, wrote nothing, or another PixInsight instance used the same files (measured "
        "2026-09-26)" },
   };
   return table;
}

const ExternalProgramBridge* FindExternalProgramBridge( const IsoString& canonicalProcessId )
{
   for ( const ExternalProgramBridge& b : ExternalProgramBridges() )
      if ( canonicalProcessId == b.processId )
         return &b;
   return nullptr;
}

uint64 ImageContentDigest( const ImageVariant& image )
{
   // Not a defaulted Rect() argument: PCL's default Rect is UNINITIALIZED
   // (Rectangle.h), which fix round 1 measured as a wrong digest.
   return image ? ImageContentDigest( image, image.Bounds() ) : ContentHasher().Final();
}

uint64 ImageContentDigest( const ImageVariant& image, const Rect& region )
{
   ContentHasher h;
   if ( !image )
      return h.Final();
   const Rect r = region.Ordered().Intersection( image.Bounds() );
   h.Word( uint64( r.Width() ) );
   h.Word( uint64( r.Height() ) );
   h.Word( uint64( image.NumberOfChannels() ) );
   h.Word( uint64( image.BitsPerSample() ) | (image.IsFloatSample() ? 0x100u : 0u)
           | (image.IsComplexSample() ? 0x200u : 0u) | (uint64( image.ColorSpace() ) << 16) );
   if ( image.IsComplexSample() )
   {
      if ( image.BitsPerSample() == 64 )
         HashChannels( static_cast<const ComplexImage&>( *image ), r, h );
      else
         HashChannels( static_cast<const DComplexImage&>( *image ), r, h );
   }
   else if ( image.IsFloatSample() )
   {
      if ( image.BitsPerSample() == 32 )
         HashChannels( static_cast<const Image&>( *image ), r, h );
      else
         HashChannels( static_cast<const DImage&>( *image ), r, h );
   }
   else
      switch ( image.BitsPerSample() )
      {
      case 8:  HashChannels( static_cast<const UInt8Image&>( *image ), r, h );  break;
      case 16: HashChannels( static_cast<const UInt16Image&>( *image ), r, h ); break;
      default: HashChannels( static_cast<const UInt32Image&>( *image ), r, h ); break;
      }
   return h.Final();
}

// Declared in ProcessApply.h (shared with the journey tools, pre-flight P22).
std::set<std::string> OpenMainViewIds()
{
   std::set<std::string> ids;
   for ( const ImageWindow& w : ImageWindow::AllWindows() )
      ids.insert( std::string( w.MainView().Id().c_str() ) );
   return ids;
}

ApplyProcessResult ApplyProcess( const IsoString& processId, const nlohmann::json& parameters,
                                 const nlohmann::json& tableParameters, View view,
                                 const std::vector<PinnedParameter>* pinned )
{
   ApplyProcessResult r;
   try
   {
      std::unique_ptr<Process> P;
      try
      {
         P.reset( new Process( processId ) );
      }
      catch ( const pcl::Exception& )
      {
         throw ApplyError{ "unknown process id '" + String( processId ) + "'; call list_processes for valid ids" };
      }
      r.processId = String( P->Id() );

      if ( !P->CanProcessViews() )
         throw ApplyError{ r.processId + " can only run in the global context (not on a view); use run_global_process" };

      if ( view.IsNull() )
         throw ApplyError{ String( "no target view: open or select an image, or pass view_id" ) };
      r.viewId = String( view.FullId() );

      // Never block on a view a running process holds (see ViewCapture.cpp):
      // fail fast here, and probe again right before execution.
      if ( ViewBusy( view ) )
         throw ApplyError{ BusyMessage( r.viewId ) };

      NoteInstanceBuild( P->Id(), "apply" );
      ProcessInstance instance( *P );   // DEFAULT parameters

      SetParameters( *P, &instance, r.processId, parameters, tableParameters, r.parametersSet, r.pinnedSet, pinned );

      String whyNot;
      if ( !instance.Validate( whyNot ) )
         throw ApplyError{ r.processId + " rejected the parameters: "
                           + (whyNot.IsEmpty() ? String( "(no reason given)" ) : whyNot) };
      if ( !instance.CanExecuteOn( view, whyNot ) )
         throw ApplyError{ r.processId + " cannot run on " + r.viewId + ": "
                           + (whyNot.IsEmpty() ? String( "(no reason given)" ) : whyNot) };

      if ( ViewBusy( view ) )
         throw ApplyError{ BusyMessage( r.viewId ) };

      // DETECT (step 7): what must change if the step is recorded.
      const bool historyUpdater = instance.IsHistoryUpdater( view );
      r.targetHistoryStep = historyUpdater;
      const bool isPreview = !view.IsMainView();
      ImageWindow window = view.Window();
      const size_type modifyCountBefore = window.ModifyCount();
      // The History before the run: counts only for a main view (for steps
      // that do not advance ModifyCount -- measured: ImageIdentifier,
      // RGBWorkingSpace); the whole (one-step) list for a preview.
      HistorySnapshot historyBefore;
      if ( historyUpdater && !g_inProcessAppliesExpected )
         historyBefore = ReadViewHistory( view.FullId(), isPreview ? 0 : std::numeric_limits<int>::max() );

      // NO-EFFECT (step 8, Task T-graxpert): for a process that bridges to an
      // external program, what must be different after a real run -- the
      // target's pixel content (replace mode) or the set of open windows.
      const ExternalProgramBridge* bridge = FindExternalProgramBridge( P->Id() );
      bool bridgeReplaces = false;
      uint64 digestBefore = 0;      // the target's own pixels before the run
      uint64 previewBaseline = 0;   // a preview: the main image's pixels in its rectangle
      std::set<std::string> windowsBefore;
      if ( bridge != nullptr )
      {
         bridgeReplaces = instance.ParameterValue( ProcessParameter( *P, IsoString( bridge->replaceParameter ) ),
                                                   kScalarRow ).ToBoolean();
         if ( !bridgeReplaces )
            windowsBefore = OpenMainViewIds();
         else
         {
            digestBefore = ViewContentDigest( view );   // not busy: probed just above
            // A preview's no-op result is EITHER of two inputs (measured,
            // fix round 1): when PixInsight records the step it is computed
            // from the main image's pixels (a preview holding PixelMath
            // $T*0.5 reverts to them); in the self-test's in-process context,
            // where nothing is recorded, the preview keeps its own pixels. A
            // real result equals neither.
            if ( isPreview )
            {
               bool mainBusy = false;
               previewBaseline = PreviewBaselineDigest( view, mainBusy );
               if ( mainBusy )
                  throw ApplyError{ BusyMessage( String( window.MainView().FullId() ) ) };
            }
         }
      }

      if ( g_beforeExecuteHook )
         g_beforeExecuteHook();   // self-test only (SetBeforeExecuteHookForSelfTest)

      const auto t0 = std::chrono::steady_clock::now();
      bool ran = false;
      try
      {
         ran = instance.ExecuteOn( view );   // swapFile=true: History + undo
      }
      catch ( const pcl::Exception& x )
      {
         throw ApplyError{ r.processId + " failed while running: " + x.Message() };
      }
      r.elapsedMs = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
      // A failure only the process itself can detect while running (e.g. a
      // PixelMath expression syntax error) comes back as false: no
      // exception, no dialog (verified under Xvfb). The core prints the
      // reason to the Process Console, which a module cannot read back, so
      // name what was set -- that is where the fix is.
      if ( !ran )
      {
         String changes = DescribeParameterChanges( parameters, tableParameters, 400 );
         changes.ReplaceString( "\n", "; " );
         throw ApplyError{ r.processId + " did not complete on " + r.viewId
                           + ": the process stopped with an error while running, or the user aborted it in PixInsight "
                             "(the reason is in the Process Console). Do not simply retry: if the user may have "
                             "aborted it, ask them first; otherwise check the values you set: " + changes };
      }
      // Step 8 BEFORE step 7: a bridged run that did nothing would otherwise
      // pass as a recorded step (its generic History transaction still runs)
      // or be misread as an unrecorded change.
      //
      // LIMITATION (stated in ProcessApply.h and the README): this proves
      // only "changed" vs "identical". When two PixInsight instances race on
      // the core's shared temp file, one outcome is a CHANGED but WRONG
      // result (the other instance's output blended in); a digest cannot
      // tell that from a correct result, and nothing module-side can.
      if ( bridge != nullptr )
      {
         String changes = DescribeParameterChanges( parameters, tableParameters, 400 );
         changes.ReplaceString( "\n", "; " );
         const String program( bridge->program );
         const String why = "PixInsight's " + r.processId + " process hands the image to the external " + program
                          + " program through one temporary file shared by every PixInsight instance on this "
                            "computer, and it reports success even when that program failed or wrote no result. "
                            "The usual cause is another PixInsight instance running " + r.processId + " at the same "
                            "time; otherwise the " + program + " program itself failed (see the Process Console). ";
         const String tell = "Tell the user this plainly; retry only after they confirm no other PixInsight instance is "
                             "running " + r.processId + ". What ran: " + r.processId + " with " + changes;
         if ( bridgeReplaces )
         {
            if ( ViewBusy( view ) )
            {
               r.unverifiedChange = true;   // may have changed: never reported as ok
               throw ApplyError{ r.processId + " reported success, but PI Copilot could NOT check whether it changed "
                                 + r.viewId + " (" + BusyMessage( r.viewId ) + "). Ask the user to look at the image "
                                 "before continuing. What ran: " + r.processId + " with " + changes };
            }
            const uint64 digestAfter = ViewContentDigest( view );
            const bool sameAsOwn = digestAfter == digestBefore;
            const bool sameAsMain = isPreview && digestAfter == previewBaseline;
            if ( sameAsOwn || sameAsMain )
            {
               r.noEffect = true;
               // Whether the core nevertheless added a History step to the target (measured: it usually
               // does; the modification count says). The journey records only a step that exists (m3).
               r.historyStepAdded = !isPreview && window.ModifyCount() > modifyCountBefore;
               // What History holds is stated only as far as it is verified.
               String history;
               if ( !isPreview )
                  // Measured: the core adds a step that changed nothing; the
                  // modification count says whether it did here.
                  history = window.ModifyCount() > modifyCountBefore
                          ? "PixInsight still added a " + r.processId + " step to " + r.viewId + "'s History that "
                            "changed nothing, so there is nothing to undo (Edit > Undo would only remove that empty "
                            "step). "
                          : String( "The image was not changed, so there is nothing to undo. " );
               else
               {
                  // A preview keeps ONE step, which each new step replaces
                  // (T-hist); measured for a no-op GraXpert run (fix round 1):
                  // a fresh preview gets one GraXpert step, a preview holding
                  // a PixelMath step has it REPLACED by the GraXpert step and
                  // shows the main image's pixels again. Read, never assumed.
                  const String unchanged = " The main image " + String( window.MainView().FullId() )
                                         + " was not changed.";
                  if ( g_inProcessAppliesExpected )
                     history = "(self-test, in-process: the preview's History is not recorded or read here.)"
                             + unchanged + " ";
                  else
                  {
                     const StepCheck c = CheckNewPreviewStep( view, historyBefore, P->Id() );
                     const bool hadStep = historyBefore.ok && historyBefore.ActiveCount() >= 1;
                     if ( c.verdict == StepCheck::Recorded )
                        history = "PixInsight still recorded an empty " + r.processId + " step as the preview "
                                + r.viewId + "'s History step (verified in its History list). A preview keeps ONE step, "
                                  "so " + (hadStep ? String( "it REPLACED the preview's earlier step, whose effect is "
                                                             "gone; " )
                                                   : String( "the preview had none before; " ))
                                + "Undo on the preview returns it to the main image's pixels there, so there is nothing "
                                  "to undo." + unchanged + " ";
                     else if ( c.verdict == StepCheck::NotRecorded )
                        history = "PixInsight recorded no History step for it on the preview " + r.viewId
                                + " (verified in its History list), so there is nothing to undo." + unchanged + " ";
                     else
                        history = "Whether PixInsight recorded an empty step as the preview " + r.viewId
                                + "'s History step could NOT be checked (" + c.reason + ")." + unchanged + " ";
                  }
               }
               throw ApplyError{ r.processId + " reported success but the image did not change: every pixel of "
                                 + r.viewId + " is identical to "
                                 + (sameAsOwn ? String( "before the run" )
                                              : String( "the main image's unprocessed pixels it started from (the "
                                                        "preview's earlier step no longer shows)" ))
                                 + " (checked by PI Copilot). " + why + history + tell };
            }
         }
         else
         {
            // Attribution (fix round 1): only a new window whose History
            // STARTS with a step of this process counts as its result --
            // measured, every GraXpert result window (GraXpert_background_
            // extraction[N], GraXpert_background[N]) has exactly one
            // initialProcessing step, "GraXpert". A window that opened
            // meanwhile for any other reason (the user, a script) does not.
            StringList others;
            for ( const std::string& id : OpenMainViewIds() )
            {
               if ( windowsBefore.count( id ) != 0 )
                  continue;
               const HistorySnapshot h = ReadViewHistory( IsoString( id.c_str() ), 0 );
               bool ours = false;
               if ( h.ok )
                  for ( const HistoryStep& st : h.steps )
                     if ( st.combinedIndex >= 0 && st.combinedIndex < h.initialLength
                       && st.processId == std::string( P->Id().c_str() ) )
                        ours = true;
               if ( ours )
                  r.resultWindows.push_back( id );
               else
                  others.Add( S16( id ) + (h.ok ? String( " (its History does not start with a " ) + r.processId
                                                  + " step)"
                                                : " (its History could not be read: "
                                                  + (h.busy ? String( "another script evaluation was running" ) : h.error)
                                                  + ")") );
            }
            if ( r.resultWindows.empty() )
            {
               r.noEffect = true;
               String meanwhile;
               if ( !others.IsEmpty() )
               {
                  meanwhile = "Window(s) that opened during the run but are NOT " + r.processId + " results: ";
                  for ( size_type i = 0; i < others.Length(); ++i )
                     meanwhile += (i > 0 ? "; " : "") + others[i];
                  meanwhile += ". ";
               }
               throw ApplyError{ r.processId + " reported success but produced no result window: no new image from "
                                 + r.processId + " opened (checked by PI Copilot; with " + String( bridge->replaceParameter )
                                 + " = false the result should open as a new image, and " + r.viewId + " is not changed "
                                 "in this mode, so there is nothing to undo). " + meanwhile + why + tell };
            }
         }
      }
      if ( !historyUpdater )
         r.undo = r.processId + " adds no History step to " + r.viewId + ": it does not change that image's History "
                  "(e.g. it creates a new image or changes only display settings), so there is nothing to undo there.";
      else if ( !isPreview && window.ModifyCount() > modifyCountBefore )
         r.undo = "Recorded in " + r.viewId + "'s History (verified: the image's modification count advanced); "
                  "the user can undo it with Edit > Undo.";
      else if ( g_inProcessAppliesExpected )
      {
         ++g_inProcessUnrecorded;   // self-test's own executeGlobal(): never recorded (see ProcessApply.h)
         r.undo = "(self-test, in-process: not recorded in History)";
      }
      else
      {
         // Main view whose ModifyCount did not advance -- a step that does
         // not count as a modification (e.g. a rename) or no step at all --
         // or a preview (ModifyCount never moves for one): the History list
         // decides (read from `view`: a rename changed the id).
         const StepCheck c = isPreview ? CheckNewPreviewStep( view, historyBefore, P->Id() )
                                       : CheckNewHistoryStep( view, historyBefore, P->Id() );
         if ( c.verdict == StepCheck::Recorded )
            r.undo = isPreview
               ? "Recorded as the preview " + r.viewId + "'s History step (verified). A preview keeps ONE step: it "
                 "was applied to the main image's pixels in that area and REPLACED any earlier preview step (whose "
                 "effect is gone); Undo on the preview returns it to the main image's pixels. A preview step does not "
                 "change the main image."
               : "Recorded in " + String( view.FullId() ) + "'s History (verified in the History list); the user "
                 "can undo it with Edit > Undo.";
         else if ( c.verdict == StepCheck::Unknown )
         {
            // Review round 2: never "ok" for a change nobody could verify.
            r.unverifiedChange = true;
            String changes = DescribeParameterChanges( parameters, tableParameters, 400 );
            changes.ReplaceString( "\n", "; " );
            throw ApplyError{ r.processId + " ran and changed " + r.viewId + ", but PI Copilot could NOT verify that "
                              "PixInsight recorded it in the image's History (" + c.reason + "). Tell the user this "
                              "now and ask them to check Edit > Undo (or the History Explorer) before continuing; do "
                              "not apply anything else to " + r.viewId + " until they have. What ran: " + r.processId
                              + " with " + changes };
         }
         else
         {
            r.unrecordedChange = true;
            String changes = DescribeParameterChanges( parameters, tableParameters, 400 );
            changes.ReplaceString( "\n", "; " );
            throw ApplyError{ r.processId + " CHANGED " + r.viewId + " but PixInsight did NOT record it in the "
                              "image's History (PixInsight was still busy executing another process), so Edit > Undo "
                              "cannot revert it. Tell the user this plainly now: the image was modified outside "
                              "History; the previous state can only be recovered from a saved copy or by redoing "
                              "the earlier steps. Do not apply anything else to " + r.viewId + " until the user "
                              "decides. What ran: " + r.processId + " with " + changes };
         }
      }
      r.ok = true;
   }
   catch ( const ApplyError& e )
   {
      r.ok = false;
      r.error = e.message;
      r.undo.Clear();
      if ( !r.unrecordedChange && !r.unverifiedChange )   // what DID change the image stays reported
      {
         r.parametersSet = nlohmann::json::object();
         r.pinnedSet = nlohmann::json::object();
      }
   }
   catch ( const pcl::Exception& x )
   {
      r.ok = false;
      r.error = "apply_process internal error: " + x.Message();
      r.parametersSet = nlohmann::json::object();
      r.pinnedSet = nlohmann::json::object();
   }
   catch ( const std::exception& x )
   {
      r.ok = false;
      r.error = String( "apply_process internal error: " ) + String( x.what() );
      r.parametersSet = nlohmann::json::object();
      r.pinnedSet = nlohmann::json::object();
   }
   catch ( ... )
   {
      r.ok = false;
      r.error = "apply_process internal error: unknown exception";
      r.parametersSet = nlohmann::json::object();
      r.pinnedSet = nlohmann::json::object();
   }
   return r;
}

void SplitNewWindows( const std::vector<std::string>& newWindows, const nlohmann::json& outputIds,
                      std::vector<std::string>& results, std::vector<std::string>& others )
{
   results.clear();
   others.clear();
   std::set<std::string> named;
   if ( outputIds.is_object() )
      for ( auto it = outputIds.begin(); it != outputIds.end(); ++it )
         if ( it.value().is_string() && !it.value().get<std::string>().empty() )
            named.insert( it.value().get<std::string>() );
   for ( const std::string& id : newWindows )
      (named.empty() || named.count( id ) ? results : others).push_back( id );
}

String PrecheckApplyRun( const IsoString& processId, const nlohmann::json& parameters,
                         const nlohmann::json& tableParameters, const std::vector<PinnedParameter>* pinned )
{
   try
   {
      std::unique_ptr<Process> P;
      try
      {
         P.reset( new Process( processId ) );
      }
      catch ( ... )
      {
         return "unknown process id '" + String( processId ) + "'; call list_processes for valid ids";
      }
      if ( !P->CanProcessViews() )
         return String( P->Id() ) + " can only run in the global context (not on a view); use run_global_process";
      // Dry run of SetParameters() on a throwaway DEFAULT instance: every
      // shape, type, range, enumeration, value-rule and duplicate-key error
      // comes back BEFORE any confirmation dialog. A denied/confirmAlways
      // process is never instantiated before the user approved (the RC-Astro
      // plug-ins can crash PixInsight): metadata-only checks; the rest runs
      // in ApplyProcess, after approval, with the same messages.
      nlohmann::json ignored = nlohmann::json::object(), ignoredPinned = nlohmann::json::object();
      if ( NoInstanceBeforeApproval( P->Id() ) )
         SetParameters( *P, nullptr, String( P->Id() ), parameters, tableParameters, ignored, ignoredPinned, pinned );
      else
      {
         NoteInstanceBuild( P->Id(), "precheckApply" );
         ProcessInstance probe( *P );
         SetParameters( *P, &probe, String( P->Id() ), parameters, tableParameters, ignored, ignoredPinned, pinned );
      }
   }
   catch ( const ApplyError& e )
   {
      return e.message;
   }
   catch ( const pcl::Exception& x )
   {
      return "apply_process internal error: " + x.Message();
   }
   catch ( const std::exception& x )
   {
      return String( "apply_process internal error: " ) + String( x.what() );
   }
   catch ( ... )
   {
      return "apply_process internal error: unknown exception";
   }
   return String();
}

String PrecheckGlobalRun( const IsoString& processId, const nlohmann::json& parameters,
                          const nlohmann::json& tableParameters, const std::vector<PinnedParameter>* pinned )
{
   try
   {
      const Process P( processId );
      if ( !P.CanProcessGlobal() )
         return String( P.Id() ) + " cannot run in the global context; use apply_process on a view";
   }
   catch ( ... )
   {
      return "unknown process id '" + String( processId ) + "'; call list_processes for valid ids";
   }
   const String files = ValidateProcessFilePaths( processId, parameters, tableParameters );
   if ( !files.IsEmpty() )
      return files;

   // Dry run of SetParameters() on a throwaway DEFAULT instance, so a shape,
   // enumeration or range error comes back BEFORE any confirmation dialog.
   // Denied/confirmAlways: metadata only, as in PrecheckApplyRun.
   try
   {
      const Process P( processId );
      nlohmann::json ignored = nlohmann::json::object(), ignoredPinned = nlohmann::json::object();
      if ( NoInstanceBeforeApproval( P.Id() ) )
         SetParameters( P, nullptr, String( P.Id() ), parameters, tableParameters, ignored, ignoredPinned, pinned );
      else
      {
         NoteInstanceBuild( P.Id(), "precheckGlobal" );
         ProcessInstance probe( P );
         SetParameters( P, &probe, String( P.Id() ), parameters, tableParameters, ignored, ignoredPinned, pinned );
      }
   }
   catch ( const ApplyError& e )
   {
      return e.message;
   }
   catch ( const pcl::Exception& x )
   {
      return "run_global_process internal error: " + x.Message();
   }
   catch ( const std::exception& x )
   {
      return String( "run_global_process internal error: " ) + String( x.what() );
   }
   catch ( ... )
   {
      return "run_global_process internal error: unknown exception";
   }
   return String();
}

GlobalRunResult RunGlobalProcess( const IsoString& processId, const nlohmann::json& parameters,
                                  const nlohmann::json& tableParameters, const std::vector<PinnedParameter>* pinned )
{
   GlobalRunResult r;
   std::set<std::string> before;
   bool started = false;
   try
   {
      const String pre = PrecheckGlobalRun( processId, parameters, tableParameters, pinned );
      if ( !pre.IsEmpty() )
         throw ApplyError{ pre };
      const Process P( processId );
      r.processId = String( P.Id() );
      NoteInstanceBuild( P.Id(), "global" );
      ProcessInstance instance( P );   // DEFAULT parameters
      SetParameters( P, &instance, r.processId, parameters, tableParameters, r.parametersSet, r.pinnedSet, pinned );

      String whyNot;
      if ( !instance.Validate( whyNot ) )
         throw ApplyError{ r.processId + " rejected the parameters: "
                           + (whyNot.IsEmpty() ? String( "(no reason given)" ) : whyNot) };
      whyNot.Clear();
      if ( !instance.CanExecuteGlobal( whyNot ) )
         throw ApplyError{ r.processId + " cannot run globally with these parameters: "
                           + (whyNot.IsEmpty() ? String( "(no reason given)" ) : whyNot) };

      before = OpenMainViewIds();
      started = true;
      const auto t0 = std::chrono::steady_clock::now();
      bool ran = false;
      try
      {
         ran = instance.ExecuteGlobal();
      }
      catch ( const pcl::Exception& x )
      {
         throw ApplyError{ r.processId + " failed while running: " + x.Message() };
      }
      r.elapsedMs = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
      // Output ids name the windows the process says it created (e.g.
      // ImageIntegration.integrationImageId); read-only string parameters.
      for ( const ProcessParameter& p : P.Parameters() )
         if ( p.IsString() && p.IsReadOnly() && p.Id().EndsWith( "ImageId" ) )
         {
            const String v = instance.ParameterValue( p, kScalarRow ).ToString();
            if ( !v.IsEmpty() )
               r.outputIds[std::string( p.Id().c_str() )] = U8( v );
         }
      // As for ExecuteOn(): a failure found only while running is false, with
      // the reason in the Process Console, which a module cannot read back.
      if ( !ran )
      {
         String changes = DescribeParameterChanges( parameters, tableParameters, 400 );
         changes.ReplaceString( "\n", "; " );
         throw ApplyError{ r.processId + " did not complete: the process stopped with an error while running, or the "
                           "user aborted it in PixInsight (the reason is in the Process Console). Do not simply retry: "
                           "if the user may have aborted it, ask them first; otherwise check the input files and the "
                           "values you set: " + changes };
      }
      r.ok = true;
   }
   catch ( const ApplyError& e )
   {
      r.error = e.message;
   }
   catch ( const pcl::Exception& x )
   {
      r.error = "run_global_process internal error: " + x.Message();
   }
   catch ( const std::exception& x )
   {
      r.error = String( "run_global_process internal error: " ) + String( x.what() );
   }
   catch ( ... )
   {
      r.error = "run_global_process internal error: unknown exception";
   }
   if ( started )
      try
      {
         std::vector<std::string> opened;
         for ( const std::string& id : OpenMainViewIds() )
            if ( before.count( id ) == 0 )
               opened.push_back( id );
         SplitNewWindows( opened, r.outputIds, r.createdWindows, r.otherNewWindows );
      }
      catch ( ... )
      {
      }
   if ( !r.ok )
   {
      r.parametersSet = nlohmann::json::object();
      r.pinnedSet = nlohmann::json::object();
   }
   return r;
}

void SetInstanceBuildObserverForSelfTest( InstanceBuildObserver observer )
{
   g_instanceObserver = std::move( observer );
}

void SetBeforeExecuteHookForSelfTest( std::function<void()> hook )
{
   g_beforeExecuteHook = std::move( hook );
}

void SetInProcessAppliesExpectedForSelfTest( bool on )
{
   g_inProcessAppliesExpected = on;
}

bool InProcessAppliesExpectedForSelfTest()
{
   return g_inProcessAppliesExpected;
}

int InProcessUnrecordedAppliesForSelfTest()
{
   return g_inProcessUnrecorded;
}

String DescribeParameterChanges( const nlohmann::json& parameters, const nlohmann::json& tableParameters,
                                 size_type maxChars )
{
   StringList lines;
   if ( parameters.is_object() )
      for ( auto it = parameters.begin(); it != parameters.end(); ++it )
         lines.Add( S16( it.key() ) + " = "
                    + S16( it.value().is_string() ? it.value().get<std::string>() : it.value().dump() ) );
   if ( tableParameters.is_object() )
      for ( auto it = tableParameters.begin(); it != tableParameters.end(); ++it )
         lines.Add( S16( it.key() ) + " = " + S16( it.value().dump() ) );
   if ( lines.IsEmpty() )
      return "(all parameters at their defaults)";

   auto join = [&lines]( size_type k )
   {
      String s;
      for ( size_type i = 0; i < k; ++i )
      {
         if ( i > 0 )
            s += "\n";
         s += lines[i];
      }
      return s;
   };
   // "… and N more parameter(s) not shown" (U+2026), N exact.
   auto more = []( size_type n )
   {
      return String::UTF8ToUTF16( "\xE2\x80\xA6 and " ) + String( unsigned( n ) ) + " more parameter(s) not shown";
   };

   const size_type n = lines.Length();
   const String all = join( n );
   if ( maxChars <= 3 || all.Length() <= maxChars )
      return all;

   // Whole lines while they fit together with the "N more" note.
   for ( size_type k = n - 1; k > 0; --k )
   {
      const String s = join( k ) + "\n" + more( n - k );
      if ( s.Length() <= maxChars )
         return s;
   }
   // Not even the first line fits whole: show its start, then the note.
   const String tail = n > 1 ? "\n" + more( n - 1 ) : String();
   const size_type room = maxChars > tail.Length() + 3 ? maxChars - tail.Length() - 3 : 0;
   return lines[0].Left( room ) + "..." + tail;
}

} // namespace pcl
