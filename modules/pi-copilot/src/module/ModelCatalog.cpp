// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "ModelCatalog.h"

namespace pcl
{

const ModelInfo kPICopilotModels[PICopilotModelCount] =
{
   { "claude-opus-5-5", "Claude Opus 5.5", true  },
   { "claude-sonnet-5", "Claude Sonnet 5", false }
};

int ModelIndex( const IsoString& id )
{
   for ( size_type i = 0; i < PICopilotModelCount; ++i )
      if ( id == kPICopilotModels[i].id )
         return int( i );
   return -1;
}

const ModelInfo* FindModel( const IsoString& id )
{
   const int i = ModelIndex( id );
   return i < 0 ? nullptr : &kPICopilotModels[i];
}

} // namespace pcl
