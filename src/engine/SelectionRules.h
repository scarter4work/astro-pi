//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Selection Rules - Algorithm selection logic

#ifndef __SelectionRules_h
#define __SelectionRules_h

#include "RegionStatistics.h"
#include "StretchLibrary.h"

#include <pcl/String.h>

#include <map>
#include <vector>
#include <functional>

namespace pcl
{

// ----------------------------------------------------------------------------
// Algorithm Recommendation
// ----------------------------------------------------------------------------

struct AlgorithmRecommendation
{
   AlgorithmType algorithm = AlgorithmType::MTF;
   std::map<IsoString, double> parameters;
   double confidence = 1.0;        // How confident we are in this choice (0-1)
   String rationale = String();    // Human-readable explanation - MUST be initialized

   // Default constructor - explicitly initialize all members
   AlgorithmRecommendation()
      : algorithm( AlgorithmType::MTF )
      , parameters()
      , confidence( 1.0 )
      , rationale()
   {
   }

   // Simple constructor - takes algorithm type only
   explicit AlgorithmRecommendation( AlgorithmType algo )
      : algorithm( algo )
      , parameters()
      , confidence( 1.0 )
      , rationale()
   {
   }

   // Full constructor - explicit parameters, no initializer_list
   AlgorithmRecommendation( AlgorithmType algo, double conf )
      : algorithm( algo )
      , parameters()
      , confidence( conf )
      , rationale()
   {
   }

   // Helper to add a parameter safely
   void SetParameter( const char* name, double value )
   {
      parameters[IsoString( name )] = value;
   }

   // Helper to set rationale safely
   void SetRationale( const char* text )
   {
      rationale = String( text );
   }
};

// ----------------------------------------------------------------------------
// Selection Rule
//
// A single rule that evaluates conditions and produces a recommendation.
// ----------------------------------------------------------------------------

class SelectionRule
{
public:

   using ConditionFunc = std::function<bool( const RegionStatistics& )>;
   using ParameterFunc = std::function<void( const RegionStatistics&, AlgorithmRecommendation& )>;

   SelectionRule() = default;

   SelectionRule( const String& name,
                  RegionClass targetRegion,
                  AlgorithmType algorithm,
                  double priority = 1.0 );

   // Rule identification
   String Name() const { return m_name; }
   RegionClass TargetRegion() const { return m_targetRegion; }
   double Priority() const { return m_priority; }

   // Set conditions (all must be true for rule to apply)
   SelectionRule& When( ConditionFunc condition );
   SelectionRule& WhenSNRAbove( double threshold );
   SelectionRule& WhenSNRBelow( double threshold );
   SelectionRule& WhenMedianAbove( double threshold );
   SelectionRule& WhenMedianBelow( double threshold );
   SelectionRule& WhenDynamicRangeAbove( double threshold );
   SelectionRule& WhenClippingAbove( double threshold );

   // Set base algorithm
   SelectionRule& UseAlgorithm( AlgorithmType algo );

   // Set parameters (static or dynamic)
   SelectionRule& WithParameter( const IsoString& name, double value );
   SelectionRule& WithDynamicParameter( const IsoString& name, ParameterFunc func );

   // Set confidence and rationale
   SelectionRule& WithConfidence( double conf );
   SelectionRule& WithRationale( const String& reason );

   // Evaluate rule against statistics
   bool Matches( const RegionStatistics& stats ) const;

   // Get recommendation (assumes Matches returned true)
   AlgorithmRecommendation GetRecommendation( const RegionStatistics& stats ) const;

private:

   String m_name = String();  // Must be initialized for PCL ABI
   RegionClass m_targetRegion = RegionClass::Background;
   AlgorithmType m_algorithm = AlgorithmType::MTF;
   double m_priority = 1.0;
   double m_confidence = 1.0;
   String m_rationale = String();  // Must be initialized for PCL ABI

   std::vector<ConditionFunc> m_conditions;
   std::map<IsoString, double> m_staticParams;
   std::map<IsoString, ParameterFunc> m_dynamicParams;
};

// ----------------------------------------------------------------------------
// Selection Rules Engine
//
// Manages a collection of rules and determines the best algorithm
// for each region based on its statistics.
// ----------------------------------------------------------------------------

class SelectionRulesEngine
{
public:

   SelectionRulesEngine();

   // Add a rule
   void AddRule( const SelectionRule& rule );

   // Remove all rules for a region
   void ClearRules( RegionClass region );

   // Clear all rules
   void ClearAllRules();

   // Get recommendation for a region
   AlgorithmRecommendation GetRecommendation( RegionClass region,
                                               const RegionStatistics& stats ) const;

   // Get all matching recommendations (sorted by priority)
   std::vector<AlgorithmRecommendation> GetAllRecommendations( RegionClass region,
                                                                const RegionStatistics& stats ) const;

   // Get default rules
   static SelectionRulesEngine CreateDefaultRules();

   // Serialize/deserialize rules
   String ToJSON() const;
   bool FromJSON( const String& json );

private:

   std::map<RegionClass, std::vector<SelectionRule>> m_rules;

   // Default recommendation when no rules match
   AlgorithmRecommendation GetDefaultRecommendation( RegionClass region ) const;
};

// ----------------------------------------------------------------------------
// Default Selection Rules
//
// Pre-defined rules based on astrophotography best practices.
// These implement the logic from the NukeX specification.
// ----------------------------------------------------------------------------

namespace DefaultRules
{
   // Get all default rules
   std::vector<SelectionRule> GetAllRules();

   // Background rules
   std::vector<SelectionRule> GetBackgroundRules();

   // Star rules (by brightness)
   std::vector<SelectionRule> GetStarBrightRules();
   std::vector<SelectionRule> GetStarMediumRules();
   std::vector<SelectionRule> GetStarFaintRules();

   // Nebula rules (by type)
   std::vector<SelectionRule> GetNebulaEmissionRules();
   std::vector<SelectionRule> GetNebulaDarkRules();

   // Galaxy rules
   std::vector<SelectionRule> GetGalaxyCoreRules();
   std::vector<SelectionRule> GetGalaxySpiralRules();

   // Structural rules
   std::vector<SelectionRule> GetDustLaneRules();
   std::vector<SelectionRule> GetStarClusterRules();
}

// ----------------------------------------------------------------------------
// Parameter Tuning Functions
//
// Functions that compute optimal parameters based on statistics.
// ----------------------------------------------------------------------------

namespace ParameterTuning
{
   // MTF midtones based on median
   double ComputeMTFMidtones( const RegionStatistics& stats );

   // ArcSinh softness based on dynamic range
   double ComputeArcSinhSoftness( const RegionStatistics& stats );

   // GHS parameters based on histogram shape
   double ComputeGHSStretchFactor( const RegionStatistics& stats );
   double ComputeGHSLocalIntensity( const RegionStatistics& stats );
   double ComputeGHSSymmetryPoint( const RegionStatistics& stats );
   double ComputeGHSHighlightProtection( const RegionStatistics& stats );

   // Log stretch scale based on faintness
   double ComputeLogScale( const RegionStatistics& stats );

   // Lumpton Q parameter based on SNR
   double ComputeLumptonQ( const RegionStatistics& stats );

   // SAS iterations based on noise level
   int ComputeSASIterations( const RegionStatistics& stats );
}

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __SelectionRules_h
