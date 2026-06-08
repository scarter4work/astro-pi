//     ____       __ __  __
//    / __ \___  / // / / /
//   / /_/ / _ \/ // /_/ /
//  / ____/  __/__  __/_/
// /_/    \___/  /_/ (_)
//
// NukeX - Intelligent Region-Aware Stretch for PixInsight
// Copyright (c) 2026 Scott Carter
//
// Stretch Algorithm Library - Factory and Registry

#ifndef __StretchLibrary_h
#define __StretchLibrary_h

#include "IStretchAlgorithm.h"

#include <memory>
#include <vector>
#include <map>
#include <functional>

namespace pcl
{

// ----------------------------------------------------------------------------
// Algorithm Type Enumeration (matches NukeXParameters)
// ----------------------------------------------------------------------------

enum class AlgorithmType
{
   MTF = 0,
   Histogram,
   GHS,
   ArcSinh,
   Log,
   Lumpton,
   RNC,
   Photometric,
   OTS,
   SAS,
   Veralux,
   StatAuto,    // Statistical Auto-Stretch (pure statistics, no ML)
   Auto,
   Count
};

// ----------------------------------------------------------------------------
// Algorithm Information
// ----------------------------------------------------------------------------

struct AlgorithmInfo
{
   AlgorithmType type;
   IsoString     id;
   String        name;
   String        description;
   bool          implemented;  // True if algorithm is fully implemented

   AlgorithmInfo( AlgorithmType t = AlgorithmType::MTF,
                  const IsoString& i = IsoString(),
                  const String& n = String(),
                  const String& d = String(),
                  bool impl = false )
      : type( t ), id( i ), name( n ), description( d ), implemented( impl )
   {
   }
};

// ----------------------------------------------------------------------------
// Stretch Library - Factory and Registry
// ----------------------------------------------------------------------------

class StretchLibrary
{
public:

   /// Get the singleton instance
   static StretchLibrary& Instance();

   /// Create an algorithm instance by type
   std::unique_ptr<IStretchAlgorithm> Create( AlgorithmType type ) const;

   /// Create an algorithm instance by ID string
   std::unique_ptr<IStretchAlgorithm> Create( const IsoString& id ) const;

   /// Get information about an algorithm
   const AlgorithmInfo& GetInfo( AlgorithmType type ) const;

   /// Get list of all registered algorithms
   std::vector<AlgorithmInfo> GetAllAlgorithms() const;

   /// Get list of implemented algorithms only
   std::vector<AlgorithmInfo> GetImplementedAlgorithms() const;

   /// Convert algorithm type to ID string
   static IsoString TypeToId( AlgorithmType type );

   /// Convert ID string to algorithm type
   static AlgorithmType IdToType( const IsoString& id );

   /// Get display name for algorithm type
   static String TypeToName( AlgorithmType type );

   /// Check if algorithm is implemented
   bool IsImplemented( AlgorithmType type ) const;

private:

   StretchLibrary();
   ~StretchLibrary() = default;

   // Non-copyable
   StretchLibrary( const StretchLibrary& ) = delete;
   StretchLibrary& operator=( const StretchLibrary& ) = delete;

   // Algorithm registry
   std::map<AlgorithmType, AlgorithmInfo> m_registry;

   // Factory functions
   typedef std::function<std::unique_ptr<IStretchAlgorithm>()> FactoryFunc;
   std::map<AlgorithmType, FactoryFunc> m_factories;

   // Register an algorithm
   void Register( AlgorithmType type, const AlgorithmInfo& info, FactoryFunc factory );

   // Initialize registry
   void InitializeRegistry();
};

// ----------------------------------------------------------------------------
// Convenience functions
// ----------------------------------------------------------------------------

/// Create a stretch algorithm by type
inline std::unique_ptr<IStretchAlgorithm> CreateStretchAlgorithm( AlgorithmType type )
{
   return StretchLibrary::Instance().Create( type );
}

/// Create a stretch algorithm by ID
inline std::unique_ptr<IStretchAlgorithm> CreateStretchAlgorithm( const IsoString& id )
{
   return StretchLibrary::Instance().Create( id );
}

// ----------------------------------------------------------------------------

} // namespace pcl

#endif // __StretchLibrary_h
