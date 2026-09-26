// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "JourneyTracker.h"
#include "CopilotSettings.h"
#include "EvalGuard.h"
#include "JourneyConstants.h"
#include "MasterFacts.h"
#include "PjsrRunner.h"   // IsPjsrScriptRunning, ScriptLiteral
#include "PICopilotModule.h"
#include "ProcessActivity.h"   // CurrentProcessActivity (the busy gate)
#include "StepStats.h"
#include "ToolHelpers.h"   // IsBusy
#include "Utf8.h"

#include <pcl/Console.h>
#include <pcl/Exception.h>
#include <pcl/File.h>
#include <pcl/ImageVariant.h>
#include <pcl/ImageWindow.h>
#include <pcl/Settings.h>
#include <pcl/Thread.h>
#include <pcl/Variant.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>

#include <unistd.h>

namespace pcl
{

namespace
{

const char* const kRetentionLastRunKey = "PICopilot/JourneyRetentionLastRun";
constexpr double  kCopilotNoteSeconds = 30;
constexpr int     kCandidateTicks = 5;
constexpr size_t  kRecentSteps = 200;
constexpr int     kCandidateDeferred = 2;   // EvaluateCandidate(): nothing read yet (busy); not a wait tick
constexpr double  kTickBudgetMs = 100;      // per-tick work cap (review M8); the rest continues next tick

// The view was locked between the tick's probe and the tracker's own lock:
// the work is deferred (never waited on), not failed.
struct ViewBusy {};

double IsoToEpoch( const std::string& iso )
{
   int Y, M, D, h, m;
   double s;
   if ( std::sscanf( iso.c_str(), "%d-%d-%dT%d:%d:%lfZ", &Y, &M, &D, &h, &m, &s ) != 6 )
      return -1;
   std::tm tm = {};
   tm.tm_year = Y - 1900;
   tm.tm_mon = M - 1;
   tm.tm_mday = D;
   tm.tm_hour = h;
   tm.tm_min = m;
   return double( timegm( &tm ) ) + s;
}

std::string ViewIdOf( const View& v )
{
   return std::string( v.Id().c_str() );
}

std::string FilePathOf( const View& v )
{
   const ImageWindow w = v.Window();
   return w.IsNull() ? std::string() : U8( w.FilePath() );
}

size_type ModifyCountOf( const View& v )
{
   const ImageWindow w = v.Window();
   return w.IsNull() ? 0 : w.ModifyCount();
}

// Geometry and sample format for MasterFingerprint(). Probed non-waiting right
// before the lock (throws ViewBusy instead of blocking); the lock does not
// notify, so it is no "process activity" for the busy gate (review M2).
struct ViewGeom { int w = 0, h = 0, ch = 0, bits = 32; bool isFloat = true; };

ViewGeom ViewGeometry( const View& v )
{
   if ( IsBusy( v ) )
      throw ViewBusy();
   ViewGeom g;
   View vv = v;
   vv.LockForWrite( false/*notify*/ );
   struct Unlock { View& v; ~Unlock() { try { v.UnlockForWrite( false ); } catch ( ... ) {} } } unlock{ vv };
   ImageVariant iv = vv.Image();
   g.w = iv.Width();
   g.h = iv.Height();
   g.ch = iv.NumberOfChannels();
   g.bits = iv.BitsPerSample();
   g.isFloat = iv.IsFloatSample();
   return g;
}

std::vector<std::string> StepIdentities( const HistorySnapshot& snap )
{
   std::vector<std::string> ids;
   for ( const HistoryStep& s : snap.steps )
      ids.push_back( s.identity );
   return ids;
}

// Ruling 1 rule 1: the first non-Script step is an integration process.
bool HistoryBeginsWithIntegration( const HistorySnapshot& snap )
{
   for ( const HistoryStep& h : snap.steps )
      if ( h.processId != "Script" )
         return IsIntegrationProcess( h.processId );
   return false;
}

// Ruling 29 (pre-flight P35): an auxiliary output of a hand-run integration
// (rejection / slope / weight map) is never a master and never linked.
// IsIntegrationAuxiliary() (Task 6) compares the view id with the result id
// the first step's integrationImageId names. Two guards keep the result itself
// from being mistaken for an auxiliary output:
//  - an opened file: its view id comes from the file name, not from the run;
//  - the result renamed before the first tick: the result id no longer names
//    an open window, so nothing tells the outputs apart and none is dropped.
bool IsAuxiliaryOutput( const View& v, const HistorySnapshot& snap )
{
   if ( !FilePathOf( v ).empty() || !IsIntegrationAuxiliary( ViewIdOf( v ), snap.steps ) )
      return false;
   for ( const HistoryStep& h : snap.steps )
      if ( h.processId != "Script" )
         return !ImageWindow::WindowById( IsoString( h.integrationImageId.c_str() ) ).IsNull();
   return false;
}

void IdentifierTokens( const std::string& s, std::set<std::string>& out )
{
   std::string cur;
   for ( char c : s + " " )
      if ( std::isalnum( static_cast<unsigned char>( c ) ) || c == '_' )
         cur += c;
      else
      {
         if ( !cur.empty() && !std::isdigit( static_cast<unsigned char>( cur[0] ) ) )
            out.insert( cur );
         cur.clear();
      }
}

void CollectStrings( const nlohmann::json& j, std::set<std::string>& out )
{
   if ( j.is_string() )
      out.insert( j.get<std::string>() );
   else if ( j.is_array() || j.is_object() )
      for ( const nlohmann::json& e : j )
         CollectStrings( e, out );
}

// Removes the entries for which drop() is true. The containers hold
// unique_ptrs, so nothing here copies a View (PCL's attach throws on a closed
// window's handle, measured in the Task 7 RED run).
template <class T, class Drop>
void EraseIf( std::vector<std::unique_ptr<T>>& v, Drop drop )
{
   v.erase( std::remove_if( v.begin(), v.end(), [&drop]( const std::unique_ptr<T>& e ) { return drop( *e ); } ), v.end() );
}

} // namespace

double JourneyWallNow()
{
   return std::chrono::duration<double>( std::chrono::system_clock::now().time_since_epoch() ).count();
}

std::string LocalDateToday()
{
   const std::time_t t = std::time( nullptr );
   std::tm tm;
   localtime_r( &t, &tm );
   char buf[64];
   std::snprintf( buf, sizeof buf, "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday );
   return buf;
}

std::string JourneyOwnerOf( long pid )
{
   if ( pid <= 0 )
      pid = long( ::getpid() );
   std::ifstream f( "/proc/" + std::to_string( pid ) + "/stat" );
   std::string line;
   if ( !f || !std::getline( f, line ) )
      return std::string();
   // Field 22 (starttime) counted after the ")" that ends the command name.
   const size_t close = line.rfind( ')' );
   if ( close == std::string::npos )
      return std::string();
   std::istringstream rest( line.substr( close + 1 ) );
   std::string field;
   for ( int i = 3; i <= 22; ++i )
      if ( !(rest >> field) )
         return std::string();
   return std::to_string( pid ) + ":" + field;
}

bool JourneyOwnerAlive( const std::string& owner )
{
   const size_t colon = owner.find( ':' );
   if ( owner.empty() || colon == std::string::npos )
      return false;
   const long pid = std::strtol( owner.substr( 0, colon ).c_str(), nullptr, 10 );
   return pid > 0 && JourneyOwnerOf( pid ) == owner;
}

// ---- JourneyTracker ----------------------------------------------------------

JourneyTracker::JourneyTracker( JourneyStore* store, const String& storeError )
   : m_store( store ), m_storeError( storeError ),
     m_useModifyCount( PICopilotJourneyScanUsesModifyCount && PICopilotJourneyNotificationsWork ),
     m_owner( JourneyOwnerOf() ),
     m_read( ReadViewHistory )
{
   // ModifyCount resets on save; a save + step inside one scan interval is only
   // caught through ImageSaved, so ModifyCount scanning needs notifications.
}

JourneyTracker::~JourneyTracker() = default;

void JourneyTracker::SetStore( JourneyStore* store, const String& storeError )
{
   m_store = store;
   m_storeError = storeError;
}

void JourneyTracker::SetEnabled( bool on )
{
   if ( on && !m_enabled )
   {
      m_forceScan = true;
      m_scannedOnce = false;   // R3: windows first seen at the re-enable scan existed before: not fresh
   }
   if ( !on )
      m_events.clear();   // recording off: queued notifications are not applied
   m_enabled = on;
}

void JourneyTracker::SetHistoryReaderForSelfTest( HistoryReadFn fn )
{
   m_read = fn ? fn : HistoryReadFn( ReadViewHistory );
}

JourneyTracker::Tracked* JourneyTracker::FindTracked( const View& v )
{
   for ( const std::unique_ptr<Tracked>& t : m_tracked )
      if ( t->view == v )
         return t.get();
   return nullptr;
}

const JourneyTracker::Tracked* JourneyTracker::FindTrackedById( const std::string& id ) const
{
   for ( const std::unique_ptr<Tracked>& t : m_tracked )
      if ( t->id == id )
         return t.get();
   return nullptr;
}

bool JourneyTracker::IsCandidate( const View& v ) const
{
   for ( const std::unique_ptr<Candidate>& c : m_candidates )
      if ( c->view == v )
         return true;
   return false;
}

void JourneyTracker::AddCandidate( const View& v, double now, bool fresh )
{
   if ( IsCandidate( v ) )
      return;
   std::unique_ptr<Candidate> c( new Candidate );
   c->view = v;
   c->firstSeen = now;
   c->fresh = fresh;
   m_candidates.push_back( std::move( c ) );
}

void JourneyTracker::Forget( const View& v )
{
   EraseIf( m_candidates, [&v]( const Candidate& c ) { return c.view == v; } );
   EraseIf( m_ignored, [&v]( const Ignored& i ) { return i.view == v; } );
}

bool JourneyTracker::OverTickBudget() const
{
   return std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - m_tickStart ).count() > kTickBudgetMs;
}

void JourneyTracker::Decision( const std::string& line )
{
   m_decisions.push_back( line );
   while ( m_decisions.size() > 100 )
      m_decisions.pop_front();
}

size_type JourneyTracker::PendingCount() const
{
   size_type n = m_events.size() + m_candidates.size() + m_pendingGaps.size();
   for ( const std::unique_ptr<Tracked>& t : m_tracked )
      if ( t->dirty )
         ++n;
   return n;
}

StringList JourneyTracker::TakeJoinNotes()
{
   StringList n = m_joinNotes;
   m_joinNotes.Clear();
   return n;
}

// Notification handlers: queue only (global constraint; pre-flight P10). They
// never touch m_tracked, m_candidates or m_ignored, so nothing Tick() iterates
// can change under it, even when a process or a script inside Tick() pumps
// events. DrainEvents() applies the queue at the start of the next Tick().

void JourneyTracker::QueueEvent( EventKind kind, const View& v, double now )
{
   if ( !m_enabled || v.IsNull() )
      return;   // off: nothing is read or stored; SetEnabled( true ) forces a full scan
   if ( m_events.size() >= 1000 )
   {
      m_events.pop_front();   // bounded; the forced scan re-derives whatever was dropped
      m_forceScan = true;
   }
   m_events.push_back( { kind, v, now } );
}

void JourneyTracker::OnImageCreated( const View& view, double now ) { QueueEvent( EventKind::Created, view, now ); }
void JourneyTracker::OnImageUpdated( const View& view, double now ) { QueueEvent( EventKind::Updated, view, now ); }
void JourneyTracker::OnImageRenamed( const View& view, double now ) { QueueEvent( EventKind::Renamed, view, now ); }
void JourneyTracker::OnImageDeleted( const View& view, double now ) { QueueEvent( EventKind::Deleted, view, now ); }
void JourneyTracker::OnImageSaved( const View& view, double now )   { QueueEvent( EventKind::Saved, view, now ); }
void JourneyTracker::OnImageFocused( const View& view, double now ) { QueueEvent( EventKind::Focused, view, now ); }

// Called only at the start of Tick(), after DropClosed().
void JourneyTracker::DrainEvents()
{
   while ( !m_events.empty() )
   {
      // By reference, and always popped: an event whose window closed since
      // it was queued throws on any API call, and must never wedge the queue.
      const PendingEvent& e = m_events.front();
      try
      {
         ApplyEvent( e, e.view );
      }
      catch ( ... )
      {
         m_forceScan = true;   // nothing about a closed view needs recording; the scan re-derives the rest
      }
      m_events.pop_front();
   }
}

void JourneyTracker::ApplyEvent( const PendingEvent& e, const View& view )
{
   switch ( e.kind )
   {
   case EventKind::Created:
      if ( !view.IsPreview() && FindTracked( view ) == nullptr )
         AddCandidate( view, e.t, true/*fresh: it appeared*/ );
      break;
   case EventKind::Updated:
      if ( view.IsPreview() )
         break;   // Ruling 25: previews are not part of the image's journey
      if ( Tracked* t = FindTracked( view ) )
      {
         t->dirty = true;
         break;
      }
      {
         bool wasIgnored = false;
         for ( const std::unique_ptr<Ignored>& i : m_ignored )
            wasIgnored = wasIgnored || i->view == view;
         if ( wasIgnored )
         {
            EraseIf( m_ignored, [&view]( const Ignored& x ) { return x.view == view; } );
            // Re-evaluated (a new step may now reference a tracked view), but it did not APPEAR now:
            // never linked by timing (c) (review I3).
            AddCandidate( view, e.t, false );
         }
      }
      break;
   case EventKind::Renamed:
      m_forceScan = true;   // the scan compares ids of the SAME view objects
      m_renameSinceScan = true;
      break;
   case EventKind::Deleted:
      m_forceScan = true;   // DropClosed() (every tick) removes it and ends its journey if it was the last open image
      break;
   case EventKind::Saved:
      if ( Tracked* t = FindTracked( view ) )
         t->dirty = true;   // ModifyCount was reset; the file path may have changed
      m_forceScan = true;
      break;
   case EventKind::Focused:
      NoteActive( ViewIdOf( view.IsPreview() ? view.Window().MainView() : view ), e.t );   // the event's own time
      break;
   }
}

void JourneyTracker::NoteActive( const std::string& id, double now )
{
   if ( id.empty() || (!m_active.empty() && m_active.back().first == id) )
      return;
   m_active.push_back( { id, now } );
   while ( m_active.size() > 200 )
      m_active.pop_front();
}

void JourneyTracker::NoteCopilotStep( const IsoString& viewFullId, const std::string& processId, const std::string& reason,
                                      const std::vector<std::string>& createdWindowIds, bool integration, double now )
{
   const std::string vid( viewFullId.c_str() );
   if ( !vid.empty() )
      m_copilot.push_back( { vid, processId, reason, now } );
   // Ruling 29: of an integration run's windows only the first (the result) may become a master.
   for ( size_t i = 0; i < createdWindowIds.size(); ++i )
      m_created.push_back( { createdWindowIds[i], vid, integration, i == 0, now } );
   for ( const std::unique_ptr<Tracked>& t : m_tracked )
      if ( t->id == vid )
         t->dirty = true;
   m_forceScan = true;
}

int JourneyTracker::FindCopilotNote( const std::string& viewId, const std::string& processId, double now,
                                     const std::vector<int>& taken ) const
{
   for ( size_t i = 0; i < m_copilot.size(); ++i )
      if ( m_copilot[i].viewId == viewId && m_copilot[i].processId == processId && now - m_copilot[i].t <= kCopilotNoteSeconds
        && std::find( taken.begin(), taken.end(), int( i ) ) == taken.end() )
         return int( i );
   return -1;
}

void JourneyTracker::Tick( double now, bool forceScan )
{
   if ( !m_enabled || m_inTick )
      return;
   // Never nests an EvaluateScript (history reads, the batch scan) inside a
   // model-written run_pjsr script or another module EvaluateScript
   // (controller ruling after Task 1; EvalGuard.h).
   if ( IsPjsrScriptRunning() || EvaluateScriptDepth() > 0 || (m_store != nullptr && m_store->InTransaction()) )
   {
      ++m_deferrals;
      return;
   }
   struct Guard { bool& b; explicit Guard( bool& x ) : b( x ) { b = true; } ~Guard() { b = false; } } guard( m_inTick );
   if ( m_store == nullptr )
   {
      m_pausedReason = m_storeError;
      return;
   }
   m_tickStart = std::chrono::steady_clock::now();
   try
   {
      std::vector<View> open;
      std::vector<size_type> counts;
      DropClosed( open, counts );   // first: reads nothing
      DrainEvents();                // reads nothing; its focus changes are older than the sample below
      // The busy gate (spec §4/§9, review I4, re-review R4): no history read,
      // lock or DB write while a process or script runs (console abort) or
      // right after one (the quiet period). Deferred, never waited on; a long
      // stall becomes visible through StatusFor().
      {
         const ProcessActivityState a = RecorderActivity();
         if ( a.busy )
         {
            ++m_deferrals;
            if ( m_gateSince == 0 )
               m_gateSince = now;
            m_gateReason = a.reason;
            if ( !m_gateNoted && now - m_gateSince >= 5 )
            {
               m_joinNotes << "PI Copilot: journey recording is waiting: " + a.reason;   // once per stall
               m_gateNoted = true;
            }
            return;
         }
         if ( m_gateNoted )
            m_joinNotes << String( "PI Copilot: journey recording continues." );
         m_gateSince = 0;
         m_gateNoted = false;
         m_gateReason.Clear();
      }
      {
         const ImageWindow aw = ImageWindow::ActiveWindow();
         if ( !aw.IsNull() )
            NoteActive( ViewIdOf( aw.MainView() ), now );
      }
      if ( forceScan || m_forceScan || now - m_lastScan >= PICopilotJourneyScanSeconds )
      {
         m_lastScan = now;       // before the scan: a failing scan can never make every tick rescan (R2)
         m_forceScan = false;
         Scan( now, open, counts );
      }
      FlushPendingGaps();
      ProcessDirty( now );
      ProcessCandidates( now );
      EndClosedJourneys();
      m_copilot.erase( std::remove_if( m_copilot.begin(), m_copilot.end(),
                                       [now]( const CopilotNote& n ) { return now - n.t > kCopilotNoteSeconds; } ), m_copilot.end() );
      m_created.erase( std::remove_if( m_created.begin(), m_created.end(),
                                       [now]( const CreatedNote& n ) { return now - n.t > kCopilotNoteSeconds; } ), m_created.end() );
      m_pausedReason.Clear();   // a complete tick: no tick-wide failure is current
   }
   catch ( const pcl::Exception& x )
   {
      m_pausedReason = x.Message();
   }
   catch ( const std::exception& x )
   {
      m_pausedReason = String( x.what() );
   }
}

void JourneyTracker::DropClosed( std::vector<View>& open, std::vector<size_type>& counts )
{
   for ( const ImageWindow& w : ImageWindow::AllWindows() )
   {
      open.push_back( w.MainView() );
      counts.push_back( w.ModifyCount() );
   }
   auto isOpen = [&open]( const View& v )
   {
      for ( const View& o : open )
         if ( o == v )   // handle comparison: no API call, safe on a closed view
            return true;
      return false;
   };
   for ( const std::unique_ptr<Tracked>& t : m_tracked )
      if ( !isOpen( t->view ) )
      {
         m_closed.push_back( t->journeyId );
         m_closedImages.push_back( t->imageId );
         // R4: closed with changes not recorded yet (dirty, or an Updated notification still queued) --
         // e.g. during a busy stall: the lost steps become a gap, never silently nothing.
         bool pending = t->dirty;
         for ( const PendingEvent& e : m_events )
            pending = pending || (e.kind == EventKind::Updated && e.view == t->view);
         if ( pending )
         {
            int lastSeq = 0;
            try
            {
               for ( const StepRow& r : m_store->Steps( t->imageId, false ) )
                  lastSeq = std::max( lastSeq, r.seq );
            }
            catch ( ... )
            {
            }
            m_pendingGaps.push_back( { t->journeyId, t->imageId, lastSeq,
                                       "closed while PixInsight was busy; its last steps were not recorded" } );
         }
      }
   EraseIf( m_tracked, [&]( const Tracked& t ) { return !isOpen( t.view ); } );
   EraseIf( m_candidates, [&]( const Candidate& c ) { return !isOpen( c.view ); } );
   EraseIf( m_ignored, [&]( const Ignored& i ) { return !isOpen( i.view ); } );
}

void JourneyTracker::Scan( double now, const std::vector<View>& open, const std::vector<size_type>& counts )
{
   for ( size_t i = 0; i < open.size(); ++i )
   {
      const View& v = open[i];
      if ( Tracked* t = FindTracked( v ) )
      {
         const std::string id = ViewIdOf( v );
         if ( id != t->id )
         try
         {
            // Review M9: an id change with no rename notification since the last scan may be a
            // reused window handle rather than a rename; recorded for diagnosis.
            if ( !m_renameSinceScan && PICopilotJourneyNotificationsWork )
            {
               char head[48];
               std::snprintf( head, sizeof head, "%.3f ", now );
               Decision( head + std::string( "id change without a rename notification: " ) + t->id + " -> " + id );
            }
            m_store->SetImageView( t->imageId, id, FilePathOf( v ) );
            t->id = id;
         }
         catch ( const JourneyRowMissing& x )
         {
            for ( size_t k = 0; k < m_tracked.size(); ++k )
               if ( m_tracked[k].get() == t )
               {
                  DropMissing( k, x.Message() );
                  break;
               }
            continue;
         }
         catch ( const pcl::Exception& x )
         {
            t->pausedReason = x.Message();   // this image only; the scan goes on
            continue;
         }
         if ( m_useModifyCount && counts[i] != t->modifyCount )
         {
            t->dirty = true;
            t->modifyCount = counts[i];
         }
         continue;
      }
      if ( IsCandidate( v ) )
         continue;
      bool ignored = false;
      for ( const std::unique_ptr<Ignored>& ig : m_ignored )
         if ( ig->view == v )
         {
            ignored = true;
            if ( ig->modifyCount != counts[i] )
            {
               EraseIf( m_ignored, [&v]( const Ignored& x ) { return x.view == v; } );
               AddCandidate( v, now, false/*changed, not new: no timing (c)*/ );
            }
            break;
         }
      if ( !ignored )
         AddCandidate( v, now, m_scannedOnce/*never seen before; not fresh when it was open before recording began*/ );
   }
   m_renameSinceScan = false;
   m_scannedOnce = true;
   if ( !m_useModifyCount )
      BatchCounts();
}

void JourneyTracker::BatchCounts()
{
   // Busy views are not read (global constraint, pre-flight P8): they are
   // marked dirty, and ProcessDirty() defers them until they are free.
   std::vector<Tracked*> probed;
   std::string ids = "[";
   for ( const std::unique_ptr<Tracked>& t : m_tracked )
   {
      if ( IsBusy( t->view ) )
      {
         t->dirty = true;
         continue;
      }
      ids += (probed.empty() ? "" : ",") + ScriptLiteral( String( t->id.c_str() ) );
      probed.push_back( t.get() );
   }
   ids += "]";
   if ( probed.empty() )
      return;
   const String js = String( "(function( ids ){ var r = [];"
      " ids.forEach( function( id ) { var v = null; try { v = View.viewById( id ); } catch ( e ) { v = null; }"
      "   r.push( v == null || v.isNull ? [ -1, -1, -1 ] : [ v.initialProcessing.length, v.processing.length, v.historyIndex ] ); } );"
      " return JSON.stringify( r ); })( " ) + String( ids.c_str() ) + " )";
   nlohmann::json r;
   {
      EvalDepthGuard guard;   // a timer-driven reader defers instead of nesting inside this call
      r = nlohmann::json::parse( U8( ThePICopilotModule->EvaluateScript( js, "JavaScript" ).ToString() ) );
   }
   for ( size_t k = 0; k < probed.size() && k < r.size(); ++k )
   {
      const std::vector<int> c = r.at( k ).get<std::vector<int>>();
      if ( c != probed[k]->lastCounts )
         probed[k]->dirty = true;
   }
}

void JourneyTracker::Remember( const HistoryStep& h, int64 imageId, int64 journeyId, int64 stepId )
{
   const double start = IsoToEpoch( h.started );
   m_recent.push_back( { h.identity, imageId, journeyId, stepId, start, start < 0 ? -1 : start + std::max( 0.0, h.durationS ) } );
   while ( m_recent.size() > kRecentSteps )
      m_recent.pop_front();
}

void JourneyTracker::ProcessDirty( double now )
{
   bool processed = false;
   std::vector<std::pair<size_t, String>> gone;   // R2: images whose journey rows vanished
   for ( size_t k = 0; k < m_tracked.size(); ++k )
   {
      Tracked& t = *m_tracked[k];
      if ( !t.dirty )
         continue;
      if ( processed && OverTickBudget() )
         break;   // review M8: the rest stays dirty for the next tick
      if ( IsBusy( t.view ) )
      {
         ++m_deferrals;   // never waited on: the next tick tries again
         continue;
      }
      processed = true;
      // R2: each image's DB work is its own failure domain. A vanished row drops that image; any other
      // failure pauses that image only; the loop goes on with the next image.
      try
      {
         ProcessOne( t, now );
      }
      catch ( const JourneyRowMissing& x )
      {
         gone.push_back( { k, x.Message() } );
      }
      catch ( const pcl::Exception& x )
      {
         if ( x.Message().Contains( "FOREIGN KEY constraint failed" ) )
            gone.push_back( { k, x.Message() } );
         else
            t.pausedReason = x.Message();
      }
      catch ( const std::exception& x )
      {
         t.pausedReason = String( x.what() );
      }
   }
   for ( auto it = gone.rbegin(); it != gone.rend(); ++it )
      DropMissing( it->first, it->second );
}

void JourneyTracker::ProcessOne( Tracked& t, double now )
{
   const auto t0 = std::chrono::steady_clock::now();
   const std::vector<StepRow> rows = m_store->Steps( t.imageId, true );
   std::vector<KnownStep> known;
   int lastSeq = 0;
   for ( const StepRow& r : rows )
   {
      known.push_back( { r.id, r.seq, r.params.value( "identity", std::string() ), r.state } );
      if ( r.state != "superseded" )
         lastSeq = std::max( lastSeq, r.seq );
   }
   HistorySnapshot snap = m_read( IsoString( t.id.c_str() ), HistoryReadFrom( known ) );
   HistoryDiff d;
   if ( snap.ok )
   {
      d = DiffHistory( known, snap );
      if ( d.needFullRead )
      {
         snap = m_read( IsoString( t.id.c_str() ), 0 );
         if ( snap.ok )
            d = DiffHistory( known, snap );
      }
   }
   if ( snap.busy )
   {
      ++m_deferrals;   // another EvaluateScript is running: nothing was read; retried on the next tick
      return;
   }
   if ( !snap.ok )
   {
      t.pausedReason = snap.error;   // this image only (review M3)
      if ( ++t.readFailures >= 3 )
      {
         m_pendingGaps.push_back( { t.journeyId, t.imageId, lastSeq, PICopilotJourneyReadGapPrefix + U8( snap.error ) } );
         t.hasGaps = true;
         t.readFailures = 0;
         t.dirty = false;   // retried on the next change
         FlushPendingGaps();
      }
      return;
   }
   std::vector<int> notes;          // m_copilot indices consumed, erased after the commit
   std::vector<int64> appendedIds;
   int64 lastActive = 0;
   const bool anything = !d.toActive.empty() || !d.toUndone.empty() || !d.toSuperseded.empty() || !d.appended.empty();
   // One transaction per image and tick (review I5): all state changes and new steps land together or not
   // at all. Nothing to write and no gap to resolve: no transaction, no write lock (re-review m2).
   if ( anything || t.hasGaps )
   {
      JourneyStore::Transaction tx( *m_store );
      int maxSeq = lastSeq;
      for ( int64 id : d.toActive )     m_store->SetStepState( id, "active" );
      for ( int64 id : d.toUndone )     m_store->SetStepState( id, "undone" );
      for ( int64 id : d.toSuperseded ) m_store->SetStepState( id, "superseded" );
      for ( size_t i = 0; i < d.appended.size(); ++i )
      {
         const HistoryStep& h = d.appended[i];
         const int n = FindCopilotNote( t.id, h.processId, now, notes );
         if ( n >= 0 )
            notes.push_back( n );
         const int64 sid = m_store->AddStep( MakeStepRow( h, t.imageId, d.appendedState[i], n >= 0 ? "copilot" : "user",
                                                          n >= 0 ? m_copilot[n].reason : std::string(), snap.ActiveCount() ) );
         appendedIds.push_back( sid );
         maxSeq = std::max( maxSeq, h.combinedIndex + 1 );
         if ( d.appendedState[i] == "active" )
            lastActive = sid;
      }
      if ( anything )
         m_store->TouchJourney( t.journeyId, NowIso() );
      // Review M4 / re-review m3: the read succeeded, so the steps a read-failure gap stood for are known
      // again and recorded up to maxSeq; those gaps are resolved. Other gaps stay.
      if ( t.hasGaps )
         m_store->ResolveGaps( t.imageId, maxSeq + 1 );
      tx.Commit();
   }
   t.hasGaps = false;
   m_pendingGaps.erase( std::remove_if( m_pendingGaps.begin(), m_pendingGaps.end(),
                                        [&t]( const GapRow& g )
                                        { return g.imageId == t.imageId && g.reason.rfind( PICopilotJourneyReadGapPrefix, 0 ) == 0; } ),
                        m_pendingGaps.end() );
   std::sort( notes.begin(), notes.end() );
   for ( auto it = notes.rbegin(); it != notes.rend(); ++it )
      m_copilot.erase( m_copilot.begin() + *it );
   for ( size_t i = 0; i < d.appended.size(); ++i )
      Remember( d.appended[i], t.imageId, t.journeyId, appendedIds[i] );
   t.pausedReason.Clear();   // the history read and the DB writes succeeded
   t.dirty = false;
   t.readFailures = 0;
   t.lastCounts = { snap.initialLength, snap.length, snap.historyIndex };
   if ( lastActive != 0 )
   {
      // Ruling 7: only the last active step appended in this tick is measured. Steps made between two
      // observations share this one measurement; the intermediate pixels no longer exist. A failure here
      // is this image's statistics pause (P11, re-review m4), never a lost step.
      try
      {
         const StepStatsResult s = ComputeStepStats( t.view, m_store->JourneyDir( t.journeyId )
                                                     + String().Format( "/thumbs/%lld.jpg", static_cast<long long>( lastActive ) ) );
         if ( !s.ok )
            throw Error( s.error );
         m_store->AddStats( t.imageId, lastActive, s.channels );
         t.statsReason.Clear();
      }
      catch ( const pcl::Exception& x )
      {
         t.statsReason = "statistics not recorded: " + x.Message();
      }
   }
   m_lastStepMs = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
}

void JourneyTracker::DropMissing( size_t index, const String& why )
{
   const std::unique_ptr<Tracked>& t = m_tracked[index];
   const View view = t->view;   // an OPEN window (the scan / tick just saw it): safe to copy
   const std::string id = t->id;
   m_joinNotes << "PI Copilot: the journey of " + String( id.c_str() )
                  + " was removed from the library; recording of " + String( id.c_str() ) + " stopped (" + why + ")";
   Decision( "dropped " + id + ": " + U8( why ) );
   m_tracked.erase( m_tracked.begin() + index );
   AddCandidate( view, JourneyWallNow(), false/*not fresh: it existed; it may start a new journey by the rules*/ );
}

void JourneyTracker::ProcessCandidates( double now )
{
   bool evaluated = false;
   for ( size_t i = 0; i < m_candidates.size(); )
   {
      Candidate& c = *m_candidates[i];
      if ( evaluated && OverTickBudget() )
         break;   // review M8: the rest waits for the next tick (not counted as a look)
      if ( IsBusy( c.view ) )
      {
         ++m_deferrals;
         ++i;
         continue;
      }
      evaluated = true;
      m_evalNote.clear();
      int r;
      try
      {
         r = EvaluateCandidate( c, now );
      }
      catch ( const ViewBusy& )
      {
         r = kCandidateDeferred;   // locked between the probe and the geometry read
      }
      {
         char head[96];
         std::snprintf( head, sizeof head, "%.3f r=%d ticks=%d ", now, r, c.ticks );
         Decision( head + ViewIdOf( c.view ) + " " + m_evalNote );
      }
      if ( r == kCandidateDeferred )
      {
         ++m_deferrals;   // nothing decided: not counted as a wait tick
         ++i;
      }
      else if ( r > 0 )
         m_candidates.erase( m_candidates.begin() + i );
      else if ( r == 0 && ++c.ticks < kCandidateTicks )
         ++i;
      else
      {
         std::unique_ptr<Ignored> ig( new Ignored );
         ig->view = c.view;
         ig->modifyCount = ModifyCountOf( c.view );
         m_ignored.push_back( std::move( ig ) );
         m_candidates.erase( m_candidates.begin() + i );
      }
   }
}

std::vector<const JourneyTracker::Tracked*> JourneyTracker::ReferencedTracked( const HistoryStep& h ) const
{
   std::set<std::string> words;
   CollectStrings( h.parameters, words );
   CollectStrings( h.tableParameters, words );
   std::set<std::string> tokens = words;
   for ( const char* p : { "expression", "expression1", "expression2", "expression3" } )
      if ( h.parameters.contains( p ) && h.parameters.at( p ).is_string() )
         IdentifierTokens( h.parameters.at( p ).get<std::string>(), tokens );
   std::vector<const Tracked*> r;
   for ( const std::unique_ptr<Tracked>& t : m_tracked )
      if ( tokens.count( t->id ) > 0 )
         r.push_back( t.get() );
   return r;
}

int JourneyTracker::EvaluateCandidate( Candidate& c, double now )
{
   if ( FindTracked( c.view ) != nullptr )
      return 1;   // review M1: already joined (never twice)
   const std::string id = ViewIdOf( c.view );
   // The history: re-read on the first two looks and whenever the window
   // changed; otherwise the last read is re-evaluated against the tracker's
   // current state (new tracked views, Copilot notes) at no cost (review M8).
   const size_type mc = ModifyCountOf( c.view );
   if ( !c.hasSnap || c.ticks < 2 || mc != c.modifyCount )
   {
      HistorySnapshot s = m_read( IsoString( id.c_str() ), 0 );
      if ( s.busy )
         return kCandidateDeferred;
      if ( !s.ok )
      {
         m_evalNote = "read: " + U8( s.error );
         return 0;
      }
      c.snap = std::move( s );
      c.hasSnap = true;
      c.modifyCount = mc;
   }
   const HistorySnapshot& snap = c.snap;
   m_evalNote = "init=" + std::to_string( snap.initialLength ) + " len=" + std::to_string( snap.length )
              + " first=" + (snap.steps.empty() ? std::string() : snap.steps.front().processId + "@" + snap.steps.front().started);
   // (1) copilot: a Copilot tool reported this window (Ruling 19.1 / 1.5).
   for ( const CreatedNote& n : m_created )
      if ( n.id == id && now - n.t <= kCopilotNoteSeconds )
      {
         if ( n.integration )
         {
            if ( !n.first )
               return -1;   // Ruling 29: an auxiliary output of Copilot's integration run
            return JoinAsMaster( c.view, snap, c.view.Window().Keywords(),
                                 "created by Copilot's run_global_process of an integration process", c.firstSeen ) != 0 ? 1 : -1;
         }
         if ( const Tracked* src = FindTrackedById( n.sourceViewId ) )
         {
            const int64 srcImage = src->imageId, srcJourney = src->journeyId;
            const std::vector<StepRow> steps = m_store->Steps( srcImage, false );
            return JoinLinked( c.view, snap, srcJourney, { { srcImage, steps.empty() ? 0 : steps.back().id, 0 } },
                               "copilot", "linked by copilot" ) != 0 ? 1 : -1;
         }
      }
   // Ruling 29: an auxiliary output of a hand-run integration is never tracked.
   if ( IsAuxiliaryOutput( c.view, snap ) )
      return -1;
   std::vector<std::string> ids;
   for ( const HistoryStep& h : snap.steps )
      ids.push_back( h.processId );
   const FITSKeywordArray kw = c.view.Window().Keywords();
   const MasterEvidence me = DetectMaster( ids, kw );
   // (2) a master of its own. Ruling 28 (pre-flight P14): a window CREATED from another one (initialLength > 0)
   //     whose history does not begin with an integration is decided by link evidence first, and by the
   //     keyword master rules (Ruling 1 rules 2-4) only when no link evidence (except timing (c)) exists,
   //     so a derived window that inherited IMAGETYP='Master Light' joins its source's journey.
   const bool derived = snap.initialLength > 0 && !HistoryBeginsWithIntegration( snap );
   if ( me.isMaster && !derived )
      return JoinAsMaster( c.view, snap, kw, me.why, c.firstSeen ) != 0 ? 1 : -1;
   // (3) reference (Ruling 19.2; spec §13.6: explicit references before timing): a step names tracked views.
   //     The creating step (initialProcessing[0], spec §5 evidence 3, pre-flight P13) and the view's own steps
   //     count; the rest of an opened file's initialProcessing does not.
   for ( const HistoryStep& h : snap.steps )
   {
      if ( h.combinedIndex < snap.initialLength && h.combinedIndex != 0 )
         continue;
      const std::vector<const Tracked*> refs = ReferencedTracked( h );
      if ( refs.empty() )
         continue;
      const int64 journeyId = refs.front()->journeyId;
      std::vector<PlannedLink> links;
      for ( const Tracked* t : refs )
         if ( t->journeyId == journeyId )
            links.push_back( { t->imageId, 0, h.combinedIndex + 1 } );
      return JoinLinked( c.view, snap, journeyId, links, "reference", "linked by reference" ) != 0 ? 1 : -1;
   }
   // (4) timing (a)/(b) (Ruling 19.3).
   if ( snap.initialLength > 0 )
   {
      // (a) the creating step is a recorded step (it also changed its source).
      for ( const RecentStep& r : m_recent )
         if ( r.identity == snap.steps.front().identity )
            return JoinLinked( c.view, snap, r.journeyId, { { r.imageId, r.stepId, 0 } }, "timing", "linked by timing" ) != 0 ? 1 : -1;
      // (b) the creating step started while a tracked view was the active view.
      const double ts = IsoToEpoch( snap.steps.front().started );
      if ( ts > 0 )
      {
         std::string activeId;
         for ( const auto& a : m_active )   // chronological
            if ( a.second <= ts + 0.05 )
               activeId = a.first;
            else
               break;
         m_evalNote += " activeAtStart=" + activeId;
         const Tracked* src = FindTrackedById( activeId );
         if ( src != nullptr && !(src->view == c.view) )
         {
            const int64 srcImage = src->imageId, srcJourney = src->journeyId;
            const std::vector<StepRow> steps = m_store->Steps( srcImage, false );
            return JoinLinked( c.view, snap, srcJourney, { { srcImage, steps.empty() ? 0 : steps.back().id, 0 } },
                               "timing", "linked by timing" ) != 0 ? 1 : -1;
         }
      }
   }
   // (5) Ruling 28: a derived window with master keywords and no link evidence is a master of its own.
   if ( me.isMaster )
      return JoinAsMaster( c.view, snap, kw, me.why, c.firstSeen ) != 0 ? 1 : -1;
   // (6) timing (c), last: a window that APPEARED (review I3: a Created notification or the first sight of
   //     a view, never an ignored window that changed later) and is no opened file, first seen inside
   //     exactly one recorded step's time window. Never decided on the first look, and 2+ hits wait
   //     instead of rejecting: a window can be seen by a tick running while its creating process still
   //     executes, before its creating step is attached (J6 (f3)). Still ignored after kCandidateTicks.
   if ( !c.fresh || !FilePathOf( c.view ).empty() )
   {
      m_evalNote += c.fresh ? " timingC.no(file)" : " timingC.no(not new)";
      return 0;
   }
   if ( c.ticks == 0 )
   {
      m_evalNote += " timingC.deferredToNextLook";
      return 0;
   }
   const RecentStep* hit = nullptr;
   int hits = 0;
   for ( const RecentStep& r : m_recent )
      if ( r.start > 0 && c.firstSeen >= r.start - 0.5 && c.firstSeen <= r.end + PICopilotJourneyTimingSlackSeconds )
      {
         hit = &r;
         ++hits;
      }
   m_evalNote += " timingC.hits=" + std::to_string( hits );
   if ( hits == 1 )
      return JoinLinked( c.view, snap, hit->journeyId, { { hit->imageId, hit->stepId, 0 } }, "timing", "linked by timing" ) != 0 ? 1 : -1;
   return 0;
}

// Records every step of an image joining a journey (inside the join's
// transaction); the first baseCount (combined index) are "base": the image's
// own starting history (a master's stacking, a derived window's creating step
// already recorded on its source). Base steps keep the diff aligned but never
// reach recipes, counts or replay (spec D2). Returns the step ids in order.
std::vector<int64> JourneyTracker::AddBaseAndSteps( int64 imageId, const HistorySnapshot& snap, int baseCount )
{
   std::vector<int64> ids;
   for ( const HistoryStep& h : snap.steps )
   {
      const std::string state = h.combinedIndex + 1 <= snap.ActiveCount() ? "active" : "undone";
      StepRow r = MakeStepRow( h, imageId, state, "user", "", snap.ActiveCount() );
      if ( h.combinedIndex < baseCount )
         r.params["base"] = true;
      ids.push_back( m_store->AddStep( r ) );
   }
   return ids;
}

void JourneyTracker::StartingStats( Tracked& t )
{
   try
   {
      const StepStatsResult s = ComputeStepStats( t.view, m_store->JourneyDir( t.journeyId )
                                                  + String().Format( "/thumbs/start-%lld.jpg", static_cast<long long>( t.imageId ) ) );
      if ( !s.ok )
         throw Error( s.error );
      m_store->AddStats( t.imageId, 0, s.channels );
      t.statsReason.Clear();
   }
   catch ( const pcl::Exception& x )
   {
      t.statsReason = "starting statistics not recorded: " + x.Message();   // a pause (P11), cleared by the next success
   }
}

int64 JourneyTracker::JoinAsMaster( const View& v, const HistorySnapshot& snap, const FITSKeywordArray& kw, const std::string& why,
                                    double firstSeen )
{
   const std::string id = ViewIdOf( v );
   const std::string path = FilePathOf( v );
   const ViewGeom g = ViewGeometry( v );
   const std::vector<std::string> identities = StepIdentities( snap );
   std::unique_ptr<Tracked> t( new Tracked );
   t->view = v;
   t->id = id;
   t->modifyCount = ModifyCountOf( v );
   t->why = why;
   // Resume (save + reopen): the journey's fingerprint over a prefix of today's history. Images open and
   // recorded right now are never resumed (review I2: a second window of the same history-less master).
   std::vector<int64> exclude;
   for ( const std::unique_ptr<Tracked>& o : m_tracked )
      exclude.push_back( o->imageId );
   for ( size_t p = 0; p <= identities.size(); ++p )
   {
      ImageRow row;
      bool found = false;
      const std::vector<std::string> prefix( identities.begin(), identities.begin() + p );
      for ( const ImageRow& r : m_store->ResumableByFingerprint( MasterFingerprint( g.w, g.h, g.ch, g.bits, g.isFloat, prefix, kw ),
                                                                 exclude ) )
         // Re-review m6: a row another RUNNING PixInsight instance records is not ours to continue.
         if ( r.owner.empty() || r.owner == m_owner || !JourneyOwnerAlive( r.owner ) )
         {
            row = r;
            found = true;
            break;
         }
      if ( found )
      {
         {
            JourneyStore::Transaction tx( *m_store );
            m_store->SetImageView( row.id, id, path );
            m_store->SetImageOwner( row.id, m_owner );
            m_store->SetJourneyStatus( row.journeyId, "recording" );
            m_store->TouchJourney( row.journeyId, NowIso() );   // review I2: not pruned as stale while open again
            tx.Commit();
         }
         t->imageId = row.id;
         t->journeyId = row.journeyId;
         t->dirty = true;
         m_tracked.push_back( std::move( t ) );
         m_joinNotes << "PI Copilot: continuing the recorded journey of " + String( id.c_str() );
         return row.journeyId;
      }
   }
   const AcquisitionFacts acq = ExtractAcquisition( kw, snap.steps, FromU8( path ), id );
   const std::string now = NowIso();
   {
      JourneyStore::Transaction tx( *m_store );   // review I5: the journey exists whole or not at all
      t->journeyId = m_store->CreateJourney( DeriveJourneyName( acq.target, acq.filter, 1, now ), acq.target, now );
      if ( m_joinFault )
         m_joinFault( "master" );
      t->imageId = m_store->AddImage( t->journeyId, id, path, MasterFingerprint( g.w, g.h, g.ch, g.bits, g.isFloat, identities, kw ),
                                      true, now );
      m_store->SetImageOwner( t->imageId, m_owner );
      m_store->SetAcquisition( t->imageId, acq );
      // Base (re-review R3): what the master brought with it -- its initialProcessing (a file's or its
      // creating history) plus the processing steps that STARTED before the tracker first saw the window.
      // Steps made after it appeared are the user's steps, even when the busy gate deferred this join
      // past them; a window restored with past processing (recording off meanwhile, a project) keeps that
      // past as base. A step with no start time (Script) before them counts as past.
      int base = snap.initialLength;
      for ( const HistoryStep& h : snap.steps )
         if ( h.combinedIndex == base )
         {
            const double st = IsoToEpoch( h.started );
            if ( st >= 0 && st >= firstSeen - 0.5 )
               break;
            ++base;
         }
      AddBaseAndSteps( t->imageId, snap, base );
      tx.Commit();
   }
   t->dirty = false;
   Tracked& tr = *t;
   const int64 jid = t->journeyId;
   m_tracked.push_back( std::move( t ) );   // tracked as soon as committed (re-review m4) ...
   StartingStats( tr );                     // ... then the statistics, whose failure is only its pause
   m_joinNotes << "PI Copilot: recording the image journey of " + String( id.c_str() ) + " (" + FromU8( why ) + ")";
   return jid;
}

int64 JourneyTracker::JoinLinked( const View& v, const HistorySnapshot& snap, int64 journeyId,
                                  const std::vector<PlannedLink>& links, const std::string& evidence, const std::string& why )
{
   const std::string id = ViewIdOf( v );
   const ViewGeom g = ViewGeometry( v );
   const std::vector<std::string> identities = StepIdentities( snap );
   const FITSKeywordArray kw = v.Window().Keywords();
   std::unique_ptr<Tracked> t( new Tracked );
   t->view = v;
   t->id = id;
   t->modifyCount = ModifyCountOf( v );
   t->why = why;
   t->journeyId = journeyId;
   {
      JourneyStore::Transaction tx( *m_store );   // review I5: image, steps and links together or not at all
      t->imageId = m_store->AddImage( journeyId, id, FilePathOf( v ),
                                      MasterFingerprint( g.w, g.h, g.ch, g.bits, g.isFloat, identities, kw ), false, NowIso() );
      m_store->SetImageOwner( t->imageId, m_owner );
      if ( m_joinFault )
         m_joinFault( "linked" );
      // initialProcessing = the creating step, already on the source
      const std::vector<int64> stepIds = AddBaseAndSteps( t->imageId, snap, snap.initialLength );
      for ( const PlannedLink& l : links )
      {
         int64 via = l.viaStepId;
         if ( l.viaSeq > 0 )
            via = size_t( l.viaSeq ) <= stepIds.size() ? stepIds[l.viaSeq - 1] : 0;
         m_store->AddLink( { l.fromImageId, t->imageId, via, evidence } );
      }
      m_store->TouchJourney( journeyId, NowIso() );
      tx.Commit();
   }
   t->dirty = false;
   Tracked& tr = *t;
   const int64 img = t->imageId;
   m_tracked.push_back( std::move( t ) );   // re-review m4: tracked right after the commit
   StartingStats( tr );
   return img;
}

int64 JourneyTracker::StartJourneyFor( const View& view, String& error, double /*now*/ )
{
   try
   {
      if ( view.IsNull() || view.IsPreview() )
      {
         error = "start_journey needs a main image view (not a preview)";
         return 0;
      }
      if ( !m_enabled )
      {
         // Spec §8: recording disabled -- nothing is read or stored.
         error = "journey recording is off (PI Copilot settings: Record image journeys); nothing was recorded";
         return 0;
      }
      if ( m_store == nullptr )
      {
         error = "the journey library is not available: " + m_storeError;
         return 0;
      }
      if ( m_inTick )
      {
         error = "the journey recorder is busy right now; try start_journey again in a moment";
         return 0;
      }
      struct Guard { bool& b; explicit Guard( bool& x ) : b( x ) { b = true; } ~Guard() { b = false; } } guard( m_inTick );
      std::vector<View> open;
      std::vector<size_type> counts;
      DropClosed( open, counts );   // forget closed windows first (review I6)
      const std::string id = ViewIdOf( view );
      if ( const Tracked* t = FindTracked( view ) )
      {
         error = String().Format( "%s is already recorded in journey #%lld", id.c_str(), static_cast<long long>( t->journeyId ) );
         return 0;
      }
      if ( IsBusy( view ) )
      {
         error = "view " + String( id.c_str() ) + " is busy (locked by a running process); try again when it finishes";
         return 0;
      }
      const HistorySnapshot snap = m_read( IsoString( id.c_str() ), 0 );
      if ( snap.busy )
      {
         error = "another script is running in PixInsight right now; try start_journey again when it finishes";
         return 0;
      }
      if ( !snap.ok )
      {
         error = snap.error;
         return 0;
      }
      if ( const Tracked* t = FindTracked( view ) )   // re-checked after the read (review I6)
      {
         error = String().Format( "%s is already recorded in journey #%lld", id.c_str(), static_cast<long long>( t->journeyId ) );
         return 0;
      }
      const int64 jid = JoinAsMaster( view, snap, view.Window().Keywords(), "started from chat (start_journey)", JourneyWallNow() );
      Forget( view );   // re-review m5: only after the join succeeded
      return jid;
   }
   catch ( const ViewBusy& )
   {
      error = "view " + String( ViewIdOf( view ).c_str() ) + " is busy (locked by a running process); try again when it finishes";
   }
   catch ( const pcl::Exception& x )
   {
      error = x.Message();
   }
   catch ( const std::exception& x )
   {
      error = String( x.what() );
   }
   return 0;
}

void JourneyTracker::FlushPendingGaps()
{
   while ( !m_pendingGaps.empty() )
   {
      m_store->AddGap( m_pendingGaps.front() );   // throws (and keeps the rest) while the DB is unusable
      m_pendingGaps.erase( m_pendingGaps.begin() );
   }
}

void JourneyTracker::EndClosedJourneys()
{
   std::vector<int64> closed;
   closed.swap( m_closed );
   for ( size_t k = 0; k < closed.size(); ++k )
   {
      const int64 jid = closed[k];
      bool open = false;
      for ( const std::unique_ptr<Tracked>& t : m_tracked )
         open = open || t->journeyId == jid;
      if ( !open )
         try
         {
            m_store->SetJourneyStatus( jid, "ended" );
         }
         catch ( const JourneyRowMissing& )
         {
            // R2: the journey is gone (another instance, a restore): there is nothing left to end.
         }
         catch ( ... )
         {
            m_closed.insert( m_closed.end(), closed.begin() + k, closed.end() );   // retried on the next tick
            throw;
         }
   }
   // Re-review m6: closed images are no longer recorded by this instance.
   std::vector<int64> images;
   images.swap( m_closedImages );
   for ( size_t k = 0; k < images.size(); ++k )
      try
      {
         m_store->SetImageOwner( images[k], "" );
      }
      catch ( const JourneyRowMissing& )
      {
      }
      catch ( ... )
      {
         m_closedImages.insert( m_closedImages.end(), images.begin() + k, images.end() );
         throw;
      }
}

JourneyStatus JourneyTracker::StatusFor( const IsoString& viewFullId ) const
{
   JourneyStatus s;
   if ( !m_enabled )
   {
      s.state = RecordingState::Off;
      return s;
   }
   if ( m_store == nullptr )
   {
      s.state = RecordingState::Paused;
      s.reason = m_storeError;
      return s;
   }
   const Tracked* t = FindTrackedById( std::string( viewFullId.c_str() ) );
   if ( t == nullptr )
   {
      s.state = RecordingState::NotTracked;
      return s;
   }
   s.journeyId = t->journeyId;
   s.imageId = t->imageId;
   s.why = t->why;
   // R4: a stall of the busy gate longer than 5 s is visible, not "recording".
   String waiting;
   if ( m_gateSince > 0 && JourneyWallNow() - m_gateSince >= 5 )
      waiting = "waiting: " + m_gateReason;
   int gaps = 0;
   for ( const GapRow& g : m_pendingGaps )
      if ( g.imageId == t->imageId )
         ++gaps;
   if ( gaps > 0 )
      s.note = String().Format( "%d unrecorded step range(s) wait to be written as gaps", gaps );
   try
   {
      JourneyRow j;
      if ( m_store->GetJourney( t->journeyId, j ) )
      {
         s.name = j.name;
         s.target = j.target;
      }
      int masters = 0;
      std::string filter;
      for ( const ImageRow& i : m_store->Images( t->journeyId ) )
         if ( i.isMaster )
         {
            AcquisitionFacts a;
            if ( masters++ == 0 && m_store->Acquisition( i.id, a ) )
               filter = a.filter;
         }
      s.kind = StripKind( filter, std::max( 1, masters ) );
      s.activeSteps = m_store->StepCount( t->journeyId, true );
      // Tick-wide failure first (it affects every image), then this image's own (review M3).
      s.reason = !m_pausedReason.IsEmpty() ? m_pausedReason : !t->pausedReason.IsEmpty() ? t->pausedReason
               : !waiting.IsEmpty() ? waiting : t->statsReason;
      s.state = s.reason.IsEmpty() ? RecordingState::Recording : RecordingState::Paused;
   }
   catch ( const pcl::Exception& x )
   {
      s.state = RecordingState::Paused;
      s.reason = x.Message();
   }
   return s;
}

int64 JourneyTracker::ImageOfView( const IsoString& viewFullId ) const
{
   const Tracked* t = FindTrackedById( std::string( viewFullId.c_str() ) );
   return t != nullptr ? t->imageId : 0;
}

int64 JourneyTracker::JourneyOfView( const IsoString& viewFullId ) const
{
   const Tracked* t = FindTrackedById( std::string( viewFullId.c_str() ) );
   return t != nullptr ? t->journeyId : 0;
}

std::vector<int64> JourneyTracker::OpenJourneyIds() const
{
   std::vector<int64> ids;
   for ( const std::unique_ptr<Tracked>& t : m_tracked )
      if ( std::find( ids.begin(), ids.end(), t->journeyId ) == ids.end() )
         ids.push_back( t->journeyId );
   return ids;
}

int RunRetentionIfDue( JourneyStore& store, int days, const std::string& today, std::string& lastRun, StringList* removed,
                       const std::vector<int64>& excludeJourneyIds )
{
   if ( today == lastRun )
      return -1;
   lastRun = today;   // before pruning (review I1): a failure retries on the next date, never on every tick
   return store.PruneUnkept( IsoDaysAgo( std::min( 3650, std::max( 1, days ) ) ), removed, excludeJourneyIds );
}

int RetentionPass( JourneyStore& store, const JourneyTracker& tracker, int days, const std::string& today,
                   std::string& openTouchedDate, std::string& lastRun, StringList* removed )
{
   const std::vector<int64> open = tracker.OpenJourneyIds();
   if ( today != openTouchedDate )
   {
      // R1: per instance, once per local date, independent of lastRun (another instance may already have
      // pruned today): every open journey's `updated` stays within a day of now, so no instance's cutoff
      // (>= 1 day) reaches it.
      JourneyStore::Transaction tx( store );
      for ( int64 id : open )
         try
         {
            store.TouchJourney( id, NowIso() );
         }
         catch ( const JourneyRowMissing& )
         {
            // gone meanwhile: the tracker drops the image on its next write (R2)
         }
      tx.Commit();
      openTouchedDate = today;
   }
   return RunRetentionIfDue( store, days, today, lastRun, removed, open );
}

// ---- JourneyService ----------------------------------------------------------

class JourneyTimerHost : public Control
{
public:

   JourneyTimerHost()
   {
      T.SetInterval( PICopilotJourneyTickSeconds );
      T.SetPeriodic( true );
      T.OnTimer( (Timer::timer_event_handler)&JourneyTimerHost::e_Tick, *this );
   }

   Timer T;

   void e_Tick( Timer& )
   {
      JourneyService::Instance().OnTick();
   }
};

JourneyService::JourneyService() = default;

JourneyService::~JourneyService()
{
   // Static destruction runs after PixInsight has torn the module's host down,
   // where no PCL call is defined (review M6). Stop() (OnUnload) normally
   // released everything on the root thread; if it did not, everything is
   // leaked -- never destroyed here, on any thread (a Control, a Timer and the
   // SQLite connection are root-thread objects of a host that is gone).
   if ( !m_host && !m_tracker && !m_store )
      return;
   (void)m_host.release();
   (void)m_tracker.release();
   (void)m_store.release();
}

JourneyService& JourneyService::Instance()
{
   static JourneyService s;
   return s;
}

String JourneyService::LibraryRoot()
{
   const char* xdg = std::getenv( "XDG_DATA_HOME" );
   if ( xdg != nullptr && *xdg == '/' )
      return String( xdg ) + "/PICopilot/journeys";
   return File::HomeDirectory() + "/.local/share/PICopilot/journeys";
}

void JourneyService::OpenStore()
{
   m_lastOpenAttempt = JourneyWallNow();
   String e;
   m_store = JourneyStore::Open( LibraryRoot(), e );
   m_storeError = e;
   if ( m_tracker )
      m_tracker->SetStore( m_store.get(), e );
   if ( !m_store )
   {
      Console().WarningLn( "PI Copilot: " + e );
      AddNote( e );
   }
}

void JourneyService::Start()
{
   if ( m_started )
      return;
   m_tracker.reset( new JourneyTracker( nullptr ) );
   OpenStore();
   {
      String last;
      Settings::Read( kRetentionLastRunKey, last );
      m_retentionLastRun = U8( last );
   }
   ApplySettings();
   m_host.reset( new JourneyTimerHost );
   m_host->T.Start();
   m_started = true;
}

void JourneyService::Stop()
{
   if ( !Thread::IsRootThread() )
      return;   // root-thread objects: only ever stopped and destroyed on the root thread (OnUnload)
   try
   {
      if ( m_host )
         m_host->T.Stop();
      m_host.reset();
   }
   catch ( ... )
   {
   }
   m_tracker.reset();
   m_store.reset();
   m_started = false;
}

void JourneyService::ApplySettings()
{
   if ( m_tracker )
      m_tracker->SetEnabled( CopilotSettings::LoadRecordJourneys() );
}

void JourneyService::RunRetention()
{
   String failure;
   int n = -1;
   try
   {
      StringList removed;
      n = RetentionPass( *m_store, *m_tracker, CopilotSettings::LoadJourneyRetentionDays(), LocalDateToday(),
                         m_openTouchedDate, m_retentionLastRun, &removed );
   }
   catch ( const pcl::Exception& x )
   {
      failure = x.Message();   // RemoveDirectoryTree names the path (Task 5); the row stays for the next date
   }
   catch ( const std::exception& x )
   {
      failure = String( x.what() );
   }
   if ( n < 0 && failure.IsEmpty() )
      return;   // not due
   Settings::Write( kRetentionLastRunKey, String( m_retentionLastRun.c_str() ) );   // ran today, whatever the outcome
   if ( !failure.IsEmpty() )
   {
      if ( failure != m_lastRetentionError )   // one note per distinct failure, not per pass
      {
         const String m = "PI Copilot: journey retention paused until tomorrow: " + failure;
         Console().WarningLn( m );
         AddNote( m );
      }
      m_lastRetentionError = failure;
      return;
   }
   m_lastRetentionError.Clear();
   if ( n > 0 )
      Console().NoteLn( String().Format( "PI Copilot: removed %d unkept image journeys older than %d days.",
                                         n, CopilotSettings::LoadJourneyRetentionDays() ) );
}

void JourneyService::OnTick()
{
   if ( !m_started || m_selfTestPaused || m_inOnTick )
      return;
   struct Guard { bool& b; explicit Guard( bool& x ) : b( x ) { b = true; } ~Guard() { b = false; } } guard( m_inOnTick );   // M7
   const double now = JourneyWallNow();
   if ( !m_store && now - m_lastOpenAttempt >= 60 )
      OpenStore();   // the user may have moved a damaged file aside
   m_tracker->Tick( now );
   for ( const String& n : m_tracker->TakeJoinNotes() )
   {
      Console().NoteLn( n );
      AddNote( n );
   }
   if ( m_store && m_tracker->Enabled() && !RecorderActivity().busy && !m_store->InTransaction() )
      RunRetention();
}

void JourneyService::OnImageCreated( const View& v ) { if ( m_tracker ) m_tracker->OnImageCreated( v, JourneyWallNow() ); }
void JourneyService::OnImageUpdated( const View& v ) { if ( m_tracker ) m_tracker->OnImageUpdated( v, JourneyWallNow() ); }
void JourneyService::OnImageRenamed( const View& v ) { if ( m_tracker ) m_tracker->OnImageRenamed( v, JourneyWallNow() ); }
void JourneyService::OnImageDeleted( const View& v ) { if ( m_tracker ) m_tracker->OnImageDeleted( v, JourneyWallNow() ); }
void JourneyService::OnImageSaved( const View& v )   { if ( m_tracker ) m_tracker->OnImageSaved( v, JourneyWallNow() ); }
void JourneyService::OnImageFocused( const View& v ) { if ( m_tracker ) m_tracker->OnImageFocused( v, JourneyWallNow() ); }

void JourneyService::AddNote( const String& note )
{
   if ( m_notes.Length() < 50 )
      m_notes << note;
}

StringList JourneyService::TakeNotes()
{
   StringList n = m_notes;
   m_notes.Clear();
   return n;
}

void JourneyService::FlushAndPauseForSelfTest()
{
   if ( m_selfTestPaused )
      return;   // already flushed (the j6 "service" fixture phase); a second flush would record later windows
   if ( m_started && m_tracker )
      for ( int i = 0; i < 2; ++i )   // the second: candidates found by the first tick's scan
      {
         // The busy gate defers a tick right after process activity; wait (pumping) until idle, max 3 s.
         const auto t0 = std::chrono::steady_clock::now();
         while ( RecorderActivity().busy
              && std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count() < 3 )
         {
            ThePICopilotModule->ProcessEvents( true/*excludeUserInputEvents*/ );
            std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
         }
         m_tracker->Tick( JourneyWallNow(), true );
      }
   m_selfTestPaused = true;
}

} // namespace pcl
