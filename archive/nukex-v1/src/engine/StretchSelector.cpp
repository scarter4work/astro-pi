//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "StretchSelector.h"

#include <algorithm>

namespace pcl
{

// ----------------------------------------------------------------------------
// StretchSelector Implementation
// ----------------------------------------------------------------------------

StretchSelector::StretchSelector()
{
   // Enable all regions by default
   for ( int i = 0; i < static_cast<int>( RegionClass::Count ); ++i )
   {
      m_enabledRegions[static_cast<RegionClass>( i )] = true;
   }
}

// ----------------------------------------------------------------------------

StretchSelector::~StretchSelector()
{
}

// ----------------------------------------------------------------------------

void StretchSelector::Initialize()
{
   m_rules = SelectionRulesEngine::CreateDefaultRules();
}

// ----------------------------------------------------------------------------

void StretchSelector::Initialize( const SelectionRulesEngine& rules )
{
   m_rules = rules;
}

// ----------------------------------------------------------------------------

std::map<RegionClass, SelectedStretch> StretchSelector::SelectAll(
   const RegionAnalysisResult& analysis ) const
{
   std::map<RegionClass, SelectedStretch> result;

   for ( const auto& pair : analysis.regionStats )
   {
      RegionClass region = pair.first;

      // Skip disabled regions
      if ( !IsRegionEnabled( region ) )
         continue;

      result.emplace( region, Select( region, pair.second ) );
   }

   return result;
}

// ----------------------------------------------------------------------------

SelectedStretch StretchSelector::Select( RegionClass region,
                                          const RegionStatistics& stats ) const
{
   SelectedStretch result;
   result.region = region;
   result.statistics = stats;

   // Check for user override first
   auto overrideIt = m_overrides.find( region );
   if ( overrideIt != m_overrides.end() && overrideIt->second.enabled )
   {
      const UserOverride& override = overrideIt->second;
      result.algorithm = override.algorithm;
      result.parameters = override.parameters;
      result.confidence = 1.0;
      result.rationale = IsoString( "User override" );
      result.isOverride = true;
   }
   else
   {
      // Use rules engine
      AlgorithmRecommendation rec = m_rules.GetRecommendation( region, stats );
      result.algorithm = rec.algorithm;
      result.parameters = rec.parameters;
      result.confidence = rec.confidence;
      result.rationale = rec.rationale;
      result.isOverride = false;
   }

   // Apply global strength modifier
   ApplyStrengthModifier( result.parameters );

   // Create the algorithm instance
   AlgorithmRecommendation tempRec( result.algorithm, result.confidence );
   tempRec.rationale = result.rationale;
   tempRec.parameters = result.parameters;
   result.algorithmInstance = CreateAlgorithm( tempRec, stats );

   if ( result.algorithmInstance )
   {
      // Apply parameters to the algorithm
      for ( const auto& param : result.parameters )
      {
         result.algorithmInstance->SetParameter( param.first, param.second );
      }

      // Auto-configure based on statistics
      result.algorithmInstance->AutoConfigure( stats );
   }

   return result;
}

// ----------------------------------------------------------------------------

void StretchSelector::SetOverride( const UserOverride& override )
{
   m_overrides[override.region] = override;
}

// ----------------------------------------------------------------------------

void StretchSelector::ClearOverride( RegionClass region )
{
   m_overrides.erase( region );
}

// ----------------------------------------------------------------------------

void StretchSelector::ClearAllOverrides()
{
   m_overrides.clear();
}

// ----------------------------------------------------------------------------

bool StretchSelector::HasOverride( RegionClass region ) const
{
   auto it = m_overrides.find( region );
   return it != m_overrides.end() && it->second.enabled;
}

// ----------------------------------------------------------------------------

const UserOverride* StretchSelector::GetOverride( RegionClass region ) const
{
   auto it = m_overrides.find( region );
   return (it != m_overrides.end()) ? &it->second : nullptr;
}

// ----------------------------------------------------------------------------

void StretchSelector::EnableRegion( RegionClass region, bool enable )
{
   m_enabledRegions[region] = enable;
}

// ----------------------------------------------------------------------------

bool StretchSelector::IsRegionEnabled( RegionClass region ) const
{
   auto it = m_enabledRegions.find( region );
   return (it != m_enabledRegions.end()) ? it->second : true;
}

// ----------------------------------------------------------------------------

void StretchSelector::SetGlobalStrength( double strength )
{
   m_globalStrength = std::max( 0.0, std::min( 2.0, strength ) );
}

// ----------------------------------------------------------------------------

IsoString StretchSelector::GetRationale( RegionClass region,
                                         const RegionStatistics& stats ) const
{
   if ( HasOverride( region ) )
   {
      return IsoString( "User override applied" );
   }

   AlgorithmRecommendation rec = m_rules.GetRecommendation( region, stats );
   return rec.rationale;
}

// ----------------------------------------------------------------------------

std::vector<AlgorithmRecommendation> StretchSelector::GetAllRecommendations(
   RegionClass region, const RegionStatistics& stats ) const
{
   return m_rules.GetAllRecommendations( region, stats );
}

// ----------------------------------------------------------------------------

std::unique_ptr<IStretchAlgorithm> StretchSelector::CreateAlgorithm(
   const AlgorithmRecommendation& rec, const RegionStatistics& stats ) const
{
   auto algorithm = StretchLibrary::Instance().Create( rec.algorithm );

   if ( algorithm )
   {
      // Apply recommendation parameters
      for ( const auto& param : rec.parameters )
      {
         algorithm->SetParameter( param.first, param.second );
      }
   }

   return algorithm;
}

// ----------------------------------------------------------------------------

void StretchSelector::ApplyStrengthModifier( std::map<IsoString, double>& params ) const
{
   if ( std::abs( m_globalStrength - 1.0 ) < 0.001 )
      return;  // No modification needed

   // Scale intensity-related parameters
   static const std::vector<IsoString> intensityParams = {
      "stretchFactor", "scale", "softness", "Q", "midtones", "strength"
   };

   for ( const IsoString& name : intensityParams )
   {
      auto it = params.find( name );
      if ( it != params.end() )
      {
         // Scale the parameter by global strength
         // For "softness" and "midtones", inverse relationship
         if ( name == "softness" || name == "midtones" )
         {
            // Lower value = more stretch, so divide by strength
            it->second /= m_globalStrength;
         }
         else
         {
            it->second *= m_globalStrength;
         }
      }
   }
}

// ----------------------------------------------------------------------------
// SelectionSummary Implementation
// ----------------------------------------------------------------------------

SelectionSummary SelectionSummary::Create(
   const std::map<RegionClass, SelectedStretch>& selections,
   const std::map<RegionClass, double>& coverage )
{
   SelectionSummary summary;
   summary.totalRegions = static_cast<int>( selections.size() );
   summary.overriddenRegions = 0;
   summary.averageConfidence = 1.0;

   double confidenceSum = 0;

   for ( const auto& pair : selections )
   {
      RegionEntry entry;
      entry.region = pair.first;
      entry.algorithm = pair.second.algorithm;
      entry.confidence = pair.second.confidence;
      entry.isOverride = pair.second.isOverride;
      entry.rationale = pair.second.rationale;  // Capture rationale

      auto covIt = coverage.find( pair.first );
      entry.coverage = (covIt != coverage.end()) ? covIt->second : 0;

      if ( entry.isOverride )
         ++summary.overriddenRegions;
      confidenceSum += entry.confidence;

      summary.entries.push_back( entry );
   }

   if ( summary.totalRegions > 0 )
      summary.averageConfidence = confidenceSum / summary.totalRegions;

   // Sort by coverage (descending)
   std::sort( summary.entries.begin(), summary.entries.end(),
              []( const RegionEntry& a, const RegionEntry& b ) {
                 return a.coverage > b.coverage;
              } );

   return summary;
}

// ----------------------------------------------------------------------------

String SelectionSummary::ToString() const
{
   String result;
   result += String().Format( "Selection Summary (%d regions, %d overrides)\n",
                               totalRegions, overriddenRegions );
   result += String().Format( "Average Confidence: %.0f%%\n\n", averageConfidence * 100 );

   for ( const auto& entry : entries )
   {
      // Compute names on-demand to avoid storing PCL Strings
      // Use IsoString (UTF-8) for Format %s - String (UTF-16) c_str() doesn't work with %s
      IsoString regionName = IsoString( RegionClassDisplayName( entry.region ) );
      IsoString algorithmName = IsoString( StretchLibrary::TypeToName( entry.algorithm ) );
      IsoString algorithmId = StretchLibrary::TypeToId( entry.algorithm );

      result += String().Format( "%s (%.1f%% coverage):\n",
                                  regionName.c_str(),
                                  entry.coverage * 100 );
      result += String().Format( "  Algorithm: %s (%s)%s\n",
                                  algorithmName.c_str(),
                                  algorithmId.c_str(),
                                  entry.isOverride ? " [Override]" : "" );
      result += String().Format( "  Confidence: %.0f%%\n", entry.confidence * 100 );
      if ( !entry.rationale.IsEmpty() )
      {
         result += String().Format( "  Rationale: %s\n", IsoString( entry.rationale ).c_str() );
      }
      result += "\n";
   }

   return result;
}

// ----------------------------------------------------------------------------

} // namespace pcl
