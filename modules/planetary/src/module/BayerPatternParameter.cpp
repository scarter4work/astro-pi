// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "BayerPatternParameter.h"

#include <pcl/ErrorHandler.h>

namespace ser = planetary::ser;

namespace pcl
{

BayerPatternEnumeration::BayerPatternEnumeration( MetaProcess* p ) : MetaEnumeration( p )
{
}

IsoString BayerPatternEnumeration::Id() const { return "bayerPattern"; }
size_type BayerPatternEnumeration::NumberOfElements() const { return NumberOfItems; }

IsoString BayerPatternEnumeration::ElementId( size_type i ) const
{
   switch ( i )
   {
   default:
   case Auto: return "BayerAuto";
   case None: return "BayerNone";
   case RGGB: return "BayerRGGB";
   case GRBG: return "BayerGRBG";
   case GBRG: return "BayerGBRG";
   case BGGR: return "BayerBGGR";
   }
}

int BayerPatternEnumeration::ElementValue( size_type i ) const { return int( i ); }
size_type BayerPatternEnumeration::DefaultValueIndex() const { return size_type( Default ); }

ser::BayerChoice BayerChoiceFor( pcl_enum bayerPattern, const char* process )
{
   switch ( bayerPattern )
   {
   case BayerPatternEnumeration::None: return ser::BayerChoice::None;
   case BayerPatternEnumeration::RGGB: return ser::BayerChoice::RGGB;
   case BayerPatternEnumeration::GRBG: return ser::BayerChoice::GRBG;
   case BayerPatternEnumeration::GBRG: return ser::BayerChoice::GBRG;
   case BayerPatternEnumeration::BGGR: return ser::BayerChoice::BGGR;
   case BayerPatternEnumeration::Auto: return ser::BayerChoice::Auto;
   default:
      throw Error( String( process ) + ": invalid bayerPattern value " + String( int( bayerPattern ) ) );
   }
}

} // namespace pcl
