//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "StretchLibrary.h"

// Include all algorithm headers
#include "algorithms/MTFStretch.h"
#include "algorithms/ArcSinhStretch.h"
#include "algorithms/HistogramStretch.h"
#include "algorithms/LogStretch.h"
#include "algorithms/GHStretch.h"
#include "algorithms/LumptonStretch.h"
#include "algorithms/RNCStretch.h"
#include "algorithms/PhotometricStretch.h"
#include "algorithms/OTSStretch.h"
#include "algorithms/SASStretch.h"
#include "algorithms/VeraluxStretch.h"
#include "algorithms/StatisticalAutoStretch.h"

namespace pcl
{

// ----------------------------------------------------------------------------

StretchLibrary& StretchLibrary::Instance()
{
   static StretchLibrary instance;
   return instance;
}

// ----------------------------------------------------------------------------

StretchLibrary::StretchLibrary()
{
   InitializeRegistry();
}

// ----------------------------------------------------------------------------

void StretchLibrary::InitializeRegistry()
{
   // Register MTF Stretch
   Register( AlgorithmType::MTF,
             AlgorithmInfo( AlgorithmType::MTF,
                            "MTF",
                            "Midtones Transfer Function",
                            "Classic PixInsight midtones transfer function stretch.",
                            true ),
             []() { return std::make_unique<MTFStretch>(); } );

   // Register Histogram Stretch
   Register( AlgorithmType::Histogram,
             AlgorithmInfo( AlgorithmType::Histogram,
                            "Histogram",
                            "Histogram Transformation",
                            "Classic histogram transformation with clipping and MTF.",
                            true ),
             []() { return std::make_unique<HistogramStretch>(); } );

   // Register GHS
   Register( AlgorithmType::GHS,
             AlgorithmInfo( AlgorithmType::GHS,
                            "GHS",
                            "Generalized Hyperbolic Stretch",
                            "Sophisticated stretch with symmetry and protection controls.",
                            true ),
             []() { return std::make_unique<GHStretch>(); } );

   // Register ArcSinh Stretch
   Register( AlgorithmType::ArcSinh,
             AlgorithmInfo( AlgorithmType::ArcSinh,
                            "ArcSinh",
                            "Inverse Hyperbolic Sine",
                            "HDR-friendly stretch excellent for star cores and bright regions.",
                            true ),
             []() { return std::make_unique<ArcSinhStretch>(); } );

   // Register Log Stretch
   Register( AlgorithmType::Log,
             AlgorithmInfo( AlgorithmType::Log,
                            "Log",
                            "Logarithmic Stretch",
                            "Aggressive stretch for revealing very faint detail.",
                            true ),
             []() { return std::make_unique<LogStretch>(); } );

   // Register Lumpton Stretch
   Register( AlgorithmType::Lumpton,
             AlgorithmInfo( AlgorithmType::Lumpton,
                            "Lumpton",
                            "Lumpton (SDSS HDR)",
                            "SDSS-style HDR stretch for astronomical survey data.",
                            true ),
             []() { return std::make_unique<LumptonStretch>(); } );

   // Register RNC Stretch
   Register( AlgorithmType::RNC,
             AlgorithmInfo( AlgorithmType::RNC,
                            "RNC",
                            "RNC Color Stretch",
                            "Color-preserving stretch algorithm.",
                            true ),
             []() { return std::make_unique<RNCStretch>(); } );

   // Register Photometric Stretch
   Register( AlgorithmType::Photometric,
             AlgorithmInfo( AlgorithmType::Photometric,
                            "Photometric",
                            "Photometric Stretch",
                            "Stretch that maintains photometric accuracy.",
                            true ),
             []() { return std::make_unique<PhotometricStretch>(); } );

   // Register OTS
   Register( AlgorithmType::OTS,
             AlgorithmInfo( AlgorithmType::OTS,
                            "OTS",
                            "Optimal Transfer Stretch",
                            "Automatic optimal transfer function stretch.",
                            true ),
             []() { return std::make_unique<OTSStretch>(); } );

   // Register SAS
   Register( AlgorithmType::SAS,
             AlgorithmInfo( AlgorithmType::SAS,
                            "SAS",
                            "Statistical Adaptive Stretch",
                            "Noise-aware stretch with statistical adaptation.",
                            true ),
             []() { return std::make_unique<SASStretch>(); } );

   // Register Veralux
   Register( AlgorithmType::Veralux,
             AlgorithmInfo( AlgorithmType::Veralux,
                            "Veralux",
                            "Veralux (Film Response)",
                            "Film-like response curve stretch.",
                            true ),
             []() { return std::make_unique<VeraluxStretch>(); } );

   // Register Statistical Auto-Stretch
   Register( AlgorithmType::StatAuto,
             AlgorithmInfo( AlgorithmType::StatAuto,
                            "StatAuto",
                            "Statistical Auto-Stretch",
                            "Pure statistics-based automatic stretch. Analyzes signal compression "
                            "to determine optimal parameters. No ML required.",
                            true ),
             []() { return std::make_unique<StatisticalAutoStretch>(); } );

   // Register Auto (special - uses region-based selection)
   Register( AlgorithmType::Auto,
             AlgorithmInfo( AlgorithmType::Auto,
                            "Auto",
                            "Auto (AI Selected)",
                            "Automatic algorithm selection based on region analysis.",
                            true ),  // "Implemented" means the selector handles it
             []() { return std::make_unique<GHStretch>(); } );  // Default to GHS for auto
}

// ----------------------------------------------------------------------------

void StretchLibrary::Register( AlgorithmType type, const AlgorithmInfo& info, FactoryFunc factory )
{
   m_registry[type] = info;
   if ( factory )
      m_factories[type] = factory;
}

// ----------------------------------------------------------------------------

std::unique_ptr<IStretchAlgorithm> StretchLibrary::Create( AlgorithmType type ) const
{
   auto factoryIt = m_factories.find( type );
   if ( factoryIt != m_factories.end() && factoryIt->second )
   {
      return factoryIt->second();
   }

   // Fall back to MTF if requested algorithm not implemented
   auto mtfIt = m_factories.find( AlgorithmType::MTF );
   if ( mtfIt != m_factories.end() && mtfIt->second )
   {
      return mtfIt->second();
   }

   return nullptr;
}

// ----------------------------------------------------------------------------

std::unique_ptr<IStretchAlgorithm> StretchLibrary::Create( const IsoString& id ) const
{
   return Create( IdToType( id ) );
}

// ----------------------------------------------------------------------------

const AlgorithmInfo& StretchLibrary::GetInfo( AlgorithmType type ) const
{
   static AlgorithmInfo nullInfo;
   auto it = m_registry.find( type );
   return ( it != m_registry.end() ) ? it->second : nullInfo;
}

// ----------------------------------------------------------------------------

std::vector<AlgorithmInfo> StretchLibrary::GetAllAlgorithms() const
{
   std::vector<AlgorithmInfo> result;
   result.reserve( m_registry.size() );

   for ( const auto& pair : m_registry )
   {
      result.push_back( pair.second );
   }

   return result;
}

// ----------------------------------------------------------------------------

std::vector<AlgorithmInfo> StretchLibrary::GetImplementedAlgorithms() const
{
   std::vector<AlgorithmInfo> result;

   for ( const auto& pair : m_registry )
   {
      if ( pair.second.implemented )
      {
         result.push_back( pair.second );
      }
   }

   return result;
}

// ----------------------------------------------------------------------------

IsoString StretchLibrary::TypeToId( AlgorithmType type )
{
   switch ( type )
   {
   case AlgorithmType::MTF:         return "MTF";
   case AlgorithmType::Histogram:   return "Histogram";
   case AlgorithmType::GHS:         return "GHS";
   case AlgorithmType::ArcSinh:     return "ArcSinh";
   case AlgorithmType::Log:         return "Log";
   case AlgorithmType::Lumpton:     return "Lumpton";
   case AlgorithmType::RNC:         return "RNC";
   case AlgorithmType::Photometric: return "Photometric";
   case AlgorithmType::OTS:         return "OTS";
   case AlgorithmType::SAS:         return "SAS";
   case AlgorithmType::Veralux:     return "Veralux";
   case AlgorithmType::StatAuto:    return "StatAuto";
   case AlgorithmType::Auto:        return "Auto";
   default:                         return "MTF";
   }
}

// ----------------------------------------------------------------------------

AlgorithmType StretchLibrary::IdToType( const IsoString& id )
{
   if ( id == "MTF" )         return AlgorithmType::MTF;
   if ( id == "Histogram" )   return AlgorithmType::Histogram;
   if ( id == "GHS" )         return AlgorithmType::GHS;
   if ( id == "ArcSinh" )     return AlgorithmType::ArcSinh;
   if ( id == "Log" )         return AlgorithmType::Log;
   if ( id == "Lumpton" )     return AlgorithmType::Lumpton;
   if ( id == "RNC" )         return AlgorithmType::RNC;
   if ( id == "Photometric" ) return AlgorithmType::Photometric;
   if ( id == "OTS" )         return AlgorithmType::OTS;
   if ( id == "SAS" )         return AlgorithmType::SAS;
   if ( id == "Veralux" )     return AlgorithmType::Veralux;
   if ( id == "StatAuto" )    return AlgorithmType::StatAuto;
   if ( id == "Auto" )        return AlgorithmType::Auto;

   return AlgorithmType::MTF; // Default
}

// ----------------------------------------------------------------------------

String StretchLibrary::TypeToName( AlgorithmType type )
{
   return Instance().GetInfo( type ).name;
}

// ----------------------------------------------------------------------------

bool StretchLibrary::IsImplemented( AlgorithmType type ) const
{
   auto it = m_registry.find( type );
   return ( it != m_registry.end() ) ? it->second.implemented : false;
}

// ----------------------------------------------------------------------------

} // namespace pcl
