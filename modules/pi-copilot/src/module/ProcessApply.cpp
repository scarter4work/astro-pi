// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ProcessApply.h"
#include "GlobalRunFiles.h"
#include "ProcessCatalog.h"
#include "Utf8.h"

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

// A model-correctable failure. Thrown only inside ApplyProcess() / RunGlobalProcess()
// (and the helpers they call) and caught there.
struct ApplyError
{
   String message;
};

String S16( const std::string& utf8 )
{
   return String::UTF8ToUTF16( utf8.c_str() );
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
      const IsoString want( v.get<std::string>().c_str() );
      for ( const ProcessParameter::EnumerationElement& e : *elements )
      {
         if ( e.id == want )
            return Variant( e.value );
         for ( const IsoString& a : e.aliases )
            if ( a.Trimmed() == want )
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
      const String allowed = p.AllowedCharacters();
      if ( !allowed.IsEmpty() )
         for ( size_type i = 0; i < s.Length(); ++i )
            if ( !allowed.Contains( s[i] ) )
               throw ApplyError{ name + ": character '" + String( s[i] )
                                 + String().Format( "' at position %u is not allowed; allowed characters: ", unsigned( i ) )
                                 + allowed };
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
void SetParameters( const Process& P, ProcessInstance& instance, const String& processId,
                    const nlohmann::json& parameters, const nlohmann::json& tableParameters,
                    nlohmann::json& parametersSet )
{
   if ( !parameters.is_null() && !parameters.is_object() )
      throw ApplyError{ String( "parameters must be an object {parameterId: value}" ) };
   if ( parameters.is_object() )
      for ( auto it = parameters.begin(); it != parameters.end(); ++it )
      {
         const String name = processId + "." + S16( it.key() );
         const ProcessParameter p = FindParameter( P, it.key(), name );
         if ( p.IsTable() )
            throw ApplyError{ name + " is a table parameter; pass it in table_parameters as [[row values]...]" };
         if ( p.IsReadOnly() )
            throw ApplyError{ name + " is read-only" };
         SetChecked( instance, p, ToVariant( p, it.value(), name ), kScalarRow, name );
         parametersSet[it.key()] = it.value();
      }

   if ( !tableParameters.is_null() && !tableParameters.is_object() )
      throw ApplyError{ String( "table_parameters must be an object {tableId: [[row values]...]}" ) };
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
         if ( !instance.AllocateTableRows( p, rows.size() ) )
            throw ApplyError{ name + String().Format( ": PixInsight refused a table of %u rows", unsigned( rows.size() ) ) };
         for ( size_type i = 0; i < rows.size(); ++i )
            for ( size_type k = 0; k < columns.Length(); ++k )
            {
               const String cell = name + String().Format( "[%u].", unsigned( i ) ) + String( columns[k].Id() );
               SetChecked( instance, columns[k], ToVariant( columns[k], rows[i][k], cell ), i, cell );
            }
         parametersSet[it.key()] = rows;
      }
}

std::set<std::string> OpenMainViewIds()
{
   std::set<std::string> ids;
   for ( const ImageWindow& w : ImageWindow::AllWindows() )
      ids.insert( std::string( w.MainView().Id().c_str() ) );
   return ids;
}

} // namespace

ApplyProcessResult ApplyProcess( const IsoString& processId, const nlohmann::json& parameters,
                                 const nlohmann::json& tableParameters, View view )
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

      ProcessInstance instance( *P );   // DEFAULT parameters

      SetParameters( *P, instance, r.processId, parameters, tableParameters, r.parametersSet );

      String whyNot;
      if ( !instance.Validate( whyNot ) )
         throw ApplyError{ r.processId + " rejected the parameters: "
                           + (whyNot.IsEmpty() ? String( "(no reason given)" ) : whyNot) };
      if ( !instance.CanExecuteOn( view, whyNot ) )
         throw ApplyError{ r.processId + " cannot run on " + r.viewId + ": "
                           + (whyNot.IsEmpty() ? String( "(no reason given)" ) : whyNot) };

      if ( ViewBusy( view ) )
         throw ApplyError{ BusyMessage( r.viewId ) };

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
      r.ok = true;
   }
   catch ( const ApplyError& e )
   {
      r.ok = false;
      r.error = e.message;
      r.parametersSet = nlohmann::json::object();
   }
   catch ( const pcl::Exception& x )
   {
      r.ok = false;
      r.error = "apply_process internal error: " + x.Message();
      r.parametersSet = nlohmann::json::object();
   }
   catch ( const std::exception& x )
   {
      r.ok = false;
      r.error = String( "apply_process internal error: " ) + String( x.what() );
      r.parametersSet = nlohmann::json::object();
   }
   catch ( ... )
   {
      r.ok = false;
      r.error = "apply_process internal error: unknown exception";
      r.parametersSet = nlohmann::json::object();
   }
   return r;
}

String PrecheckGlobalRun( const IsoString& processId, const nlohmann::json& parameters,
                          const nlohmann::json& tableParameters )
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
   // PHASE-B: call ValidateProcessFilePaths() (ProcessSafety), which
   // passes Section( CompiledProcessSafety(), "fileTables" ) here instead.
   return ValidateGlobalRunFilePaths( processId, PhaseAFileTables(), parameters, tableParameters );
}

GlobalRunResult RunGlobalProcess( const IsoString& processId, const nlohmann::json& parameters,
                                  const nlohmann::json& tableParameters )
{
   GlobalRunResult r;
   std::set<std::string> before;
   bool started = false;
   try
   {
      const String pre = PrecheckGlobalRun( processId, parameters, tableParameters );
      if ( !pre.IsEmpty() )
         throw ApplyError{ pre };
      const Process P( processId );
      r.processId = String( P.Id() );
      ProcessInstance instance( P );   // DEFAULT parameters
      SetParameters( P, instance, r.processId, parameters, tableParameters, r.parametersSet );

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
         for ( const std::string& id : OpenMainViewIds() )
            if ( before.count( id ) == 0 )
               r.createdWindows.push_back( id );
      }
      catch ( ... )
      {
      }
   if ( !r.ok )
      r.parametersSet = nlohmann::json::object();
   return r;
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
