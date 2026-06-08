//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Stretch Selector - Main selection interface

#ifndef __StretchSelector_h
#define __StretchSelector_h

#include "SelectionRules.h"
#include "StretchLibrary.h"
#include "RegionAnalyzer.h"

#include <pcl/String.h>

#include <map>
#include <memory>

namespace pcl
{

// ----------------------------------------------------------------------------
// Selected Stretch Configuration
// ----------------------------------------------------------------------------

struct SelectedStretch
{
   RegionClass region = RegionClass::Background;
   AlgorithmType algorithm = AlgorithmType::MTF;
   std::map<IsoString, double> parameters;

   // The configured algorithm instance (ready to use)
   std::unique_ptr<IStretchAlgorithm> algorithmInstance;

   // Selection metadata
   double confidence = 1.0;
   String rationale;  // Initialized by explicit default constructor below
   bool isOverride = false;     // User overrode the automatic selection

   // Statistics used for selection
   RegionStatistics statistics;

   SelectedStretch() = default;
   SelectedStretch( SelectedStretch&& ) = default;
   SelectedStretch& operator=( SelectedStretch&& ) = default;

   // No copy (algorithmInstance is unique_ptr)
   SelectedStretch( const SelectedStretch& ) = delete;
   SelectedStretch& operator=( const SelectedStretch& ) = delete;
};

// ----------------------------------------------------------------------------
// User Override
// ----------------------------------------------------------------------------

struct UserOverride
{
   RegionClass region;
   AlgorithmType algorithm;
   std::map<IsoString, double> parameters;
   bool enabled = true;
};

// ----------------------------------------------------------------------------
// Stretch Selector
//
// Determines the best stretch algorithm for each region based on:
// 1. Region statistics
// 2. Selection rules
// 3. User overrides
// ----------------------------------------------------------------------------

class StretchSelector
{
public:

   StretchSelector();
   ~StretchSelector();

   // Initialize with default rules
   void Initialize();

   // Initialize with custom rules
   void Initialize( const SelectionRulesEngine& rules );

   // Select algorithms for all regions in an analysis result
   std::map<RegionClass, SelectedStretch> SelectAll(
      const RegionAnalysisResult& analysis ) const;

   // Select algorithm for a specific region
   SelectedStretch Select( RegionClass region,
                           const RegionStatistics& stats ) const;

   // User overrides
   void SetOverride( const UserOverride& override );
   void ClearOverride( RegionClass region );
   void ClearAllOverrides();
   bool HasOverride( RegionClass region ) const;
   const UserOverride* GetOverride( RegionClass region ) const;

   // Enable/disable specific region
   void EnableRegion( RegionClass region, bool enable = true );
   bool IsRegionEnabled( RegionClass region ) const;

   // Global strength multiplier (affects all regions)
   void SetGlobalStrength( double strength );
   double GetGlobalStrength() const { return m_globalStrength; }

   // Get selection rationale for a region
   String GetRationale( RegionClass region,
                        const RegionStatistics& stats ) const;

   // Get all recommendations (not just the top one)
   std::vector<AlgorithmRecommendation> GetAllRecommendations(
      RegionClass region, const RegionStatistics& stats ) const;

   // Access rules engine
   SelectionRulesEngine& Rules() { return m_rules; }
   const SelectionRulesEngine& Rules() const { return m_rules; }

private:

   SelectionRulesEngine m_rules;
   std::map<RegionClass, UserOverride> m_overrides;
   std::map<RegionClass, bool> m_enabledRegions;
   double m_globalStrength = 1.0;

   // Create algorithm instance from recommendation
   std::unique_ptr<IStretchAlgorithm> CreateAlgorithm(
      const AlgorithmRecommendation& rec,
      const RegionStatistics& stats ) const;

   // Apply global strength to parameters
   void ApplyStrengthModifier( std::map<IsoString, double>& params ) const;
};

// ----------------------------------------------------------------------------
// Stretch Selection Summary
//
// For UI display of selection results.
// ----------------------------------------------------------------------------

struct SelectionSummary
{
   struct RegionEntry
   {
      RegionClass region = RegionClass::Background;
      AlgorithmType algorithm = AlgorithmType::MTF;
      double confidence = 0.0;
      bool isOverride = false;
      double coverage = 0.0;
      // NOTE: String members removed to avoid PCL ABI crashes
      // Names are computed on-demand in ToString()
   };

   std::vector<RegionEntry> entries;
   int totalRegions = 0;
   int overriddenRegions = 0;
   double averageConfidence = 0;

   // Create from selection results
   static SelectionSummary Create(
      const std::map<RegionClass, SelectedStretch>& selections,
      const std::map<RegionClass, double>& coverage );

   // Format as string
   String ToString() const;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __StretchSelector_h
