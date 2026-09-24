// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PICopilotSelfTest.h"
#include "PICopilotModule.h"     // ThePICopilotModule
#include "AnthropicClient.h"
#include "ChatThread.h"

#include <pcl/Process.h>
#include <pcl/ProcessInstance.h>
#include <pcl/Settings.h>
#include <pcl/Variant.h>

#include <nlohmann/json.hpp>

#include <cstdlib>

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
      evalResult = v.ToInt();
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

   // Path 3: Settings round-trip on a throwaway key (proves the local
   // Settings store works before KeyStore relies on it for the real
   // Anthropic API key in later tasks). Never touches
   // "PICopilot/AnthropicApiKey".
   bool keyStoreOk = false;
   try
   {
      Settings::Write( "PICopilot/SelfTestKey", String( "rt-probe-42" ) );
      String back;
      Settings::Read( "PICopilot/SelfTestKey", back );
      keyStoreOk = (back == "rt-probe-42");
      Settings::Remove( "PICopilot/SelfTestKey" );
   }
   catch ( ... )
   {
      keyStoreOk = false;
   }

   // Path 4: gated real-API check against AnthropicClient. Only runs when
   // PICOPILOT_TEST_API_KEY is set in the environment (the controller
   // exports it from a local, gitignored key file before this self-test
   // runs); otherwise this path is skipped so CI without a key still
   // passes. Never touches KeyStore/Settings -- the env var is separate
   // from the user's persisted key.
   bool anthropicOk = false;
   bool anthropicSkipped = true;
   try
   {
      if ( const char* envKey = std::getenv( "PICOPILOT_TEST_API_KEY" ) )
      {
         anthropicSkipped = false;
         AnthropicClient client{ String( envKey ) };
         AnthropicResult r = client.Send( "You are a test.",
            { AnthropicMessage{ IsoString( "user" ), String( "Reply with exactly: WORKING" ) } } );
         anthropicOk = r.ok && r.text.Contains( "WORKING" );
      }
      else
      {
         anthropicOk = true; // nothing to prove without a key
      }
   }
   catch ( ... )
   {
      anthropicOk = false;
   }

   // Path 5: off-root-thread Anthropic request. Runs a ChatThread (and so
   // AnthropicClient::Send -- including its Control-derived response sink
   // and the NetworkTransfer POST) on a real pcl::Thread with a
   // deliberately INVALID key, and requires the API's 401 to come back
   // through TryTakeResult(). No real key needed: a 401 proves the worker
   // thread built the Control, performed the HTTPS POST and parsed the
   // error body. Bounded wait so a hung request can't wedge the harness.
   bool workerThreadOk = false;
   int  workerHttpStatus = 0;
   String workerError = "no result (thread did not complete)";
   try
   {
      ChatThread t( String( "sk-ant-invalid-selftest" ), String( "You are a test." ),
         { AnthropicMessage{ IsoString( "user" ), String( "ping" ) } } );
      t.Start();
      if ( t.Wait( 150000 ) )
      {
         AnthropicResult r;
         if ( t.TryTakeResult( r ) )
         {
            workerHttpStatus = r.httpStatus;
            workerError = r.error;
            workerThreadOk = !r.ok && r.httpStatus == 401;
         }
      }
      else
      {
         workerError = "timed out after 150 s";
         t.Abort();
         t.Wait();
      }
   }
   catch ( ... )
   {
      workerThreadOk = false;
      workerError = "exception constructing/starting ChatThread";
   }

   bool ok = evalOk && piValid && keyStoreOk && anthropicOk && workerThreadOk;
   jsonOut = String().Format(
      "{\"evalResult\":%d,\"evalOk\":%s,\"processInstanceValid\":%s,\"keyStoreOk\":%s,"
      "\"anthropicOk\":%s,\"anthropicSkipped\":%s,\"workerThreadOk\":%s,\"workerHttpStatus\":%d,\"workerError\":%s,"
      "\"ok\":%s}",
      evalResult, evalOk ? "true" : "false", piValid ? "true" : "false",
      keyStoreOk ? "true" : "false", anthropicOk ? "true" : "false",
      anthropicSkipped ? "true" : "false", workerThreadOk ? "true" : "false", workerHttpStatus,
      // nlohmann escapes the API's error text into a valid JSON string.
      nlohmann::json( workerError.ToUTF8().c_str() ).dump().c_str(),
      ok ? "true" : "false" );
   return ok;
}

} // namespace pcl
