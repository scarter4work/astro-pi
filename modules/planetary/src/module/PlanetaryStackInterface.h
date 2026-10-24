// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __PlanetaryStackInterface_h
#define __PlanetaryStackInterface_h

#include <pcl/ComboBox.h>
#include <pcl/Label.h>
#include <pcl/ProcessInterface.h>
#include <pcl/PushButton.h>
#include <pcl/Sizer.h>
#include <pcl/TextBox.h>
#include <pcl/Timer.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "Engine.h"
#include "FramePreview.h"
#include "FrameSource.h"
#include "PlanetaryStackInstance.h"

namespace pcl
{

// The run's progress: an empty bar with a status line when no stack runs; while
// one runs, the part of the whole run done filled (in kProgressFill, which the
// GUI smoke reads) with the step and its own percentage on it.
class StackProgressBar : public Control
{
public:
   StackProgressBar( Control& parent = Control::Null() );

   void SetRunning( double fraction, const String& text );
   void SetIdle( const String& text );

private:
   bool   m_running = false;
   double m_fraction = 0;
   String m_text;

   void e_Paint( Control& sender, const pcl::Rect& updateRect );
};

// The one PlanetaryStack window (spec "What the user sees"): Open... picks a
// video (its facts go in the info block; an AVI gets the Bayer pattern combo
// and, without a sidecar, Save pattern...); Stack! runs stack::RunStack on a
// worker thread while a timer shows its progress, and Cancel stops it; a stack
// is previewed (display stretch only, the data stays linear) with Fit / 100% /
// 200% / zoom out / zoom in and drag to pan, and described in the info block;
// a capture with nothing usable shows no preview, only its diagnosis. Execute
// (and Apply Global) opens the stack shown as an image window, exactly as
// ExecuteGlobal would (OpenStackWindow), without stacking again -- or, when
// nothing is stacked for the file and pattern shown, stacks first and opens
// the result. No video file is held open between runs. Every handler is
// guarded; closing the window, or PixInsight exiting, cancels a running stack
// and waits for it.
class PlanetaryStackInterface : public ProcessInterface
{
public:
   PlanetaryStackInterface();
   virtual ~PlanetaryStackInterface();

   IsoString Id() const override;
   MetaProcess* Process() const override;
   InterfaceFeatures Features() const override;

   void ApplyInstanceGlobal() const override;
   void ResetInstance() override;

   bool Launch( const MetaProcess&, const ProcessImplementation*, bool& dynamic, unsigned& flags ) override;

   ProcessImplementation* NewProcess() const override;

   bool ValidateProcess( const ProcessImplementation&, String& whyNot ) const override;
   bool RequiresInstanceValidation() const override;

   bool ImportProcess( const ProcessImplementation& ) override;

   // Cancels a running stack and waits until it has stopped (the worker and
   // every engine thread joined, every reader closed), touching no control and
   // not the console: PlanetaryModule::OnUnload calls it before PixInsight
   // exits, when the core may be tearing its windows down; the destructor too.
   void JoinStack();

private:
   PlanetaryStackInstance instance;

   // The capture as last read (Open..., a Bayer pattern change, Save pattern...);
   // the reader is closed straight after.
   std::optional<planetary::ser::FileInfo> m_info;
   std::string m_infoLine;   // stackcards::StackInfoLine (UTF-8)
   std::string m_warnings;   // the reader's warnings, one "Warning: " line each
   std::string m_error;      // the last failure, shown in the info block

   // One run, shared with its worker thread. The worker writes only the atomics
   // (from the engine's Progress callbacks, which touch no PCL object) and, before
   // `finished`, the results; the GUI thread reads those only after joining it.
   struct Run
   {
      std::string path;
      planetary::ser::BayerChoice bayer;
      pcl_enum pattern = 0;  // instance.p_bayerPattern the run was started with
      int threads = 1;
      int64_t frames = 0;   // the capture's frame count, for the progress text
      std::atomic<uint64_t> progress{ 0 };  // the phase and its done / total counts (PackProgress)
      std::atomic<bool> cancel{ false };    // Cancel / StopStack; the engine's cancelled() reads it
      std::atomic<bool> finished{ false };  // the worker's last write
      std::optional<planetary::stack::StackOutcome> outcome;
      StackTimes times;
      bool cancelled = false;
      std::string error;
   };
   std::unique_ptr<Run> m_run;
   std::thread m_worker;
   bool m_openWhenDone = false;   // Execute started this run: open its stack when it ends

   // The last run's outcome, for the file and the effective Bayer pattern shown
   // (dropped when either changes), and its SER frame times.
   std::optional<planetary::stack::StackOutcome> m_outcome;
   String m_outcomePath;
   pcl_enum m_outcomePattern = 0;  // instance.p_bayerPattern it was stacked with
   StackTimes m_times;
   String m_status;   // the idle progress bar's line

   struct GUIData
   {
      GUIData( PlanetaryStackInterface& );

      VerticalSizer   Global_Sizer;
         HorizontalSizer File_Sizer;
            PushButton      Open_PushButton;
            Label           Bayer_Label;
            ComboBox        Bayer_ComboBox;
            PushButton      SavePattern_PushButton;
         HorizontalSizer Run_Sizer;
            PushButton      Stack_PushButton;
            StackProgressBar Progress_Bar;
            PushButton      Cancel_PushButton;
         ZoomPreview     Preview_Control;
         HorizontalSizer Zoom_Sizer;
            PushButton      Fit_PushButton;
            PushButton      Zoom100_PushButton;
            PushButton      Zoom200_PushButton;
            PushButton      ZoomOut_PushButton;
            PushButton      ZoomIn_PushButton;
            Label           Zoom_Label;
         TextBox         Info_TextBox;
         HorizontalSizer Execute_Sizer;
            PushButton      Execute_PushButton;

      Timer Progress_Timer;
   };

   GUIData* GUI = nullptr;

   bool Running() const
   {
      return bool( m_run );
   }

   // Reads instance.p_filePath with instance.p_bayerPattern (the reader closed
   // again at once) for the info block; drops a stack made from another file or
   // another effective pattern. A failure is shown (plus a message box when
   // interactive) and clears the window -- except a finished result of this file
   // and pattern (HoldsResultForInstance), which stays with the last info read: a file
   // that cannot be read right now (a network share's hiccup) says nothing about
   // the stack (final review, module m4). Returns whether the read succeeded.
   bool ReadInfo( bool interactive );
   // A finished result (stack or diagnosis) of the file and Bayer pattern the
   // window now names: kept through a failure that is not about it.
   bool HoldsResultForInstance() const;
   void LoadFromInstance();
   void DropResult();
   void ClearAll();
   // Starts a run of the file and pattern shown; openWhenDone: Execute's.
   void StartStack( bool openWhenDone );
   // The timer's tick: the progress, or, once the worker is done, its result.
   void PollStack();
   void FinishStack();
   // JoinStack, then says so: the console ("cancelled because <why>"), the
   // progress bar and the info block.
   void StopStack( const char* why );
   // Execute / Apply Global.
   void ExecuteStack();
   // Save pattern...: VideoControls' SaveBayerPattern, then the capture read
   // again with Auto (the stack stays: the effective pattern is the same).
   void SavePattern();

   void ShowText();
   void UpdateControls();

   template <class F>
   void Guarded( F&& f, bool interactive );
   void Fail( const String& message, bool interactive );

   void e_Click( Button& sender, bool checked );
   void e_ItemSelected( ComboBox& sender, int itemIndex );
   void e_Timer( Timer& sender );
   void e_Show( Control& sender );
   void e_Hide( Control& sender );
};

extern PlanetaryStackInterface* ThePlanetaryStackInterface;

} // namespace pcl

#endif // __PlanetaryStackInterface_h
