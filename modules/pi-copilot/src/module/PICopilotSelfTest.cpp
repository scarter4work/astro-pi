// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PICopilotSelfTest.h"
#include "PICopilotModule.h"     // ThePICopilotModule

#include <pcl/Process.h>
#include <pcl/ProcessInstance.h>
#include <pcl/Variant.h>

namespace pcl
{

bool RunSelfTest( String& jsonOut )
{
   int  evalResult = -1;
   bool evalOk = false;
   bool piValid = false;

   // Path 1: execute PJSR from C++ and read the result Variant.
   try
   {
      Variant v = ThePICopilotModule->EvaluateScript( "1+2", "JavaScript" );
      evalResult = int( v.ToInt() );
      evalOk = (evalResult == 3);
   }
   catch ( ... )
   {
      evalOk = false;
   }

   // Path 2: construct a native ProcessInstance against the registry.
   //
   // ChannelCombination, not PixelMath: a stock PixelMath instance has no
   // target-independent parameters set (no "create new image" flag), so
   // CanExecuteGlobal() correctly refuses it ("some of its parameters
   // depend on a particular target image") until it's configured for a
   // view. ChannelCombination has no such dependency in its default
   // state, so it proves the construction path without extra setup.
   try
   {
      IsoString classId( "ChannelCombination" );
      Process P( classId );
      ProcessInstance instance( P );
      String whyNot;
      piValid = instance.CanExecuteGlobal( whyNot );
   }
   catch ( ... )
   {
      piValid = false;
   }

   bool ok = evalOk && piValid;
   jsonOut = String().Format(
      "{\"evalResult\":%d,\"evalOk\":%s,\"processInstanceValid\":%s,\"ok\":%s}",
      evalResult, evalOk ? "true" : "false", piValid ? "true" : "false", ok ? "true" : "false" );
   return ok;
}

} // namespace pcl
