// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "JourneyTracker.h"
#include "CopilotSettings.h"
#include "EvalGuard.h"
#include "JourneyConstants.h"
#include "MasterFacts.h"
#include "PjsrRunner.h"   // IsPjsrScriptRunning, ScriptLiteral
#include "PICopilotModule.h"
#include "StepStats.h"
#include "ToolHelpers.h"   // IsBusy
#include "Utf8.h"

#include <pcl/AutoViewLock.h>
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
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <set>

namespace pcl
{

namespace
{

const char* const kRetentionLastRunKey = "PICopilot/JourneyRetentionLastRun";
constexpr double  kCopilotNoteSeconds = 30;
constexpr int     kCandidateTicks = 5;
constexpr size_t  kRecentSteps = 200;
constexpr int     kCandidateDeferred = 2;   // EvaluateCandidate(): nothing read yet (another script runs); not a wait tick

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

// Geometry and sample format for MasterFingerprint(). The caller has probed
// IsBusy( v ) == false in this tick, so the lock is free (never waited on).
struct ViewGeom { int w = 0, h = 0, ch = 0, bits = 32; bool isFloat = true; };

ViewGeom ViewGeometry( const View& v )
{
   ViewGeom g;
   View vv = v;
   AutoViewWriteLock lock( vv );
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

// Keeps the elements for which keep() is true. PCL copies a View by attaching
// to its handle, and attaching to the handle of a CLOSED window throws
// "AttachToUIObject(): API function error" (measured: Task 7 J6 RED run --
// vector::erase shifting a dead view wedged every later tick). So a vector
// that may hold a dead view is never erased from or reallocated: the
// survivors are copied into a fresh vector and the rest are only destroyed.
template <class T, class Keep>
void KeepOnly( std::vector<T>& v, Keep keep )
{
   std::vector<T> kept;
   kept.reserve( v.size() );
   for ( const T& e : v )
      if ( keep( e ) )
         kept.push_back( e );
   v.swap( kept );
}

void CollectStrings( const nlohmann::json& j, std::set<std::string>& out )
{
   if ( j.is_string() )
      out.insert( j.get<std::string>() );
   else if ( j.is_array() || j.is_object() )
      for ( const nlohmann::json& e : j )
         CollectStrings( e, out );
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

// ---- JourneyTracker ----------------------------------------------------------

JourneyTracker::JourneyTracker( JourneyStore* store, const String& storeError )
   : m_store( store ), m_storeError( storeError ),
     m_useModifyCount( PICopilotJourneyScanUsesModifyCount && PICopilotJourneyNotificationsWork ),
     m_read( ReadViewHistory )
{
   // ModifyCount resets on save; a save + step inside one scan interval is only
   // caught through ImageSaved, so ModifyCount scanning needs notifications.
}

void JourneyTracker::SetStore( JourneyStore* store, const String& storeError )
{
   m_store = store;
   m_storeError = storeError;
}

void JourneyTracker::SetEnabled( bool on )
{
   if ( on && !m_enabled )
      m_forceScan = true;
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
   for ( Tracked& t : m_tracked )
      if ( t.view == v )
         return &t;
   return nullptr;
}

const JourneyTracker::Tracked* JourneyTracker::FindTrackedById( const std::string& id ) const
{
   for ( const Tracked& t : m_tracked )
      if ( t.id == id )
         return &t;
   return nullptr;
}

bool JourneyTracker::IsCandidate( const View& v ) const
{
   for ( const Candidate& c : m_candidates )
      if ( c.view == v )
         return true;
   return false;
}

void JourneyTracker::AddCandidate( const View& v, double now )
{
   if ( !IsCandidate( v ) )
      m_candidates.push_back( { v, now, 0 } );
}

size_type JourneyTracker::PendingCount() const
{
   size_type n = m_events.size() + m_candidates.size() + m_pendingGaps.size();
   for ( const Tracked& t : m_tracked )
      if ( t.dirty )
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

// Called only at the start of Tick(), before anything iterates the vectors.
void JourneyTracker::DrainEvents()
{
   while ( !m_events.empty() )
   {
      // By reference: copying the event would attach to its view's handle,
      // which throws once that window is closed (see KeepOnly). The event is
      // popped whatever happens, so one bad event can never wedge the queue.
      const PendingEvent& e = m_events.front();
      try
      {
         ApplyEvent( e, e.view );
      }
      catch ( ... )
      {
         // The view closed before the event was applied (any API call on it throws). Nothing about a
         // closed view needs recording; the forced scan drops it and re-derives everything else.
         m_forceScan = true;
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
         AddCandidate( view, e.t );
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
         for ( const Ignored& i : m_ignored )
            wasIgnored = wasIgnored || i.view == view;
         if ( wasIgnored )
         {
            KeepOnly( m_ignored, [&view]( const Ignored& x ) { return !(x.view == view); } );
            AddCandidate( view, e.t );   // re-evaluated: a new step may now reference a tracked view
         }
      }
      break;
   case EventKind::Renamed:
      m_forceScan = true;   // the scan compares ids of the SAME view objects
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
   for ( Tracked& t : m_tracked )
      if ( t.id == vid )
         t.dirty = true;
   m_forceScan = true;
}

bool JourneyTracker::ConsumeCopilotNote( const std::string& viewId, const std::string& processId, double now, std::string& reason )
{
   for ( auto it = m_copilot.begin(); it != m_copilot.end(); ++it )
      if ( it->viewId == viewId && it->processId == processId && now - it->t <= kCopilotNoteSeconds )
      {
         reason = it->reason;
         m_copilot.erase( it );
         return true;
      }
   return false;
}

void JourneyTracker::Tick( double now, bool forceScan )
{
   // Never nests an EvaluateScript (history reads, the batch scan) inside
   // another one -- a model-written run_pjsr script or any module caller that
   // pumps events (controller ruling after Task 1; EvalGuard.h): the whole
   // tick is deferred to the next one.
   if ( !m_enabled || m_inTick || IsPjsrScriptRunning() || EvaluateScriptDepth() > 0 )
      return;
   struct Guard { bool& b; explicit Guard( bool& x ) : b( x ) { b = true; } ~Guard() { b = false; } } guard( m_inTick );
   if ( m_store == nullptr )
   {
      m_pausedReason = m_storeError;
      return;
   }
   try
   {
      std::vector<View> open;
      std::vector<size_type> counts;
      DropClosed( open, counts );   // every tick, first: nothing below may copy (or shift) a closed window's view
      DrainEvents();   // then the queue: its focus changes are older than the ActiveWindow() sample below
      {
         const ImageWindow aw = ImageWindow::ActiveWindow();
         if ( !aw.IsNull() )
            NoteActive( ViewIdOf( aw.MainView() ), now );
      }
      if ( forceScan || m_forceScan || now - m_lastScan >= PICopilotJourneyScanSeconds )
      {
         Scan( now, open, counts );
         m_lastScan = now;
         m_forceScan = false;
      }
      FlushPendingGaps();
      ProcessDirty( now );
      ProcessCandidates( now );
      EndClosedJourneys();
      m_copilot.erase( std::remove_if( m_copilot.begin(), m_copilot.end(),
                                       [now]( const CopilotNote& n ) { return now - n.t > kCopilotNoteSeconds; } ), m_copilot.end() );
      m_created.erase( std::remove_if( m_created.begin(), m_created.end(),
                                       [now]( const CreatedNote& n ) { return now - n.t > kCopilotNoteSeconds; } ), m_created.end() );
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
   for ( const Tracked& t : m_tracked )
      if ( !isOpen( t.view ) )
         m_closed.push_back( t.journeyId );
   KeepOnly( m_tracked, [&]( const Tracked& t ) { return isOpen( t.view ); } );
   KeepOnly( m_candidates, [&]( const Candidate& c ) { return isOpen( c.view ); } );
   KeepOnly( m_ignored, [&]( const Ignored& i ) { return isOpen( i.view ); } );
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
         {
            m_store->SetImageView( t->imageId, id, FilePathOf( v ) );
            t->id = id;
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
      for ( auto it = m_ignored.begin(); it != m_ignored.end(); ++it )
         if ( it->view == v )
         {
            ignored = true;
            if ( it->modifyCount != counts[i] )
            {
               m_ignored.erase( it );
               AddCandidate( v, now );
            }
            break;
         }
      if ( !ignored )
         AddCandidate( v, now );
   }
   if ( !m_useModifyCount )
      BatchCounts();
}

void JourneyTracker::BatchCounts()
{
   // Busy views are not read (global constraint, pre-flight P8): they are
   // marked dirty, and ProcessDirty() defers them until they are free.
   std::vector<size_t> probed;
   std::string ids = "[";
   for ( size_t i = 0; i < m_tracked.size(); ++i )
   {
      if ( IsBusy( m_tracked[i].view ) )
      {
         m_tracked[i].dirty = true;
         continue;
      }
      ids += (probed.empty() ? "" : ",") + ScriptLiteral( String( m_tracked[i].id.c_str() ) );
      probed.push_back( i );
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
      if ( c != m_tracked[probed[k]].lastCounts )
         m_tracked[probed[k]].dirty = true;
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
   for ( Tracked& t : m_tracked )
   {
      if ( !t.dirty )
         continue;
      if ( IsBusy( t.view ) )
      {
         ++m_deferrals;   // never waited on: the next tick tries again
         continue;
      }
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
         continue;
      }
      if ( !snap.ok )
      {
         m_pausedReason = snap.error;
         if ( ++t.readFailures >= 3 )
         {
            m_pendingGaps.push_back( { t.journeyId, t.imageId, lastSeq, U8( snap.error ) } );
            t.readFailures = 0;
            t.dirty = false;   // retried on the next change
            FlushPendingGaps();
         }
         continue;
      }
      bool changed = false;
      for ( int64 id : d.toActive )     { m_store->SetStepState( id, "active" ); changed = true; }
      for ( int64 id : d.toUndone )     { m_store->SetStepState( id, "undone" ); changed = true; }
      for ( int64 id : d.toSuperseded ) { m_store->SetStepState( id, "superseded" ); changed = true; }
      int64 lastActive = 0;
      for ( size_t i = 0; i < d.appended.size(); ++i )
      {
         const HistoryStep& h = d.appended[i];
         std::string reason;
         const bool copilot = ConsumeCopilotNote( t.id, h.processId, now, reason );
         const int64 sid = m_store->AddStep( MakeStepRow( h, t.imageId, d.appendedState[i], copilot ? "copilot" : "user",
                                                          reason, snap.ActiveCount() ) );
         if ( d.appendedState[i] == "active" )
            lastActive = sid;
         Remember( h, t.imageId, t.journeyId, sid );
         changed = true;
      }
      if ( changed )
         m_store->TouchJourney( t.journeyId, NowIso() );
      m_pausedReason.Clear();   // the history read and the DB writes succeeded
      if ( lastActive != 0 )
      {
         // Ruling 7: only the last active step appended in this tick is measured. Steps made between two
         // observations share this one measurement; the intermediate pixels no longer exist.
         const StepStatsResult s = ComputeStepStats( t.view, m_store->JourneyDir( t.journeyId )
                                                     + String().Format( "/thumbs/%lld.jpg", static_cast<long long>( lastActive ) ) );
         if ( s.ok )
         {
            m_store->AddStats( t.imageId, lastActive, s.channels );
            m_statsPausedReason.Clear();
         }
         else
            // Spec §8 row 1 / pre-flight P11: the strip shows "recording paused: …" until a later step's
            // statistics succeed. The steps themselves are recorded, so no gap row is written.
            m_statsPausedReason = "statistics not recorded: " + s.error;
      }
      t.dirty = false;
      t.readFailures = 0;
      t.lastCounts = { snap.initialLength, snap.length, snap.historyIndex };
      m_lastStepMs = std::chrono::duration<double, std::milli>( std::chrono::steady_clock::now() - t0 ).count();
   }
}

void JourneyTracker::ProcessCandidates( double now )
{
   for ( auto it = m_candidates.begin(); it != m_candidates.end(); )
   {
      if ( IsBusy( it->view ) )
      {
         ++m_deferrals;
         ++it;
         continue;
      }
      m_evalNote.clear();
      const int r = EvaluateCandidate( *it, now );
      {
         char head[96];
         std::snprintf( head, sizeof head, "%.3f r=%d ticks=%d ", now, r, it->ticks );
         m_decisions.push_back( head + ViewIdOf( it->view ) + " " + m_evalNote );
         while ( m_decisions.size() > 100 )
            m_decisions.pop_front();
      }
      if ( r == kCandidateDeferred )
      {
         ++m_deferrals;   // nothing was read: not counted as a wait tick
         ++it;
      }
      else if ( r > 0 )
         it = m_candidates.erase( it );
      else if ( r == 0 && ++it->ticks < kCandidateTicks )
         ++it;
      else
      {
         const ImageWindow w = it->view.Window();
         m_ignored.push_back( { it->view, w.IsNull() ? 0 : w.ModifyCount() } );
         it = m_candidates.erase( it );
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
   for ( const Tracked& t : m_tracked )
      if ( tokens.count( t.id ) > 0 )
         r.push_back( &t );
   return r;
}

int JourneyTracker::EvaluateCandidate( Candidate& c, double now )
{
   const std::string id = ViewIdOf( c.view );
   // (1) copilot: a Copilot tool reported this window (Ruling 19.1 / 1.5).
   for ( const CreatedNote& n : m_created )
      if ( n.id == id && now - n.t <= kCopilotNoteSeconds )
      {
         const HistorySnapshot snap = m_read( IsoString( id.c_str() ), 0 );
         if ( snap.busy )
            return kCandidateDeferred;
         if ( !snap.ok )
            return 0;
         if ( n.integration )
         {
            if ( !n.first )
               return -1;   // Ruling 29: an auxiliary output of Copilot's integration run
            return JoinAsMaster( c.view, snap, c.view.Window().Keywords(),
                                 "created by Copilot's run_global_process of an integration process" ) != 0 ? 1 : -1;
         }
         if ( const Tracked* src = FindTrackedById( n.sourceViewId ) )
         {
            const int64 srcImage = src->imageId, srcJourney = src->journeyId;
            const std::vector<StepRow> steps = m_store->Steps( srcImage, false );
            return JoinLinked( c.view, snap, srcImage, srcJourney, steps.empty() ? 0 : steps.back().id, "copilot" ) != 0 ? 1 : -1;
         }
      }
   const HistorySnapshot snap = m_read( IsoString( id.c_str() ), 0 );
   if ( snap.busy )
      return kCandidateDeferred;
   if ( !snap.ok )
   {
      m_evalNote = "read: " + U8( snap.error );
      return 0;
   }
   m_evalNote = "init=" + std::to_string( snap.initialLength ) + " len=" + std::to_string( snap.length )
              + " first=" + (snap.steps.empty() ? std::string() : snap.steps.front().processId + "@" + snap.steps.front().started);
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
      return JoinAsMaster( c.view, snap, kw, me.why ) != 0 ? 1 : -1;
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
      // Pre-flight P3: copy what is needed out of m_tracked BEFORE JoinLinked() appends to it (reallocation).
      struct RefImage { int64 imageId, journeyId; };
      std::vector<RefImage> refImages;
      for ( const Tracked* t : refs )
         refImages.push_back( { t->imageId, t->journeyId } );
      const int64 journeyId = refImages.front().journeyId;
      const int64 img = JoinLinked( c.view, snap, 0, journeyId, 0, std::string() );
      if ( img == 0 )
         return -1;
      int64 via = 0;
      for ( const StepRow& r : m_store->Steps( img, false ) )
         if ( r.seq == h.combinedIndex + 1 )
            via = r.id;
      for ( const RefImage& r : refImages )
         if ( r.journeyId == journeyId )
            m_store->AddLink( { r.imageId, img, via, "reference" } );
      return 1;
   }
   // (4) timing (a)/(b) (Ruling 19.3).
   if ( snap.initialLength > 0 )
   {
      // (a) the creating step is a recorded step (it also changed its source).
      for ( const RecentStep& r : m_recent )
         if ( r.identity == snap.steps.front().identity )
            return JoinLinked( c.view, snap, r.imageId, r.journeyId, r.stepId, "timing" ) != 0 ? 1 : -1;
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
            const int64 srcImage = src->imageId, srcJourney = src->journeyId;   // src is not used after JoinLinked
            const std::vector<StepRow> steps = m_store->Steps( srcImage, false );
            return JoinLinked( c.view, snap, srcImage, srcJourney, steps.empty() ? 0 : steps.back().id, "timing" ) != 0 ? 1 : -1;
         }
      }
   }
   // (5) Ruling 28: a derived window with master keywords and no link evidence is a master of its own.
   if ( me.isMaster )
      return JoinAsMaster( c.view, snap, kw, me.why ) != 0 ? 1 : -1;
   // (6) timing (c), last: first seen inside exactly one recorded step's time window.
   //     Never decided on the FIRST look, and an ambiguous window (2+ steps) is no rejection: a window
   //     made by a hand-run process can be seen by a tick that runs while that process is still
   //     executing, before its creating step is in initialProcessing (J6 (f3)); rejecting it then
   //     would lose it for good, although the next look finds timing (b) or reference evidence.
   //     With no better evidence it is still ignored after kCandidateTicks looks, as before.
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
      return JoinLinked( c.view, snap, hit->imageId, hit->journeyId, hit->stepId, "timing" ) != 0 ? 1 : -1;
   return 0;
}


// Records every step of an image joining a journey; the first baseCount
// (combined index) are "base": the image's own starting history (a master's
// stacking, a derived window's creating step already recorded on its source).
// Base steps keep the diff aligned but never reach recipes, counts or replay (spec D2).
void JourneyTracker::AddBaseAndSteps( int64 imageId, const HistorySnapshot& snap, int baseCount )
{
   for ( const HistoryStep& h : snap.steps )
   {
      const std::string state = h.combinedIndex + 1 <= snap.ActiveCount() ? "active" : "undone";
      StepRow r = MakeStepRow( h, imageId, state, "user", "", snap.ActiveCount() );
      if ( h.combinedIndex < baseCount )
         r.params["base"] = true;
      m_store->AddStep( r );
   }
}

void JourneyTracker::StartingStats( const View& v, int64 journeyId, int64 imageId )
{
   const StepStatsResult s = ComputeStepStats( v, m_store->JourneyDir( journeyId )
                                               + String().Format( "/thumbs/start-%lld.jpg", static_cast<long long>( imageId ) ) );
   if ( s.ok )
   {
      m_store->AddStats( imageId, 0, s.channels );
      m_statsPausedReason.Clear();
   }
   else
      m_statsPausedReason = "starting statistics not recorded: " + s.error;   // a pause (P11), cleared by the next success
}

int64 JourneyTracker::JoinAsMaster( const View& v, const HistorySnapshot& snap, const FITSKeywordArray& kw, const std::string& why )
{
   const std::string id = ViewIdOf( v );
   const std::string path = FilePathOf( v );
   const ViewGeom g = ViewGeometry( v );
   const std::vector<std::string> identities = StepIdentities( snap );
   // Resume (save + reopen): the journey's fingerprint over a prefix of today's history.
   for ( size_t p = 0; p <= identities.size(); ++p )
   {
      ImageRow row;
      const std::vector<std::string> prefix( identities.begin(), identities.begin() + p );
      if ( m_store->FindResumableByFingerprint( MasterFingerprint( g.w, g.h, g.ch, g.bits, g.isFloat, prefix, kw ), row ) )
      {
         m_store->SetImageView( row.id, id, path );
         m_store->SetJourneyStatus( row.journeyId, "recording" );
         const ImageWindow win = v.Window();
         m_tracked.push_back( { v, id, row.id, row.journeyId, win.IsNull() ? 0 : win.ModifyCount(), {}, true, 0, why } );
         m_joinNotes << "PI Copilot: continuing the recorded journey of " + String( id.c_str() );
         return row.journeyId;
      }
   }
   const AcquisitionFacts acq = ExtractAcquisition( kw, snap.steps, FromU8( path ), id );
   const std::string now = NowIso();
   const int64 jid = m_store->CreateJourney( DeriveJourneyName( acq.target, acq.filter, 1, now ), acq.target, now );
   const int64 img = m_store->AddImage( jid, id, path, MasterFingerprint( g.w, g.h, g.ch, g.bits, g.isFloat, identities, kw ), true, now );
   m_store->SetAcquisition( img, acq );
   AddBaseAndSteps( img, snap, snap.TotalCount() );   // a master's whole history at join time is base
   StartingStats( v, jid, img );
   const ImageWindow win = v.Window();
   m_tracked.push_back( { v, id, img, jid, win.IsNull() ? 0 : win.ModifyCount(), {}, false, 0, why } );
   m_joinNotes << "PI Copilot: recording the image journey of " + String( id.c_str() ) + " (" + FromU8( why ) + ")";
   return jid;
}

int64 JourneyTracker::JoinLinked( const View& v, const HistorySnapshot& snap, int64 fromImageId, int64 journeyId,
                                  int64 viaStepId, const std::string& evidence )
{
   const std::string id = ViewIdOf( v );
   const ViewGeom g = ViewGeometry( v );
   const std::vector<std::string> identities = StepIdentities( snap );
   const FITSKeywordArray kw = v.Window().Keywords();
   const int64 img = m_store->AddImage( journeyId, id, FilePathOf( v ),
                                        MasterFingerprint( g.w, g.h, g.ch, g.bits, g.isFloat, identities, kw ), false, NowIso() );
   AddBaseAndSteps( img, snap, snap.initialLength );   // initialProcessing = the creating step, already on the source
   if ( !evidence.empty() )
      m_store->AddLink( { fromImageId, img, viaStepId, evidence } );
   m_store->TouchJourney( journeyId, NowIso() );
   StartingStats( v, journeyId, img );
   const ImageWindow win = v.Window();
   m_tracked.push_back( { v, id, img, journeyId, win.IsNull() ? 0 : win.ModifyCount(), {}, false, 0,
                          evidence.empty() ? std::string( "linked by reference" ) : "linked by " + evidence } );
   return img;
}

int64 JourneyTracker::StartJourneyFor( const View& view, String& error, double /*now*/ )
{
   if ( view.IsNull() || view.IsPreview() )
   {
      error = "start_journey needs a main image view (not a preview)";
      return 0;
   }
   if ( m_store == nullptr )
   {
      error = "the journey library is not available: " + m_storeError;
      return 0;
   }
   const std::string id = ViewIdOf( view );
   if ( const Tracked* t = FindTrackedById( id ) )
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
   m_candidates.erase( std::remove_if( m_candidates.begin(), m_candidates.end(),
                                       [&view]( const Candidate& c ) { return c.view == view; } ), m_candidates.end() );
   m_ignored.erase( std::remove_if( m_ignored.begin(), m_ignored.end(),
                                    [&view]( const Ignored& i ) { return i.view == view; } ), m_ignored.end() );
   try
   {
      return JoinAsMaster( view, snap, view.Window().Keywords(), "started from chat (start_journey)" );
   }
   catch ( const pcl::Exception& x )
   {
      error = x.Message();
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
   for ( int64 jid : m_closed )
   {
      bool open = false;
      for ( const Tracked& t : m_tracked )
         open = open || t.journeyId == jid;
      if ( !open )
         m_store->SetJourneyStatus( jid, "ended" );
   }
   m_closed.clear();
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
   if ( !m_pendingGaps.empty() )
      s.note = String().Format( "%d unrecorded step range(s) wait to be written as gaps", int( m_pendingGaps.size() ) );
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
      s.reason = !m_pausedReason.IsEmpty() ? m_pausedReason : m_statsPausedReason;
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

int RunRetentionIfDue( JourneyStore& store, int days, const std::string& today, std::string& lastRun, StringList* removed )
{
   if ( today == lastRun )
      return -1;
   const int n = store.PruneUnkept( IsoDaysAgo( std::min( 3650, std::max( 1, days ) ) ), removed );
   lastRun = today;
   return n;
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
   // The store, the tracker and the Timer's Control are root-thread objects
   // (JourneyStore refuses every call off it). Stop() normally ran in
   // OnUnload; if this static is destroyed off the root thread anyway, they
   // are released WITHOUT being destroyed (a leak at process exit) -- never
   // closed or deleted on the wrong thread, and a destructor never throws.
   if ( !Thread::IsRootThread() )
   {
      (void)m_host.release();
      (void)m_tracker.release();
      (void)m_store.release();
   }
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

void JourneyService::OnTick()
{
   if ( !m_started || m_selfTestPaused )
      return;
   const double now = JourneyWallNow();
   if ( !m_store && now - m_lastOpenAttempt >= 60 )
      OpenStore();   // the user may have moved a damaged file aside
   m_tracker->Tick( now );
   for ( const String& n : m_tracker->TakeJoinNotes() )
   {
      Console().NoteLn( n );
      AddNote( n );
   }
   if ( m_store && m_tracker->Enabled() )
      try
      {
         StringList removed;
         const int n = RunRetentionIfDue( *m_store, CopilotSettings::LoadJourneyRetentionDays(), LocalDateToday(),
                                          m_retentionLastRun, &removed );
         if ( n >= 0 )
         {
            Settings::Write( kRetentionLastRunKey, String( m_retentionLastRun.c_str() ) );
            if ( n > 0 )
               Console().NoteLn( String().Format( "PI Copilot: removed %d unkept image journeys older than %d days.",
                                                  n, CopilotSettings::LoadJourneyRetentionDays() ) );
         }
      }
      catch ( const pcl::Exception& x )
      {
         // RemoveDirectoryTree throws naming the path (Task 5); the row stays and the next date retries.
         const String m = "PI Copilot: journey retention paused: " + x.Message();
         Console().WarningLn( m );
         AddNote( m );
      }
      catch ( const std::exception& x )
      {
         const String m = "PI Copilot: journey retention paused: " + String( x.what() );
         Console().WarningLn( m );
         AddNote( m );
      }
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
   {
      m_tracker->Tick( JourneyWallNow(), true );
      m_tracker->Tick( JourneyWallNow(), true );   // candidates found by the first tick's scan
   }
   m_selfTestPaused = true;
}

} // namespace pcl
