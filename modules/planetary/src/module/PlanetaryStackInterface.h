// Planetary — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#ifndef __PlanetaryStackInterface_h
#define __PlanetaryStackInterface_h

#include <pcl/CheckBox.h>
#include <pcl/ComboBox.h>
#include <pcl/Label.h>
#include <pcl/NumericControl.h>
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
//
// Target names what the capture is of (Auto: the ASIAIR capture name's,
// shown in the combo), and the post-processing row follows it
// (stackcards::PostKindFor): for the Moon, Mineral moon with Strength, Chroma
// blur and Sharpen; for anything else, Planet processing with Saturation,
// Chroma blur and Sharpen. Only that set is shown. Its check box is enabled
// only where the product can run (stackcards::PostRefusalFor: a colour Moon or
// planet, not through a narrowband or IR-pass filter; Target Other is refused);
// its tooltip says why not, and a choice that stops it turns it off, saying so.
// Checked, Stack! makes the product after the stack (a fourth step, Cancel
// stops it and keeps no image) and the preview shows it; checked after a stack,
// it is made from that stack (the worker, as Stack!'s). Unchecking shows the
// stack again; neither restacks. The sliders re-run only the finish (the
// sharpening and the colour step) on the cached data (stackcards::FinishPost on
// a colour worker, the newest values once it is free), never the stack. A
// Target change that the cache was not made for (another kind, another planet)
// drops it, and a colour result still in flight with it. Execute opens the
// product as a second window (<stem>_mineral, <stem>_planet).
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
   // every engine thread joined, every reader closed; a slider's sharpening and
   // colour steps running are waited for too), touching no control and
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
   // A Stack run stacks the capture (then, with `post`, makes its product: the
   // mineral moon or planet processing, `params`); a Post run makes the product
   // of the stack shown (`input`, a copy of it).
   enum class Job { Stack, Post };
   struct Run
   {
      Job job = Job::Stack;
      std::string path;
      planetary::ser::BayerChoice bayer;
      pcl_enum pattern = 0;  // instance.p_bayerPattern the run was started with
      int threads = 1;
      int64_t frames = 0;   // the capture's frame count, for the progress text
      int steps = 3;        // the progress text's steps: 3 (a stack), 4 (and its product), 1 (a Post run)
      bool post = false;    // Stack: make the product too
      PostParams params;    // the product's kind, planet and slider values
      std::optional<planetary::ser::FloatImage> input;  // Post: the stack
      std::optional<planetary::stack::StackNoise> inputNoise;  // Post: its noise (StackOutcome::noise; Ruling R35)
      std::atomic<uint64_t> progress{ 0 };  // the phase and its done / total counts (PackProgress)
      std::atomic<bool> cancel{ false };    // Cancel / StopStack; the engine's cancelled() reads it
      std::atomic<bool> finished{ false };  // the worker's last write
      std::optional<planetary::stack::StackOutcome> outcome;
      StackTimes times;
      std::shared_ptr<const planetary::stack::MineralCache> cache;  // the product's, when made
      std::optional<planetary::stack::MineralOutcome> postMade;
      std::string postError;  // the product could not be made (the stack stands)
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

   // The product of the stack shown (dropped with it, and on a Target change it
   // was not made for): PreparePost's cache, made once per stack (and planet),
   // and the image last finished from it with the slider values it was made
   // with. m_postFor: the kind and planet the cache, the image or the error are
   // of. m_postError: it could not be made.
   std::shared_ptr<const planetary::stack::MineralCache> m_post;
   std::optional<planetary::stack::MineralOutcome> m_postShown;
   PostParams m_shownParams;
   PostParams m_postFor;
   std::string m_postError;
   std::string m_postOff;  // which option was turned off for the capture or target shown, and why ("Mineral moon turned off: ...")

   // The sharpening and colour steps for a slider change (FinishPost on the
   // cache it holds, so a dropped stack cannot pull it away). Its worker
   // writes only the results, then `finished`; the timer collects them. A change
   // while it runs waits (m_colourPending) and the newest values run next.
   struct ColourRun
   {
      std::shared_ptr<const planetary::stack::MineralCache> cache;
      PostParams params;
      int threads = 1;
      std::atomic<bool> finished{ false };
      std::optional<planetary::stack::MineralOutcome> post;
      std::string error;
   };
   std::unique_ptr<ColourRun> m_colour;
   std::thread m_colourWorker;
   bool m_colourPending = false;

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
         HorizontalSizer Target_Sizer;
            Label           Target_Label;
            ComboBox        Target_ComboBox;
            CheckBox        Mineral_CheckBox;
            CheckBox        Planet_CheckBox;
         NumericControl  Strength_NumericControl;
         NumericControl  ChromaBlur_NumericControl;
         NumericControl  Sharpen_NumericControl;
         NumericControl  Saturation_NumericControl;
         NumericControl  PlanetChromaBlur_NumericControl;
         NumericControl  PlanetSharpen_NumericControl;
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
   // Starts a run of the file and pattern shown (and, with the target's option
   // checked, its product); openWhenDone: Execute's.
   void StartStack( bool openWhenDone );
   // Starts a Post run (the target's product) on the stack shown; openWhenDone: Execute's.
   void StartPost( bool openWhenDone );
   // Starts the worker of the run in m_run (both kinds).
   void StartWorker( bool openWhenDone );
   // The timer's tick: the progress, or, once the worker is done, its result.
   void PollStack();
   void FinishStack();

   // The target the window names (target parameter, else the file name's).
   TargetChoice Target() const;
   // The product the target's row offers (stackcards::PostKindFor).
   PostKind ActiveKind() const;
   // The kind's option (instance.p_mineralMoon, instance.p_planetProcess).
   pcl_bool& PostFlag( PostKind kind );
   bool PostChecked() const;   // the target's option is checked
   // Why the kind cannot run for the capture shown (nullopt: it can); with no
   // capture loaded, one must be opened first.
   std::optional<std::string> PostRefusal( PostKind kind ) const;
   // A capture or target that refuses an option turns it off (saying why in
   // the info block) and shows the stack.
   void ApplyPostRefusals();
   // A product made for another kind or planet than the target's now (a Target
   // change) is dropped: its cache, image and error.
   void DropStalePost();
   // The target's product and its slider values (pcl::Error when one is out of range).
   PostParams CurrentPost() const;
   // The preview: the product when its option is checked and it is made, else
   // the stack (at the zoom shown when it replaces the other).
   void ShowPreview();
   // The product at the current values: the one shown, or finished now from
   // the cache (FinishPost: the sharpening and colour steps only).
   planetary::stack::MineralOutcome CurrentPostOutcome() const;
   // Execute's windows: the stack, then (its option checked) the product.
   void OpenWindows();

   // The colour worker: started for the newest values (or queued behind the
   // one running); its result collected by the timer.
   void RequestColour();
   void PollColour();
   void JoinColour();
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
   void e_ValueUpdated( NumericEdit& sender, double value );
   void e_Timer( Timer& sender );
   void e_Show( Control& sender );
   void e_Hide( Control& sender );
};

extern PlanetaryStackInterface* ThePlanetaryStackInterface;

} // namespace pcl

#endif // __PlanetaryStackInterface_h
