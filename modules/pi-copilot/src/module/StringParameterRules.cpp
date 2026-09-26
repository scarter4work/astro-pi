// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "StringParameterRules.h"

#include <pcl/Exception.h>
#include <pcl/Process.h>
#include <pcl/api/APIInterface.h>

#include <set>

namespace pcl
{

namespace
{

// The PixInsight identifier characters (String::IsValidIdentifier()).
const char* const kIdentifierSet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_";
// FITS keyword parameters of FluxCalibration: identifier characters plus '-'.
const char* const kKeywordSet    = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-";

struct CompiledRule
{
   const char* path;      // StringParameterPath()
   const char* allowed;   // copied from the process's AllowedCharacters() override
};

// Every String parameter of PixInsight 1.9.x that declares a character set
// (the self-test enumerates all installed String parameters and requires each
// one that the core cannot report to be here, with the core's length).
// Sources: ~/PCL/src/modules/processes/<dir>/<X>Parameters.cpp, the
// AllowedCharacters() overrides (ImageIdentifier lists the same 63 characters
// in another order; the order does not matter). The last six are in modules
// with no PCL source; each holds a view identifier (its value is only ever
// used to name or find a view, and the core reports 63 characters, the size
// of the identifier set).
const CompiledRule kCompiledRules[] =
{
   { "PixelMath.newImageId",                         kIdentifierSet },   // PixelMath/PixelMathParameters.cpp
   { "NewImage.id",                                  kIdentifierSet },   // Image/NewImageParameters.cpp
   { "ImageIdentifier.id",                           kIdentifierSet },   // Image/ImageIdentifierParameters.cpp
   { "CreateAlphaChannels.sourceId",                 kIdentifierSet },   // Image/CreateAlphaChannelsParameters.cpp
   { "DynamicPSF.views.id",                          kIdentifierSet },   // Image/DynamicPSFParameters.cpp
   { "ExtractAlphaChannels.channelList",             "0123456789, " },   // Image/ExtractAlphaChannelsParameters.cpp
   { "ChannelCombination.channels.id",               kIdentifierSet },   // ColorSpaces/ChannelParameters.cpp
   { "ChannelExtraction.channels.id",                kIdentifierSet },   // ColorSpaces/ChannelParameters.cpp
   { "LRGBCombination.channels.id",                  kIdentifierSet },   // ColorSpaces (LRGBChannelId : ChannelId)
   { "CloneStamp.actions.sourceId",                  kIdentifierSet },   // CloneStamp/CloneStampParameters.cpp
   { "B3Estimator.inputViewId1",                     kIdentifierSet },   // Flux/B3EParameters.cpp
   { "B3Estimator.inputViewId2",                     kIdentifierSet },   // Flux/B3EParameters.cpp
   { "InverseFourierTransform.idOfFirstComponent",   kIdentifierSet },   // Fourier/InverseFourierTransformParameters.cpp
   { "InverseFourierTransform.idOfSecondComponent",  kIdentifierSet },   // Fourier/InverseFourierTransformParameters.cpp
   { "FluxCalibration.wavelengthKeyword",            kKeywordSet },      // Flux/FluxCalibrationParameters.cpp
   { "FluxCalibration.transmissivityKeyword",        kKeywordSet },
   { "FluxCalibration.filterWidthKeyword",           kKeywordSet },
   { "FluxCalibration.apertureKeyword",              kKeywordSet },
   { "FluxCalibration.centralObstructionKeyword",    kKeywordSet },
   { "FluxCalibration.exposureTimeKeyword",          kKeywordSet },
   { "FluxCalibration.atmosphericExtinctionKeyword", kKeywordSet },
   { "FluxCalibration.sensorGainKeyword",            kKeywordSet },
   { "FluxCalibration.quantumEfficiencyKeyword",     kKeywordSet },
   // No PCL source: view identifiers.
   { "AutomaticBackgroundExtractor.correctedImageId", kIdentifierSet },
   { "DynamicBackgroundExtraction.modelId",           kIdentifierSet },
   { "DynamicBackgroundExtraction.correctedImageId",  kIdentifierSet },
   { "DynamicAlignment.sourceImageId",                kIdentifierSet },
   { "DynamicAlignment.registeredImageId",            kIdentifierSet },
   { "Divide.flatId",                                 kIdentifierSet },
};

bool SameCharacterSet( const String& a, const String& b )
{
   const std::set<char16_type> x( a.Begin(), a.End() ), y( b.Begin(), b.End() );
   return x == y;
}

// The core's own count of declared characters: the size query (null buffer)
// of GetParameterAllowedCharacters, which works (only the copy is broken).
// The raw handle is looked up by name: ProcessParameter::Handle() is private.
bool DeclaredAllowedLength( const ProcessParameter& p, size_type& length, String& why )
{
   length = 0;
   const ProcessParameter table = p.ParentTable();
   const IsoString processId = p.ParentProcess().Id();
   meta_process_handle hp = (*API->Process->GetProcessByName)( ModuleHandle(), processId.c_str() );
   if ( hp == nullptr )
   {
      why = "the core did not resolve the process by its id";
      return false;
   }
   meta_parameter_handle h = nullptr;
   if ( table.IsNull() )
      h = (*API->Process->GetParameterByName)( hp, p.Id().c_str() );
   else
   {
      meta_parameter_handle ht = (*API->Process->GetParameterByName)( hp, table.Id().c_str() );
      if ( ht != nullptr )
         h = (*API->Process->GetTableColumnByName)( ht, p.Id().c_str() );
   }
   if ( h == nullptr )
   {
      why = "the core did not resolve the parameter by its id";
      return false;
   }
   (*API->Process->GetParameterAllowedCharacters)( h, nullptr, &length );   // size query: returns false by design
   return true;
}

StringCharacterRule FromSet( const String& allowed, size_type declared, const char* source )
{
   StringCharacterRule r;
   r.ok = true;
   r.declaredLength = declared;
   r.source = source;
   r.allowed = allowed;
   if ( allowed.IsEmpty() )
      r.kind = StringCharacterRuleKind::None;
   else if ( SameCharacterSet( allowed, String( kIdentifierSet ) ) )
      r.kind = StringCharacterRuleKind::Identifier;
   else
      r.kind = StringCharacterRuleKind::CharacterSet;
   return r;
}

} // namespace

String StringParameterPath( const ProcessParameter& p )
{
   const ProcessParameter table = p.ParentTable();
   String path = String( p.ParentProcess().Id() ) + ".";
   if ( !table.IsNull() )
      path += String( table.Id() ) + ".";
   return path + String( p.Id() );
}

StringCharacterRule ResolveStringCharacterRule( const ProcessParameter& p )
{
   StringCharacterRule r;
   String path;
   try
   {
      path = StringParameterPath( p );
      if ( !p.IsString() )
      {
         r.error = path + ": not a text parameter";
         return r;
      }
      size_type declared = 0;
      String why;
      if ( !DeclaredAllowedLength( p, declared, why ) )
      {
         r.error = path + ": cannot read its allowed characters (" + why + "); the value is not set";
         return r;
      }
      String coreError;
      try
      {
         const String allowed = p.AllowedCharacters();
         if ( allowed.Length() == declared )
            return FromSet( allowed, declared, allowed.IsEmpty() ? "" : "core" );
         coreError = String().Format( "the core returned %u characters but declares %u",
                                      unsigned( allowed.Length() ), unsigned( declared ) );
      }
      catch ( const pcl::Exception& x )
      {
         coreError = x.Message();   // the known defect (see the header)
      }
      for ( const CompiledRule& c : kCompiledRules )
         if ( path == String( c.path ) )
         {
            const String allowed( c.allowed );
            if ( allowed.Length() == declared )
               return FromSet( allowed, declared, "compiled-in" );
            r.declaredLength = declared;
            r.error = path + String().Format( ": PixInsight declares %u allowed characters for this text parameter, "
                                              "but PI Copilot's copy of the set has %u (a different PixInsight version?), "
                                              "and the core cannot report the set itself (",
                                              unsigned( declared ), unsigned( allowed.Length() ) )
                      + coreError + "); the value cannot be checked, so it is not set";
            return r;
         }
      r.declaredLength = declared;
      r.error = path + String().Format( ": PixInsight declares %u allowed characters for this text parameter "
                                        "but cannot report which (", unsigned( declared ) )
                + coreError + "), and PI Copilot has no verified copy of the set; the value cannot be checked, "
                "so it is not set";
      return r;
   }
   catch ( const pcl::Exception& x )
   {
      r.ok = false;
      r.error = (path.IsEmpty() ? String( "text parameter" ) : path) + ": " + x.Message();
      return r;
   }
   catch ( ... )
   {
      r.ok = false;
      r.error = (path.IsEmpty() ? String( "text parameter" ) : path) + ": unknown error reading its allowed characters";
      return r;
   }
}

String StringCharacterProblem( const StringCharacterRule& rule, const String& s, const String& name )
{
   if ( !rule.ok )
      return rule.error;
   if ( rule.kind == StringCharacterRuleKind::None )
      return String();
   String bad;
   for ( size_type i = 0; i < s.Length() && bad.IsEmpty(); ++i )
      if ( !rule.allowed.Contains( s[i] ) )
         bad = "character '" + s.Substring( i, 1 ) + String().Format( "' at position %u is not allowed", unsigned( i ) );
   if ( rule.kind == StringCharacterRuleKind::Identifier )
   {
      if ( bad.IsEmpty() && !s.IsEmpty() && !s.IsValidIdentifier() )
         bad = "it starts with the digit '" + s.Substring( 0, 1 ) + "'";
      if ( !bad.IsEmpty() )
         return name + ": '" + s + "' is not a valid PixInsight identifier (" + bad
                + "); use only letters A-Z/a-z, digits 0-9 and underscores, not starting with a digit";
      return String();
   }
   if ( !bad.IsEmpty() )
      return name + ": " + bad + "; allowed characters: " + rule.allowed;
   return String();
}

size_type CompiledStringCharacterRuleCount()
{
   return sizeof( kCompiledRules )/sizeof( *kCompiledRules );
}

String CompiledStringCharacterRulePath( size_type i )
{
   return i < CompiledStringCharacterRuleCount() ? String( kCompiledRules[i].path ) : String();
}

} // namespace pcl
