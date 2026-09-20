// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __PICopilotInstance_h
#define __PICopilotInstance_h

#include <pcl/ProcessImplementation.h>

namespace pcl
{

class PICopilotInstance : public ProcessImplementation
{
public:
   PICopilotInstance( const MetaProcess* );
   PICopilotInstance( const PICopilotInstance& );

   void Assign( const ProcessImplementation& ) override;
   bool IsHistoryUpdater( const View& ) const override;
   UndoFlags UndoMode( const View& ) const override;

   bool CanExecuteOn( const View&, String& whyNot ) const override;
   bool CanExecuteGlobal( String& whyNot ) const override;
   bool ExecuteGlobal() override;
};

// No singleton — PCL creates instances per-use via Process::Create()/Clone()

} // namespace pcl

#endif // __PICopilotInstance_h
