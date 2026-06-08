//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Stretch Algorithm Interface

#ifndef __IStretchAlgorithm_h
#define __IStretchAlgorithm_h

#include <pcl/String.h>
#include <pcl/Image.h>
#include <pcl/Property.h>

#include <memory>
#include <vector>
#include <map>

#include "RegionStatistics.h"

namespace pcl
{

// ----------------------------------------------------------------------------
// Parameter Definition for algorithms
// ----------------------------------------------------------------------------

struct AlgorithmParameter
{
   IsoString id;           // Parameter identifier
   String    name;         // Display name
   String    tooltip;      // Help text
   double    value;        // Current value
   double    defaultValue; // Default value
   double    minValue;     // Minimum allowed value
   double    maxValue;     // Maximum allowed value
   int       precision;    // Decimal places for display

   AlgorithmParameter( const IsoString& id_ = IsoString(),
                       const String& name_ = String(),
                       double defaultVal = 0.0,
                       double minVal = 0.0,
                       double maxVal = 1.0,
                       int prec = 3,
                       const String& tip = String() )
      : id( id_ )
      , name( name_ )
      , tooltip( tip )
      , value( defaultVal )
      , defaultValue( defaultVal )
      , minValue( minVal )
      , maxValue( maxVal )
      , precision( prec )
   {
   }
};

typedef std::vector<AlgorithmParameter> ParameterList;

// ----------------------------------------------------------------------------
// Stretch Algorithm Interface
// ----------------------------------------------------------------------------

class IStretchAlgorithm
{
public:

   virtual ~IStretchAlgorithm() = default;

   // -------------------------------------------------------------------------
   // Core stretch operation
   // -------------------------------------------------------------------------

   /// Apply stretch to a single pixel value
   /// @param value Input pixel value (0-1 normalized)
   /// @return Stretched pixel value (0-1 normalized)
   virtual double Apply( double value ) const = 0;

   /// Apply stretch to an image with optional mask
   /// @param image The image to stretch (modified in place)
   /// @param mask Optional mask (nullptr for no mask, values 0-1)
   virtual void ApplyToImage( Image& image, const Image* mask = nullptr ) const;

   // -------------------------------------------------------------------------
   // Algorithm metadata
   // -------------------------------------------------------------------------

   /// Get algorithm identifier
   virtual IsoString Id() const = 0;

   /// Get algorithm display name
   virtual String Name() const = 0;

   /// Get algorithm description
   virtual String Description() const = 0;

   // -------------------------------------------------------------------------
   // Parameter management
   // -------------------------------------------------------------------------

   /// Get list of algorithm parameters
   virtual ParameterList GetParameters() const = 0;

   /// Set parameter value by id
   virtual bool SetParameter( const IsoString& id, double value ) = 0;

   /// Get parameter value by id
   virtual double GetParameter( const IsoString& id ) const = 0;

   /// Reset all parameters to defaults
   virtual void ResetParameters() = 0;

   // -------------------------------------------------------------------------
   // Auto-configuration
   // -------------------------------------------------------------------------

   /// Auto-configure parameters based on region statistics
   /// @param stats Statistics of the region to be stretched
   virtual void AutoConfigure( const RegionStatistics& stats ) = 0;

   // -------------------------------------------------------------------------
   // Cloning
   // -------------------------------------------------------------------------

   /// Create a copy of this algorithm
   [[nodiscard]] virtual std::unique_ptr<IStretchAlgorithm> Clone() const = 0;

protected:

   /// Helper to clamp value to valid range
   static double Clamp( double value, double min = 0.0, double max = 1.0 )
   {
      return (value < min) ? min : (value > max) ? max : value;
   }
};

// ----------------------------------------------------------------------------
// Base implementation with common functionality
// ----------------------------------------------------------------------------

class StretchAlgorithmBase : public IStretchAlgorithm
{
public:

   void ApplyToImage( Image& image, const Image* mask = nullptr ) const override;

   ParameterList GetParameters() const override { return m_parameters; }

   bool SetParameter( const IsoString& id, double value ) override;
   double GetParameter( const IsoString& id ) const override;
   void ResetParameters() override;

protected:

   ParameterList m_parameters;

   /// Add a parameter to the algorithm
   void AddParameter( const AlgorithmParameter& param )
   {
      m_parameters.push_back( param );
   }

   /// Find parameter by id (returns nullptr if not found)
   AlgorithmParameter* FindParameter( const IsoString& id );
   const AlgorithmParameter* FindParameter( const IsoString& id ) const;
};

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __IStretchAlgorithm_h
