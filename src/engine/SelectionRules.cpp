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
               // Build rationale safely using IsoString (no ABI issues)
               IsoString ruleName = rule.Name();
               IsoString rationale = rec.rationale;
               rec.rationale = ruleName + ": " + rationale;
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
      rec.SetParameter( "midtones", 0.20 );  // Conservative
      rec.SetRationale( "Default background stretch" );
      break;

   // Star classes (1-4)
   case RegionClass::StarBright:
   case RegionClass::StarSaturated:
      rec.algorithm = AlgorithmType::ArcSinh;
      rec.SetParameter( "softness", 0.05 );  // Strong compression for bright stars
      rec.SetRationale( "Default bright star protection" );
      break;

   case RegionClass::StarMedium:
      rec.algorithm = AlgorithmType::GHS;
      rec.SetParameter( "stretchFactor", 0.3 );  // Gentle
      rec.SetParameter( "highlightProtection", 0.9 );
      rec.SetRationale( "Default medium star stretch" );
      break;

   case RegionClass::StarFaint:
      rec.algorithm = AlgorithmType::GHS;
      rec.SetParameter( "stretchFactor", 0.6 );
      rec.SetParameter( "highlightProtection", 0.7 );
      rec.SetRationale( "Default faint star enhancement" );
      break;

   // Nebula classes (5-8)
   case RegionClass::NebulaEmission:
      rec.algorithm = AlgorithmType::GHS;
      rec.SetParameter( "stretchFactor", 0.65 );
      rec.SetParameter( "highlightProtection", 0.75 );
      rec.SetRationale( "Emission nebula enhancement" );
      break;

   case RegionClass::NebulaReflection:
      rec.algorithm = AlgorithmType::GHS;
      rec.SetParameter( "stretchFactor", 0.6 );
      rec.SetParameter( "highlightProtection", 0.75 );
      rec.SetRationale( "Reflection nebula blue enhancement" );
      break;

   case RegionClass::NebulaDark:
      rec.algorithm = AlgorithmType::MTF;
      rec.SetParameter( "midtones", 0.15 );  // Very gentle to preserve darkness
      rec.SetRationale( "Dark nebula preservation" );
      break;

   case RegionClass::NebulaPlanetary:
      rec.algorithm = AlgorithmType::ArcSinh;
      rec.SetParameter( "softness", 0.08 );  // Preserve shell structure
      rec.SetRationale( "Planetary nebula shell enhancement" );
      break;

   // Galaxy classes (9-12)
   case RegionClass::GalaxySpiral:
      rec.algorithm = AlgorithmType::GHS;
      rec.SetParameter( "stretchFactor", 0.4 );
      rec.SetParameter( "highlightProtection", 0.85 );
      rec.SetRationale( "Default spiral galaxy arm stretch" );
      break;

   case RegionClass::GalaxyElliptical:
      rec.algorithm = AlgorithmType::GHS;
      rec.SetParameter( "stretchFactor", 0.5 );
      rec.SetParameter( "highlightProtection", 0.8 );
      rec.SetRationale( "Default elliptical galaxy stretch" );
      break;

   case RegionClass::GalaxyIrregular:
      rec.algorithm = AlgorithmType::GHS;
      rec.SetParameter( "stretchFactor", 0.55 );
      rec.SetParameter( "highlightProtection", 0.75 );
      rec.SetRationale( "Default irregular galaxy stretch" );
      break;

   case RegionClass::GalaxyCore:
      rec.algorithm = AlgorithmType::ArcSinh;
      rec.SetParameter( "softness", 0.05 );  // Strong compression for cores
      rec.SetRationale( "Default galaxy core protection" );
      break;

   // Structural classes (13-15)
   case RegionClass::DustLane:
      rec.algorithm = AlgorithmType::MTF;  // Simple stretch for dust
      rec.SetParameter( "midtones", 0.15 );  // Very gentle
      rec.SetRationale( "Default dust lane treatment" );
      break;

   case RegionClass::StarClusterOpen:
      rec.algorithm = AlgorithmType::GHS;
      rec.SetParameter( "stretchFactor", 0.35 );
      rec.SetParameter( "highlightProtection", 0.85 );
      rec.SetRationale( "Open cluster balanced stretch" );
      break;

   case RegionClass::StarClusterGlobular:
      rec.algorithm = AlgorithmType::GHS;
      rec.SetParameter( "stretchFactor", 0.3 );
      rec.SetParameter( "highlightProtection", 0.9 );  // Protect dense cores
      rec.SetRationale( "Globular cluster balanced stretch" );
      break;

   // Artifact classes (16-20) - minimal processing
   case RegionClass::ArtifactHotPixel:
   case RegionClass::ArtifactSatellite:
   case RegionClass::ArtifactDiffraction:
   case RegionClass::ArtifactGradient:
   case RegionClass::ArtifactNoise:
      rec.algorithm = AlgorithmType::MTF;
      rec.SetParameter( "midtones", 0.2 );  // Conservative
      rec.confidence = 0.3;
      rec.SetRationale( "Artifact region - minimal processing" );
      break;

   default:
      rec.algorithm = AlgorithmType::MTF;
      rec.SetParameter( "midtones", 0.18 );  // Conservative
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

bool SelectionRulesEngine::FromJSON( const String& json )
{
   // Note: Full JSON parsing would require a JSON library
   // This is a placeholder for future implementation
   return false;
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
   add( GetStarBrightRules() );
   add( GetStarMediumRules() );
   add( GetStarFaintRules() );
   add( GetNebulaEmissionRules() );
   add( GetNebulaDarkRules() );
   add( GetGalaxyCoreRules() );
   add( GetGalaxySpiralRules() );
   add( GetDustLaneRules() );
   add( GetStarClusterRules() );

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

std::vector<SelectionRule> GetStarBrightRules()
{
   std::vector<SelectionRule> rules;

   // Very bright stars - strong protection
   rules.push_back(
      SelectionRule( "StarBright_VeryBright", RegionClass::StarBright, AlgorithmType::ArcSinh, 10.0 )
         .WhenMedianAbove( 0.9 )
         .WithParameter( "softness", 0.2 )
         .WithConfidence( 0.95 )
         .WithRationale( "Very bright star cores need strong HDR protection" )
   );

   // High dynamic range stars
   rules.push_back(
      SelectionRule( "StarBright_HighDR", RegionClass::StarBright, AlgorithmType::ArcSinh, 9.0 )
         .WhenDynamicRangeAbove( 3.0 )
         .WithDynamicParameter( "softness", []( const RegionStatistics& s, AlgorithmRecommendation& r ) {
            r.parameters["softness"] = ParameterTuning::ComputeArcSinhSoftness( s );
         } )
         .WithConfidence( 0.9 )
         .WithRationale( "High dynamic range requires adaptive softness" )
   );

   // Stars with clipping risk
   rules.push_back(
      SelectionRule( "StarBright_ClipRisk", RegionClass::StarBright, AlgorithmType::ArcSinh, 8.0 )
         .WhenClippingAbove( 1.0 )
         .WithParameter( "softness", 0.15 )
         .WithConfidence( 0.9 )
         .WithRationale( "Existing clipping requires conservative stretch" )
   );

   // Default bright star
   rules.push_back(
      SelectionRule( "StarBright_Default", RegionClass::StarBright, AlgorithmType::ArcSinh, 1.0 )
         .WithParameter( "softness", 0.3 )
         .WithConfidence( 0.8 )
         .WithRationale( "Standard bright star protection" )
   );

   return rules;
}

// ----------------------------------------------------------------------------

std::vector<SelectionRule> GetStarMediumRules()
{
   std::vector<SelectionRule> rules;

   // Medium stars - controlled GHS
   rules.push_back(
      SelectionRule( "StarMedium_Bright", RegionClass::StarMedium, AlgorithmType::GHS, 10.0 )
         .WhenMedianAbove( 0.5 )
         .WithParameter( "stretchFactor", 0.3 )
         .WithParameter( "highlightProtection", 0.8 )
         .WithConfidence( 0.85 )
         .WithRationale( "Medium stars need highlight protection" )
   );

   // Default medium star
   rules.push_back(
      SelectionRule( "StarMedium_Default", RegionClass::StarMedium, AlgorithmType::GHS, 1.0 )
         .WithDynamicParameter( "stretchFactor", []( const RegionStatistics& s, AlgorithmRecommendation& r ) {
            r.parameters["stretchFactor"] = ParameterTuning::ComputeGHSStretchFactor( s );
         } )
         .WithDynamicParameter( "highlightProtection", []( const RegionStatistics& s, AlgorithmRecommendation& r ) {
            r.parameters["highlightProtection"] = ParameterTuning::ComputeGHSHighlightProtection( s );
         } )
         .WithConfidence( 0.8 )
         .WithRationale( "Balanced medium star stretch" )
   );

   return rules;
}

// ----------------------------------------------------------------------------

std::vector<SelectionRule> GetStarFaintRules()
{
   std::vector<SelectionRule> rules;

   // Faint stars with good SNR
   rules.push_back(
      SelectionRule( "StarFaint_GoodSNR", RegionClass::StarFaint, AlgorithmType::GHS, 10.0 )
         .WhenSNRAbove( 8.0 )
         .WithParameter( "stretchFactor", 0.6 )
         .WithParameter( "highlightProtection", 0.7 )
         .WithConfidence( 0.85 )
         .WithRationale( "Faint stars with good SNR can use moderate stretch" )
   );

   // Default faint star
   rules.push_back(
      SelectionRule( "StarFaint_Default", RegionClass::StarFaint, AlgorithmType::GHS, 1.0 )
         .WithDynamicParameter( "stretchFactor", []( const RegionStatistics& s, AlgorithmRecommendation& r ) {
            r.parameters["stretchFactor"] = ParameterTuning::ComputeGHSStretchFactor( s );
         } )
         .WithConfidence( 0.75 )
         .WithRationale( "Standard faint star enhancement" )
   );

   return rules;
}

// ----------------------------------------------------------------------------

std::vector<SelectionRule> GetNebulaEmissionRules()
{
   std::vector<SelectionRule> rules;

   // High SNR emission nebula - RNC for color
   rules.push_back(
      SelectionRule( "NebulaEmission_HighSNR", RegionClass::NebulaEmission, AlgorithmType::RNC, 10.0 )
         .WhenSNRAbove( 20.0 )
         .WhenMedianAbove( 0.3 )
         .WithConfidence( 0.9 )
         .WithRationale( "High SNR emission nebula benefits from color-preserving stretch" )
   );

   // Wide dynamic range nebula - GHS
   rules.push_back(
      SelectionRule( "NebulaEmission_WideDR", RegionClass::NebulaEmission, AlgorithmType::GHS, 9.0 )
         .WhenDynamicRangeAbove( 2.5 )
         .WithDynamicParameter( "stretchFactor", []( const RegionStatistics& s, AlgorithmRecommendation& r ) {
            r.parameters["stretchFactor"] = ParameterTuning::ComputeGHSStretchFactor( s );
         } )
         .WithConfidence( 0.85 )
         .WithRationale( "Wide dynamic range needs GHS flexibility" )
   );

   // Default emission nebula
   rules.push_back(
      SelectionRule( "NebulaEmission_Default", RegionClass::NebulaEmission, AlgorithmType::GHS, 1.0 )
         .WithParameter( "stretchFactor", 1.0 )
         .WithConfidence( 0.75 )
         .WithRationale( "Standard emission nebula stretch" )
   );

   return rules;
}

// ----------------------------------------------------------------------------

std::vector<SelectionRule> GetNebulaDarkRules()
{
   std::vector<SelectionRule> rules;

   // Very dark nebula - preserve darkness
   rules.push_back(
      SelectionRule( "NebulaDark_VeryDark", RegionClass::NebulaDark, AlgorithmType::MTF, 10.0 )
         .WhenMedianBelow( 0.03 )
         .WithParameter( "midtones", 0.15 )
         .WithConfidence( 0.85 )
         .WithRationale( "Very dark nebula needs shadow preservation" )
   );

   // Dark nebula with some detail
   rules.push_back(
      SelectionRule( "NebulaDark_WithDetail", RegionClass::NebulaDark, AlgorithmType::GHS, 9.0 )
         .WhenSNRAbove( 8.0 )
         .WhenMedianBelow( 0.1 )
         .WithParameter( "stretchFactor", 0.4 )
         .WithParameter( "highlightProtection", 0.8 )
         .WithConfidence( 0.8 )
         .WithRationale( "Dark nebula with detail can use gentle stretch" )
   );

   // Default dark nebula
   rules.push_back(
      SelectionRule( "NebulaDark_Default", RegionClass::NebulaDark, AlgorithmType::MTF, 1.0 )
         .WithParameter( "midtones", 0.2 )
         .WithConfidence( 0.7 )
         .WithRationale( "Standard dark nebula preservation" )
   );

   return rules;
}

// ----------------------------------------------------------------------------

std::vector<SelectionRule> GetGalaxyCoreRules()
{
   std::vector<SelectionRule> rules;

   // Very bright core - similar to star core
   rules.push_back(
      SelectionRule( "GalaxyCore_VeryBright", RegionClass::GalaxyCore, AlgorithmType::ArcSinh, 10.0 )
         .WhenMedianAbove( 0.8 )
         .WithParameter( "softness", 0.2 )
         .WithConfidence( 0.9 )
         .WithRationale( "Very bright galaxy core needs HDR protection" )
   );

   // Default galaxy core
   rules.push_back(
      SelectionRule( "GalaxyCore_Default", RegionClass::GalaxyCore, AlgorithmType::ArcSinh, 1.0 )
         .WithDynamicParameter( "softness", []( const RegionStatistics& s, AlgorithmRecommendation& r ) {
            r.parameters["softness"] = ParameterTuning::ComputeArcSinhSoftness( s );
         } )
         .WithConfidence( 0.8 )
         .WithRationale( "Standard galaxy core protection" )
   );

   return rules;
}

// ----------------------------------------------------------------------------

std::vector<SelectionRule> GetGalaxySpiralRules()
{
   std::vector<SelectionRule> rules;

   // Good SNR spiral - Lumpton
   rules.push_back(
      SelectionRule( "GalaxySpiral_GoodSNR", RegionClass::GalaxySpiral, AlgorithmType::Lumpton, 10.0 )
         .WhenSNRAbove( 10.0 )
         .WithDynamicParameter( "Q", []( const RegionStatistics& s, AlgorithmRecommendation& r ) {
            r.parameters["Q"] = ParameterTuning::ComputeLumptonQ( s );
         } )
         .WithConfidence( 0.85 )
         .WithRationale( "Good SNR spiral galaxy uses Lumpton stretch" )
   );

   // Well-defined arms - GHS
   rules.push_back(
      SelectionRule( "GalaxySpiral_WellDefined", RegionClass::GalaxySpiral, AlgorithmType::GHS, 9.0 )
         .WhenSNRAbove( 12.0 )
         .WithDynamicParameter( "stretchFactor", []( const RegionStatistics& s, AlgorithmRecommendation& r ) {
            r.parameters["stretchFactor"] = ParameterTuning::ComputeGHSStretchFactor( s );
         } )
         .WithConfidence( 0.85 )
         .WithRationale( "Well-defined spiral arms benefit from GHS" )
   );

   // Low SNR spiral - SAS
   rules.push_back(
      SelectionRule( "GalaxySpiral_LowSNR", RegionClass::GalaxySpiral, AlgorithmType::SAS, 8.0 )
         .WhenSNRBelow( 6.0 )
         .WithDynamicParameter( "iterations", []( const RegionStatistics& s, AlgorithmRecommendation& r ) {
            r.parameters["iterations"] = ParameterTuning::ComputeSASIterations( s );
         } )
         .WithConfidence( 0.8 )
         .WithRationale( "Low SNR spiral needs noise-aware stretch" )
   );

   // Default spiral galaxy
   rules.push_back(
      SelectionRule( "GalaxySpiral_Default", RegionClass::GalaxySpiral, AlgorithmType::GHS, 1.0 )
         .WithParameter( "stretchFactor", 1.0 )
         .WithConfidence( 0.75 )
         .WithRationale( "Standard spiral galaxy stretch" )
   );

   return rules;
}

// ----------------------------------------------------------------------------

std::vector<SelectionRule> GetStarClusterRules()
{
   std::vector<SelectionRule> rules;

   // Open clusters - moderate stretch
   rules.push_back(
      SelectionRule( "StarCluster_Open", RegionClass::StarClusterOpen, AlgorithmType::GHS, 10.0 )
         .WhenSNRAbove( 10.0 )
         .WithParameter( "stretchFactor", 0.4 )
         .WithParameter( "highlightProtection", 0.85 )
         .WithConfidence( 0.85 )
         .WithRationale( "Open clusters need balanced stretch with star protection" )
   );

   // Globular clusters - strong protection for dense cores
   rules.push_back(
      SelectionRule( "StarCluster_Globular", RegionClass::StarClusterGlobular, AlgorithmType::ArcSinh, 10.0 )
         .WhenMedianAbove( 0.4 )
         .WithParameter( "softness", 0.1 )
         .WithConfidence( 0.9 )
         .WithRationale( "Dense globular cores need HDR protection" )
   );

   // Default open cluster
   rules.push_back(
      SelectionRule( "StarClusterOpen_Default", RegionClass::StarClusterOpen, AlgorithmType::GHS, 1.0 )
         .WithParameter( "stretchFactor", 0.5 )
         .WithParameter( "highlightProtection", 0.8 )
         .WithConfidence( 0.75 )
         .WithRationale( "Standard open cluster stretch" )
   );

   // Default globular cluster
   rules.push_back(
      SelectionRule( "StarClusterGlobular_Default", RegionClass::StarClusterGlobular, AlgorithmType::GHS, 1.0 )
         .WithParameter( "stretchFactor", 0.35 )
         .WithParameter( "highlightProtection", 0.9 )
         .WithConfidence( 0.75 )
         .WithRationale( "Standard globular cluster stretch" )
   );

   return rules;
}

// ----------------------------------------------------------------------------

std::vector<SelectionRule> GetDustLaneRules()
{
   std::vector<SelectionRule> rules;

   // Very dark dust - preserve darkness
   rules.push_back(
      SelectionRule( "DustLane_VeryDark", RegionClass::DustLane, AlgorithmType::Histogram, 10.0 )
         .WhenMedianBelow( 0.02 )
         .WithParameter( "shadowsClipping", 0.0 )
         .WithConfidence( 0.85 )
         .WithRationale( "Very dark dust lanes need shadow preservation" )
   );

   // Default dust lane
   rules.push_back(
      SelectionRule( "DustLane_Default", RegionClass::DustLane, AlgorithmType::Histogram, 1.0 )
         .WithParameter( "shadowsClipping", 0.0 )
         .WithParameter( "midtones", 0.3 )
         .WithConfidence( 0.75 )
         .WithRationale( "Standard dust lane treatment" )
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
