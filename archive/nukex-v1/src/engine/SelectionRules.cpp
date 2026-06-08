//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter

#include "SelectionRules.h"

#include <algorithm>
#include <cmath>

namespace pcl
{

// ----------------------------------------------------------------------------
// SelectionRule Implementation
// ----------------------------------------------------------------------------

SelectionRule::SelectionRule( const String& name,
                              RegionClass targetRegion,
                              AlgorithmType algorithm,
                              double priority )
   : m_name( IsoString( name ) )
   , m_targetRegion( targetRegion )
   , m_algorithm( algorithm )
   , m_priority( priority )
   , m_confidence( 1.0 )
   , m_rationale()
{
}

// ----------------------------------------------------------------------------

SelectionRule::SelectionRule( const SelectionRule& other )
   : m_name( other.m_name )
   , m_targetRegion( other.m_targetRegion )
   , m_algorithm( other.m_algorithm )
   , m_priority( other.m_priority )
   , m_confidence( other.m_confidence )
   , m_rationale( other.m_rationale )
   , m_conditions( other.m_conditions )
   , m_staticParams( other.m_staticParams )
   , m_dynamicParams( other.m_dynamicParams )
{
}

// ----------------------------------------------------------------------------

SelectionRule& SelectionRule::operator=( const SelectionRule& other )
{
   if ( this != &other )
   {
      m_name = other.m_name;
      m_targetRegion = other.m_targetRegion;
      m_algorithm = other.m_algorithm;
      m_priority = other.m_priority;
      m_confidence = other.m_confidence;
      m_rationale = other.m_rationale;
      m_conditions = other.m_conditions;
      m_staticParams = other.m_staticParams;
      m_dynamicParams = other.m_dynamicParams;
   }
   return *this;
}

// ----------------------------------------------------------------------------

SelectionRule& SelectionRule::When( ConditionFunc condition )
{
   m_conditions.push_back( condition );
   return *this;
}

// ----------------------------------------------------------------------------

SelectionRule& SelectionRule::WhenSNRAbove( double threshold )
{
   return When( [threshold]( const RegionStatistics& s ) {
      return s.snrEstimate > threshold;
   } );
}

// ----------------------------------------------------------------------------

SelectionRule& SelectionRule::WhenSNRBelow( double threshold )
{
   return When( [threshold]( const RegionStatistics& s ) {
      return s.snrEstimate < threshold;
   } );
}

// ----------------------------------------------------------------------------

SelectionRule& SelectionRule::WhenMedianAbove( double threshold )
{
   return When( [threshold]( const RegionStatistics& s ) {
      return s.median > threshold;
   } );
}

// ----------------------------------------------------------------------------

SelectionRule& SelectionRule::WhenMedianBelow( double threshold )
{
   return When( [threshold]( const RegionStatistics& s ) {
      return s.median < threshold;
   } );
}

// ----------------------------------------------------------------------------

SelectionRule& SelectionRule::WhenDynamicRangeAbove( double threshold )
{
   return When( [threshold]( const RegionStatistics& s ) {
      return s.dynamicRange > threshold;
   } );
}

// ----------------------------------------------------------------------------

SelectionRule& SelectionRule::WhenClippingAbove( double threshold )
{
   return When( [threshold]( const RegionStatistics& s ) {
      return s.clippingPct > threshold;
   } );
}

// ----------------------------------------------------------------------------

SelectionRule& SelectionRule::UseAlgorithm( AlgorithmType algo )
{
   m_algorithm = algo;
   return *this;
}

// ----------------------------------------------------------------------------

SelectionRule& SelectionRule::WithParameter( const IsoString& name, double value )
{
   m_staticParams[name] = value;
   return *this;
}

// ----------------------------------------------------------------------------

SelectionRule& SelectionRule::WithDynamicParameter( const IsoString& name, ParameterFunc func )
{
   m_dynamicParams[name] = func;
   return *this;
}

// ----------------------------------------------------------------------------

SelectionRule& SelectionRule::WithConfidence( double conf )
{
   m_confidence = conf;
   return *this;
}

// ----------------------------------------------------------------------------

SelectionRule& SelectionRule::WithRationale( const IsoString& reason )
{
   m_rationale = reason;
   return *this;
}

// ----------------------------------------------------------------------------

bool SelectionRule::Matches( const RegionStatistics& stats ) const
{
   // All conditions must be true
   for ( const auto& condition : m_conditions )
   {
      if ( !condition( stats ) )
         return false;
   }
   return true;
}

// ----------------------------------------------------------------------------

AlgorithmRecommendation SelectionRule::GetRecommendation( const RegionStatistics& stats ) const
{
   AlgorithmRecommendation rec;
   rec.algorithm = m_algorithm;
   rec.confidence = m_confidence;
   rec.rationale = m_rationale;

   // Copy static parameters
   rec.parameters = m_staticParams;

   // Compute dynamic parameters
   for ( const auto& pair : m_dynamicParams )
   {
      pair.second( stats, rec );
   }

   return rec;
}

// ----------------------------------------------------------------------------
// SelectionRulesEngine Implementation
// ----------------------------------------------------------------------------

SelectionRulesEngine::SelectionRulesEngine()
{
}

// ----------------------------------------------------------------------------

void SelectionRulesEngine::AddRule( const SelectionRule& rule )
{
   m_rules[rule.TargetRegion()].push_back( rule );
}

// ----------------------------------------------------------------------------

void SelectionRulesEngine::ClearRules( RegionClass region )
{
   m_rules.erase( region );
}

// ----------------------------------------------------------------------------

void SelectionRulesEngine::ClearAllRules()
{
   m_rules.clear();
}

// ----------------------------------------------------------------------------

AlgorithmRecommendation SelectionRulesEngine::GetRecommendation( RegionClass region,
                                                                  const RegionStatistics& stats ) const
{
   auto all = GetAllRecommendations( region, stats );

   if ( all.empty() )
      return GetDefaultRecommendation( region );

   return all[0];  // Highest priority
}

// ----------------------------------------------------------------------------

std::vector<AlgorithmRecommendation> SelectionRulesEngine::GetAllRecommendations(
   RegionClass region, const RegionStatistics& stats ) const
{
   std::vector<AlgorithmRecommendation> result;

   try
   {
      // Find rules for this region
      auto it = m_rules.find( region );
      if ( it != m_rules.end() )
      {
         // Evaluate each rule and collect matching recommendations
         for ( const auto& rule : it->second )
         {
            if ( rule.Matches( stats ) )
            {
               AlgorithmRecommendation rec = rule.GetRecommendation( stats );
               // rationale is already set by the rule - keep it as-is
               result.push_back( rec );
            }
         }
      }

      // If no rules matched, return default
      if ( result.empty() )
      {
         result.push_back( GetDefaultRecommendation( region ) );
         return result;
      }

      // Sort by priority (rules added first have implicit higher priority within same confidence)
      // Higher confidence first
      std::sort( result.begin(), result.end(),
                 []( const AlgorithmRecommendation& a, const AlgorithmRecommendation& b ) {
                    return a.confidence > b.confidence;
                 } );
   }
   catch ( ... )
   {
      // If anything goes wrong, return default recommendation
      result.clear();
      result.push_back( GetDefaultRecommendation( region ) );
   }

   return result;
}

// ----------------------------------------------------------------------------

SelectionRulesEngine SelectionRulesEngine::CreateDefaultRules()
{
   SelectionRulesEngine engine;

   auto allRules = DefaultRules::GetAllRules();
   for ( const auto& rule : allRules )
   {
      engine.AddRule( rule );
   }

   return engine;
}

// ----------------------------------------------------------------------------

AlgorithmRecommendation SelectionRulesEngine::GetDefaultRecommendation( RegionClass region ) const
{
   // Fallback recommendations when no rules match
   // Conservative defaults to avoid blowing out highlights
   AlgorithmRecommendation rec;
   rec.confidence = 0.5;

   switch ( region )
   {
   case RegionClass::Background:
      rec.algorithm = AlgorithmType::MTF;
      rec.SetParameter( "midtones", 0.20 );
      rec.SetRationale( "Default background stretch" );
      break;

   case RegionClass::BrightCompact:
      rec.algorithm = AlgorithmType::ArcSinh;
      rec.SetParameter( "softness", 0.05 );
      rec.SetRationale( "Default bright compact protection" );
      break;

   case RegionClass::FaintCompact:
      rec.algorithm = AlgorithmType::GHS;
      rec.SetParameter( "stretchFactor", 0.4 );
      rec.SetParameter( "highlightProtection", 0.85 );
      rec.SetRationale( "Default faint compact stretch" );
      break;

   case RegionClass::BrightExtended:
      rec.algorithm = AlgorithmType::GHS;
      rec.SetParameter( "stretchFactor", 0.55 );
      rec.SetParameter( "highlightProtection", 0.75 );
      rec.SetRationale( "Default bright extended stretch" );
      break;

   case RegionClass::DarkExtended:
      rec.algorithm = AlgorithmType::MTF;
      rec.SetParameter( "midtones", 0.15 );
      rec.SetRationale( "Default dark extended preservation" );
      break;

   case RegionClass::Artifact:
      rec.algorithm = AlgorithmType::MTF;
      rec.SetParameter( "midtones", 0.05 );
      rec.confidence = 0.4;
      rec.SetRationale( "Artifact suppression" );
      break;

   case RegionClass::StarHalo:
      rec.algorithm = AlgorithmType::GHS;
      rec.SetParameter( "stretchFactor", 0.6 );
      rec.SetParameter( "highlightProtection", 0.75 );
      rec.SetRationale( "Default star halo gradient-preserving stretch" );
      break;

   default:
      rec.algorithm = AlgorithmType::MTF;
      rec.SetParameter( "midtones", 0.18 );
      rec.confidence = 0.3;
      rec.SetRationale( "Generic fallback" );
      break;
   }

   return rec;
}


// ----------------------------------------------------------------------------

String SelectionRulesEngine::ToJSON() const
{
   // Simplified serialization
   IsoString json = "{\n  \"rules\": [\n";

   bool first = true;
   for ( const auto& regionPair : m_rules )
   {
      for ( const auto& rule : regionPair.second )
      {
         if ( !first ) json += ",\n";
         first = false;

         json += IsoString().Format(
            "    {\"name\": \"%s\", \"region\": \"%s\", \"priority\": %.2f}",
            rule.Name().c_str(),
            RegionClassToString( rule.TargetRegion() ).c_str(),
            rule.Priority() );
      }
   }

   json += "\n  ]\n}";
   return String( json );
}

// ----------------------------------------------------------------------------
// Default Rules Implementation
// ----------------------------------------------------------------------------

namespace DefaultRules
{

std::vector<SelectionRule> GetAllRules()
{
   std::vector<SelectionRule> rules;

   auto add = [&rules]( const std::vector<SelectionRule>& r ) {
      rules.insert( rules.end(), r.begin(), r.end() );
   };

   add( GetBackgroundRules() );
   add( GetBrightCompactRules() );
   add( GetFaintCompactRules() );
   add( GetBrightExtendedRules() );
   add( GetDarkExtendedRules() );
   add( GetArtifactRules() );
   add( GetStarHaloRules() );

   return rules;
}

// ----------------------------------------------------------------------------

std::vector<SelectionRule> GetBackgroundRules()
{
   std::vector<SelectionRule> rules;

   // High SNR background - use SAS for clean stretch
   rules.push_back(
      SelectionRule( "Background_HighSNR", RegionClass::Background, AlgorithmType::SAS, 10.0 )
         .WhenSNRAbove( 15.0 )
         .WhenMedianBelow( 0.1 )
         .WithDynamicParameter( "iterations", []( const RegionStatistics& s, AlgorithmRecommendation& r ) {
            r.parameters["iterations"] = ParameterTuning::ComputeSASIterations( s );
         } )
         .WithConfidence( 0.9 )
         .WithRationale( "High SNR background benefits from statistical adaptive stretch" )
   );

   // Low SNR background - gentle MTF
   rules.push_back(
      SelectionRule( "Background_LowSNR", RegionClass::Background, AlgorithmType::MTF, 8.0 )
         .WhenSNRBelow( 8.0 )
         .WhenMedianBelow( 0.1 )
         .WithDynamicParameter( "midtones", []( const RegionStatistics& s, AlgorithmRecommendation& r ) {
            r.parameters["midtones"] = ParameterTuning::ComputeMTFMidtones( s );
         } )
         .WithConfidence( 0.85 )
         .WithRationale( "Low SNR background needs gentle MTF to avoid amplifying noise" )
   );

   // Default background
   rules.push_back(
      SelectionRule( "Background_Default", RegionClass::Background, AlgorithmType::MTF, 1.0 )
         .WithParameter( "midtones", 0.25 )
         .WithConfidence( 0.7 )
         .WithRationale( "Standard background stretch" )
   );

   return rules;
}

// ----------------------------------------------------------------------------

std::vector<SelectionRule> GetBrightCompactRules()
{
   std::vector<SelectionRule> rules;

   // Very bright / saturated - strong ArcSinh protection
   rules.push_back(
      SelectionRule( "BrightCompact_VeryBright", RegionClass::BrightCompact, AlgorithmType::ArcSinh, 10.0 )
         .WhenMedianAbove( 0.9 )
         .WithParameter( "softness", 0.02 )
         .WithConfidence( 0.95 )
         .WithRationale( "Very bright/saturated stars need maximum HDR compression" )
   );

   // High dynamic range bright stars
   rules.push_back(
      SelectionRule( "BrightCompact_HighDR", RegionClass::BrightCompact, AlgorithmType::ArcSinh, 9.0 )
         .WhenDynamicRangeAbove( 3.0 )
         .WithDynamicParameter( "softness", []( const RegionStatistics& s, AlgorithmRecommendation& r ) {
            r.parameters["softness"] = ParameterTuning::ComputeArcSinhSoftness( s );
         } )
         .WithConfidence( 0.9 )
         .WithRationale( "High dynamic range requires adaptive softness" )
   );

   // Stars with clipping risk
   rules.push_back(
      SelectionRule( "BrightCompact_ClipRisk", RegionClass::BrightCompact, AlgorithmType::ArcSinh, 8.0 )
         .WhenClippingAbove( 1.0 )
         .WithParameter( "softness", 0.15 )
         .WithConfidence( 0.9 )
         .WithRationale( "Existing clipping requires conservative stretch" )
   );

   // Default bright compact
   rules.push_back(
      SelectionRule( "BrightCompact_Default", RegionClass::BrightCompact, AlgorithmType::ArcSinh, 1.0 )
         .WithParameter( "softness", 0.1 )
         .WithConfidence( 0.8 )
         .WithRationale( "Standard bright compact protection" )
   );

   return rules;
}

// ----------------------------------------------------------------------------

std::vector<SelectionRule> GetFaintCompactRules()
{
   std::vector<SelectionRule> rules;

   // Brighter faint compact objects
   rules.push_back(
      SelectionRule( "FaintCompact_Bright", RegionClass::FaintCompact, AlgorithmType::GHS, 10.0 )
         .WhenMedianAbove( 0.5 )
         .WithParameter( "stretchFactor", 0.3 )
         .WithParameter( "highlightProtection", 0.8 )
         .WithConfidence( 0.85 )
         .WithRationale( "Brighter faint compact objects need highlight protection" )
   );

   // Good SNR faint compact
   rules.push_back(
      SelectionRule( "FaintCompact_GoodSNR", RegionClass::FaintCompact, AlgorithmType::GHS, 9.0 )
         .WhenSNRAbove( 8.0 )
         .WithParameter( "stretchFactor", 0.5 )
         .WithParameter( "highlightProtection", 0.75 )
         .WithConfidence( 0.85 )
         .WithRationale( "Good SNR faint compact can use moderate stretch" )
   );

   // Default faint compact
   rules.push_back(
      SelectionRule( "FaintCompact_Default", RegionClass::FaintCompact, AlgorithmType::GHS, 1.0 )
         .WithDynamicParameter( "stretchFactor", []( const RegionStatistics& s, AlgorithmRecommendation& r ) {
            r.parameters["stretchFactor"] = ParameterTuning::ComputeGHSStretchFactor( s );
         } )
         .WithDynamicParameter( "highlightProtection", []( const RegionStatistics& s, AlgorithmRecommendation& r ) {
            r.parameters["highlightProtection"] = ParameterTuning::ComputeGHSHighlightProtection( s );
         } )
         .WithConfidence( 0.75 )
         .WithRationale( "Standard faint compact stretch" )
   );

   return rules;
}

// ----------------------------------------------------------------------------

std::vector<SelectionRule> GetBrightExtendedRules()
{
   std::vector<SelectionRule> rules;

   // High SNR bright extended - RNC for color preservation
   rules.push_back(
      SelectionRule( "BrightExtended_HighSNR", RegionClass::BrightExtended, AlgorithmType::RNC, 10.0 )
         .WhenSNRAbove( 20.0 )
         .WhenMedianAbove( 0.3 )
         .WithConfidence( 0.9 )
         .WithRationale( "High SNR bright extended benefits from color-preserving stretch" )
   );

   // Wide dynamic range - GHS
   rules.push_back(
      SelectionRule( "BrightExtended_WideDR", RegionClass::BrightExtended, AlgorithmType::GHS, 9.0 )
         .WhenDynamicRangeAbove( 2.5 )
         .WithDynamicParameter( "stretchFactor", []( const RegionStatistics& s, AlgorithmRecommendation& r ) {
            r.parameters["stretchFactor"] = ParameterTuning::ComputeGHSStretchFactor( s );
         } )
         .WithConfidence( 0.85 )
         .WithRationale( "Wide dynamic range needs GHS flexibility" )
   );

   // Bright core (galaxy core / AGN) - ArcSinh for HDR
   rules.push_back(
      SelectionRule( "BrightExtended_BrightCore", RegionClass::BrightExtended, AlgorithmType::ArcSinh, 8.0 )
         .WhenMedianAbove( 0.8 )
         .WithParameter( "softness", 0.2 )
         .WithConfidence( 0.9 )
         .WithRationale( "Very bright extended core needs HDR protection" )
   );

   // Default bright extended
   rules.push_back(
      SelectionRule( "BrightExtended_Default", RegionClass::BrightExtended, AlgorithmType::GHS, 1.0 )
         .WithParameter( "stretchFactor", 0.65 )
         .WithParameter( "highlightProtection", 0.75 )
         .WithConfidence( 0.75 )
         .WithRationale( "Standard bright extended stretch" )
   );

   return rules;
}

// ----------------------------------------------------------------------------

std::vector<SelectionRule> GetDarkExtendedRules()
{
   std::vector<SelectionRule> rules;

   // Very dark - preserve darkness
   rules.push_back(
      SelectionRule( "DarkExtended_VeryDark", RegionClass::DarkExtended, AlgorithmType::MTF, 10.0 )
         .WhenMedianBelow( 0.03 )
         .WithParameter( "midtones", 0.15 )
         .WithConfidence( 0.85 )
         .WithRationale( "Very dark extended needs shadow preservation" )
   );

   // Dark with some detail
   rules.push_back(
      SelectionRule( "DarkExtended_WithDetail", RegionClass::DarkExtended, AlgorithmType::GHS, 9.0 )
         .WhenSNRAbove( 8.0 )
         .WhenMedianBelow( 0.1 )
         .WithParameter( "stretchFactor", 0.4 )
         .WithParameter( "highlightProtection", 0.8 )
         .WithConfidence( 0.8 )
         .WithRationale( "Dark extended with detail can use gentle stretch" )
   );

   // Default dark extended
   rules.push_back(
      SelectionRule( "DarkExtended_Default", RegionClass::DarkExtended, AlgorithmType::MTF, 1.0 )
         .WithParameter( "midtones", 0.2 )
         .WithConfidence( 0.7 )
         .WithRationale( "Standard dark extended preservation" )
   );

   return rules;
}

// ----------------------------------------------------------------------------

std::vector<SelectionRule> GetArtifactRules()
{
   std::vector<SelectionRule> rules;

   // Bright artifact - aggressive rejection
   rules.push_back(
      SelectionRule( "Artifact_Bright", RegionClass::Artifact, AlgorithmType::MTF, 10.0 )
         .WhenMedianAbove( 0.3 )
         .WithParameter( "midtones", 0.05 )
         .WithConfidence( 0.9 )
         .WithRationale( "Bright artifact - aggressive rejection" )
   );

   // Low SNR noise artifact - SAS for noise awareness
   rules.push_back(
      SelectionRule( "Artifact_LowSNR", RegionClass::Artifact, AlgorithmType::SAS, 9.0 )
         .WhenSNRBelow( 5.0 )
         .WithDynamicParameter( "iterations", []( const RegionStatistics& s, AlgorithmRecommendation& r ) {
            r.parameters["iterations"] = ParameterTuning::ComputeSASIterations( s );
         } )
         .WithConfidence( 0.85 )
         .WithRationale( "Low SNR artifact region benefits from statistical adaptive smoothing" )
   );

   // Default artifact - aggressive suppression
   rules.push_back(
      SelectionRule( "Artifact_Default", RegionClass::Artifact, AlgorithmType::MTF, 1.0 )
         .WithParameter( "midtones", 0.05 )
         .WithConfidence( 0.85 )
         .WithRationale( "Artifact suppression with aggressive clipping" )
   );

   return rules;
}

// ----------------------------------------------------------------------------

std::vector<SelectionRule> GetStarHaloRules()
{
   std::vector<SelectionRule> rules;

   // Bright star halo - moderate GHS to preserve radial gradient
   rules.push_back(
      SelectionRule( "StarHalo_Bright", RegionClass::StarHalo, AlgorithmType::GHS, 10.0 )
         .WhenSNRAbove( 15.0 )
         .WhenMedianAbove( 0.2 )
         .WithParameter( "stretchFactor", 0.7 )
         .WithParameter( "highlightProtection", 0.8 )
         .WithConfidence( 0.9 )
         .WithRationale( "Bright star halo - moderate stretch preserving radial gradient falloff" )
   );

   // Faint star halo - gentle treatment to avoid noise amplification
   rules.push_back(
      SelectionRule( "StarHalo_Faint", RegionClass::StarHalo, AlgorithmType::GHS, 9.0 )
         .WhenSNRBelow( 8.0 )
         .WhenMedianBelow( 0.1 )
         .WithParameter( "stretchFactor", 0.5 )
         .WithParameter( "highlightProtection", 0.7 )
         .WithConfidence( 0.8 )
         .WithRationale( "Faint star halo - gentle stretch to avoid amplifying noise in diffuse glow" )
   );

   // Default star halo
   rules.push_back(
      SelectionRule( "StarHalo_Default", RegionClass::StarHalo, AlgorithmType::GHS, 1.0 )
         .WithParameter( "stretchFactor", 0.6 )
         .WithParameter( "highlightProtection", 0.75 )
         .WithConfidence( 0.75 )
         .WithRationale( "Standard star halo gradient-preserving stretch" )
   );

   return rules;
}

} // namespace DefaultRules

// ----------------------------------------------------------------------------
// Parameter Tuning Implementation
// ----------------------------------------------------------------------------

namespace ParameterTuning
{

double ComputeMTFMidtones( const RegionStatistics& stats )
{
   // Darker regions need higher midtones (more stretch)
   // More conservative range: 0.15 to 0.35
   double median = stats.median;

   if ( median < 0.02 )
      return 0.35;   // Very faint - moderate stretch
   else if ( median < 0.05 )
      return 0.30;
   else if ( median < 0.1 )
      return 0.25;
   else if ( median < 0.3 )
      return 0.20;   // Mid-brightness - gentle
   else
      return 0.15;   // Bright - very gentle
}

// ----------------------------------------------------------------------------

double ComputeArcSinhSoftness( const RegionStatistics& stats )
{
   // Higher dynamic range or brightness needs lower softness (more compression)
   // Lower values = stronger highlight compression
   // Range: 0.02 to 0.3
   double dr = stats.dynamicRange;
   double median = stats.median;

   // Very bright regions need strong compression regardless of DR
   if ( median > 0.7 )
      return 0.02;   // Very strong compression for near-saturated
   else if ( median > 0.5 )
      return 0.05;   // Strong compression for bright

   // Otherwise base on dynamic range
   if ( dr > 4.0 )
      return 0.05;
   else if ( dr > 3.0 )
      return 0.10;
   else if ( dr > 2.0 )
      return 0.15;
   else if ( dr > 1.0 )
      return 0.20;
   else
      return 0.25;
}

// ----------------------------------------------------------------------------

double ComputeGHSStretchFactor( const RegionStatistics& stats )
{
   // Fainter regions need more stretch, bright regions need less
   // More conservative range: 0.2 to 1.5
   double median = stats.median;

   if ( median < 0.02 )
      return 1.5;    // Very faint - moderate stretch
   else if ( median < 0.05 )
      return 1.2;
   else if ( median < 0.1 )
      return 0.9;
   else if ( median < 0.2 )
      return 0.6;
   else if ( median < 0.4 )
      return 0.4;    // Mid-bright - gentle
   else if ( median < 0.6 )
      return 0.25;   // Bright - very gentle
   else
      return 0.15;   // Very bright - minimal stretch
}

// ----------------------------------------------------------------------------

double ComputeGHSLocalIntensity( const RegionStatistics& stats )
{
   // Higher SNR allows more local intensity
   // Range: 0.0 to 1.0
   double snr = stats.snrEstimate;

   if ( snr > 30.0 )
      return 0.8;
   else if ( snr > 20.0 )
      return 0.6;
   else if ( snr > 10.0 )
      return 0.4;
   else
      return 0.2;
}

// ----------------------------------------------------------------------------

double ComputeGHSSymmetryPoint( const RegionStatistics& stats )
{
   // Symmetry point near median for balanced stretch
   // Range: 0.0 to 1.0
   return std::max( 0.1, std::min( 0.9, stats.median ) );
}

// ----------------------------------------------------------------------------

double ComputeGHSHighlightProtection( const RegionStatistics& stats )
{
   // More protection if there's clipping risk or bright data
   // Higher values = more protection
   // Range: 0.5 to 0.98
   double median = stats.median;

   // Always protect bright regions aggressively
   if ( median > 0.6 || stats.clippedHighPct > 0.5 )
      return 0.98;   // Maximum protection
   else if ( median > 0.4 || stats.clippedHighPct > 0.1 )
      return 0.95;
   else if ( stats.p95 > 0.9 )
      return 0.90;
   else if ( stats.p95 > 0.7 || median > 0.3 )
      return 0.80;
   else if ( stats.p95 > 0.5 )
      return 0.70;
   else
      return 0.50;   // Base protection for faint data
}

// ----------------------------------------------------------------------------

double ComputeLogScale( const RegionStatistics& stats )
{
   // Fainter data needs higher scale
   // Range: 10 to 1000
   double median = stats.median;

   if ( median < 0.01 )
      return 500.0;
   else if ( median < 0.03 )
      return 300.0;
   else if ( median < 0.05 )
      return 150.0;
   else if ( median < 0.1 )
      return 100.0;
   else
      return 50.0;
}

// ----------------------------------------------------------------------------

double ComputeLumptonQ( const RegionStatistics& stats )
{
   // Q parameter based on SNR and dynamic range
   // Range: 1 to 20
   double snr = stats.snrEstimate;
   double dr = stats.dynamicRange;

   double baseQ = 8.0;

   // Adjust for SNR
   if ( snr > 20.0 )
      baseQ += 4.0;
   else if ( snr < 5.0 )
      baseQ -= 3.0;

   // Adjust for dynamic range
   if ( dr > 3.0 )
      baseQ += 2.0;

   return std::max( 1.0, std::min( 20.0, baseQ ) );
}

// ----------------------------------------------------------------------------

int ComputeSASIterations( const RegionStatistics& stats )
{
   // More iterations for noisier data
   // Range: 1 to 10
   double snr = stats.snrEstimate;

   if ( snr < 5.0 )
      return 8;
   else if ( snr < 10.0 )
      return 5;
   else if ( snr < 20.0 )
      return 3;
   else
      return 2;
}

} // namespace ParameterTuning

// ----------------------------------------------------------------------------

} // namespace pcl
