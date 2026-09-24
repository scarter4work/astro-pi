// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "PICopilotSelfTest.h"
#include "PICopilotModule.h"     // ThePICopilotModule
#include "AnthropicClient.h"
#include "ChatThread.h"
#include "PICopilotInterface.h"   // PICopilotInterface::PlainText
#include "PICopilotVisionSelfTest.h"
#include "Utf8.h"

#include <pcl/Process.h>
#include <pcl/ProcessInstance.h>
#include <pcl/Settings.h>
#include <pcl/TextBox.h>
#include <pcl/Variant.h>

#include <nlohmann/json.hpp>

#include <chrono>
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
            { AnthropicMessage{ IsoString( "user" ), String( "Reply with exactly: WORKING" ), IsoString() } } );
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

   // Path 5: off-root-thread Anthropic request. The ChatThread constructor
   // builds the AnthropicRequest (Control-derived response sink +
   // NetworkTransfer) HERE on the root thread; the worker pcl::Thread only
   // Perform()s the HTTPS POST + parse, with a deliberately INVALID key, and
   // the API's 401 must come back through TryTakeResult(). No real key
   // needed: a 401 proves the worker performed the POST and parsed the
   // error body. Bounded wait so a hung request can't wedge the harness.
   bool workerThreadOk = false;
   int  workerHttpStatus = 0;
   String workerError = "no result (thread did not complete)";
   try
   {
      ChatThread t( String( "sk-ant-invalid-selftest" ), String( "You are a test." ),
         { AnthropicMessage{ IsoString( "user" ), String( "ping" ), IsoString() } } );
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
         // Thread::Abort() only raises a flag the blocking POST never
         // checks; RequestCancel() aborts the transfer itself.
         t.RequestCancel();
         t.Wait();
      }
   }
   catch ( ... )
   {
      workerThreadOk = false;
      workerError = "exception constructing/starting ChatThread";
   }

   // Path 6: cancel + overall deadline against a stalled connection. The
   // harness runs a local TCP server that accepts, reads the request and
   // never answers -- exactly the case SetConnectionTimeout() cannot bound.
   bool   stallSkipped = true;
   bool   cancelOk = false, deadlineOk = false;
   String cancelError = "not run", deadlineError = "not run";
   double cancelSeconds = -1, deadlineSeconds = -1;
   using clock = std::chrono::steady_clock;
   auto secondsSince = []( clock::time_point t0 )
   {
      return std::chrono::duration<double>( clock::now() - t0 ).count();
   };
   const char* stallUrl = std::getenv( "PICOPILOT_SELFTEST_STALL_URL" );
   if ( stallUrl != nullptr && *stallUrl != '\0' )
   {
      stallSkipped = false;
      const Array<AnthropicMessage> ping = { AnthropicMessage{ IsoString( "user" ), String( "ping" ), IsoString() } };

      // 6a: cancel mid-stall. Default (300 s) deadline, so only the cancel
      // can end it quickly. Wait(2000) lets the request connect and stall.
      try
      {
         ChatThread t( String( "sk-ant-invalid-selftest" ), String( "You are a test." ), ping,
                       PICOPILOT_DEFAULT_MODEL, String( stallUrl ) );
         clock::time_point t0 = clock::now();
         t.Start();
         bool finishedEarly = t.Wait( 2000 );
         t.RequestCancel();
         bool finished = finishedEarly || t.Wait( 30000 );
         cancelSeconds = secondsSince( t0 );
         if ( !finished )
         {
            cancelError = "still running 30 s after RequestCancel()";
            // Last resort so the harness can exit: the deadline ends it.
            t.Wait();
         }
         else
         {
            AnthropicResult r;
            if ( t.TryTakeResult( r ) )
               cancelError = r.error;
            cancelOk = !finishedEarly && !r.ok && r.error == "request cancelled" && cancelSeconds < 15;
         }
      }
      catch ( ... )
      {
         cancelError = "exception in cancel path";
      }

      // 6b: deadline. A 3 s overall limit must end the stalled request by
      // itself, well before the harness bound.
      try
      {
         ChatThread t( String( "sk-ant-invalid-selftest" ), String( "You are a test." ), ping,
                       PICOPILOT_DEFAULT_MODEL, String( stallUrl ), 3/*timeoutSeconds*/ );
         clock::time_point t0 = clock::now();
         t.Start();
         bool finished = t.Wait( 30000 );
         deadlineSeconds = secondsSince( t0 );
         if ( !finished )
         {
            deadlineError = "still running after 30 s with a 3 s deadline";
            t.RequestCancel();
            t.Wait();
         }
         else
         {
            AnthropicResult r;
            if ( t.TryTakeResult( r ) )
               deadlineError = r.error;
            deadlineOk = !r.ok && r.error == "request timed out after 3 s"
                      && deadlineSeconds >= 2.5 && deadlineSeconds < 15;
         }
      }
      catch ( ... )
      {
         deadlineError = "exception in deadline path";
      }
   }

   // Path 7: a literal "</raw>" inside chat text must stay literal in a
   // real TextBox (root-thread Control, like the response sink above).
   bool   plainTextOk = false;
   String plainTextBack;
   try
   {
      const String probe = "a</raw><b>x</b>< / RAW>z";
      TextBox box;
      box.SetText( PICopilotInterface::PlainText( probe ) );
      plainTextBack = box.Text();
      plainTextOk = plainTextBack.Trimmed() == probe;
   }
   catch ( ... )
   {
      plainTextBack = "exception constructing TextBox";
   }

   bool ok = evalOk && piValid && keyStoreOk && anthropicOk && workerThreadOk
          && (stallSkipped || (cancelOk && deadlineOk)) && plainTextOk;
   // nlohmann builds the JSON so every free-text field (API error text,
   // TextBox read-back) is escaped into a valid JSON string.
   nlohmann::json j = {
      { "evalResult", evalResult },
      { "evalOk", evalOk },
      { "processInstanceValid", piValid },
      { "keyStoreOk", keyStoreOk },
      { "anthropicOk", anthropicOk },
      { "anthropicSkipped", anthropicSkipped },
      { "workerThreadOk", workerThreadOk },
      { "workerHttpStatus", workerHttpStatus },
      { "workerError", U8( workerError ) },
      { "stallSkipped", stallSkipped },
      { "cancelOk", cancelOk },
      { "cancelError", U8( cancelError ) },
      { "cancelSeconds", cancelSeconds },
      { "deadlineOk", deadlineOk },
      { "deadlineError", U8( deadlineError ) },
      { "deadlineSeconds", deadlineSeconds },
      { "plainTextOk", plainTextOk },
      { "plainTextBack", U8( plainTextBack ) }
   };

   // Increment 3: vision/grounding sections. Never let an escape here lose
   // the increment-1/2 verdict -- record it as a failure instead.
   bool visionOk = false;
   try
   {
      nlohmann::json vision;
      visionOk = RunVisionSelfTest( vision );
      j.update( vision );
   }
   catch ( const std::exception& x )
   {
      j["visionException"] = x.what();
   }
   catch ( ... )
   {
      j["visionException"] = "unknown exception";
   }

   ok = ok && visionOk;
   j["ok"] = ok;
   jsonOut = String::UTF8ToUTF16( j.dump().c_str() );
   return ok;
}

} // namespace pcl
