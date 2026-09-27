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
#include <map>
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
constexpr double  kTickBudgetMs = 100;
const char* const kTouchFailurePrefix = "keeping the open journeys current failed: ";      // per-tick work cap (review M8); the rest continues next tick

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

// The opaque handle of a LIVE UIObject, for identity comparisons only (never dereferenced, never used to
// build a View). Legal access: a pointer-to-member formed in a derived class names the protected member.
struct HandleAccess : public View
{
   static const void* Of( const UIObject& o ) { return o.*(&HandleAccess::handle); }
};

// Re-resolves a main view by id for one narrow use; null when no such window is open. Never kept.
View LiveView( const std::string& id )
{
   try
   {
      return View::ViewById( IsoString( id.c_str() ) );
   }
   catch ( ... )
   {
      return View::Null();
   }
}

std::string FilePathOf( const std::string& id )
{
   const View v = LiveView( id );
   if ( v.IsNull() )
      return std::string();
   const ImageWindow w = v.Window();
   return w.IsNull() ? std::string() : U8( w.FilePath() );
}

size_type ModifyCountOf( const std::string& id )
{
   const View v = LiveView( id );
   if ( v.IsNull() )
      return 0;
   const ImageWindow w = v.Window();
   return w.IsNull() ? 0 : w.ModifyCount();
}

FITSKeywordArray KeywordsOf( const std::string& id )
{
   const View v = LiveView( id );
   if ( v.IsNull() )
      throw Error( "view " + String( id.c_str() ) + " is no longer open" );
   return v.Window().Keywords();
}

bool IsBusyId( const std::string& id )
{
   const View v = LiveView( id );
   return v.IsNull() || IsBusy( v );
}

// Geometry and sample format for MasterFingerprint(). Probed non-waiting right
// before the lock (throws ViewBusy instead of blocking); the lock does not
// notify, so it is no "process activity" for the busy gate (review M2).
struct ViewGeom { int w = 0, h = 0, ch = 0, bits = 32; bool isFloat = true; };

ViewGeom ViewGeometry( const std::string& id )
{
   View vv = LiveView( id );
   if ( vv.IsNull() )
      throw Error( "view " + String( id.c_str() ) + " is no longer open" );
   if ( IsBusy( vv ) )
      throw ViewBusy();
   ViewGeom g;
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
bool IsAuxiliaryOutput( const std::string& id, const HistorySnapshot& snap )
{
   if ( !FilePathOf( id ).empty() || !IsIntegrationAuxiliary( id, snap.steps ) )
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

// Removes the entries for which drop() is true.
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

JourneyTracker::Tracked* JourneyTracker::FindTracked( const void* handle )
{
   for ( const std::unique_ptr<Tracked>& t : m_tracked )
      if ( t->handle == handle )
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

bool JourneyTracker::IsCandidate( const void* handle ) const
{
   for ( const std::unique_ptr<Candidate>& c : m_candidates )
      if ( c->handle == handle )
         return true;
   return false;
}

void JourneyTracker::AddCandidate( const std::string& id, const void* handle, double now, bool fresh )
{
   // A frozen journey's image waiting for its continuation is never evaluated as a new candidate: timing
   // evidence would link it back into the kept journey (Ruling 26).
   if ( IsCandidate( handle ) || IsPendingFreeze( handle ) )
      return;
   std::unique_ptr<Candidate> c( new Candidate );
   c->handle = handle;
   c->id = id;
   c->firstSeen = now;
   c->fresh = fresh;
   m_candidates.push_back( std::move( c ) );
}

void JourneyTracker::Forget( const void* handle )
{
   EraseIf( m_candidates, [handle]( const Candidate& c ) { return c.handle == handle; } );
   EraseIf( m_ignored, [handle]( const Ignored& i ) { return i.handle == handle; } );
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
   size_type n = m_events.size() + m_candidates.size() + m_pendingGaps.size() + m_pendingFreeze.size() + m_freezeRequests.size();
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
   // Read NOW, while the view is alive (inside the notification): ids and an opaque handle. No View is kept.
   PendingEvent e;
   e.kind = kind;
   e.t = now;
   try
   {
      e.preview = v.IsPreview();
      e.id = std::string( v.FullId().c_str() );
      if ( e.preview )
      {
         const View main = v.Window().MainView();
         e.mainId = ViewIdOf( main );
         e.handle = HandleAccess::Of( main );
      }
      else
      {
         e.mainId = e.id;
         e.handle = HandleAccess::Of( v );
      }
   }
   catch ( ... )
   {
      m_forceScan = true;   // unreadable: the next scan re-derives the state
      return;
   }
   e.seq = ++m_seq;
   if ( m_events.size() >= 1000 )
   {
      // Bounded (data only). Updated / Renamed / Saved / Focused / Created are re-derived by the forced scan
      // (ModifyCount and id comparisons); deaths are never in this queue (m_deaths).
      m_events.pop_front();
      m_forceScan = true;
   }
   m_events.push_back( std::move( e ) );
}

void JourneyTracker::OnImageCreated( const View& view, double now ) { QueueEvent( EventKind::Created, view, now ); }
void JourneyTracker::OnImageUpdated( const View& view, double now ) { QueueEvent( EventKind::Updated, view, now ); }
void JourneyTracker::OnImageRenamed( const View& view, double now ) { QueueEvent( EventKind::Renamed, view, now ); }
void JourneyTracker::OnImageDeleted( const View& view, double /*now*/ )
{
   // Re-review I-1r: a death is recorded even while recording is off (an opaque integer, no image data),
   // never dropped, and consumed exactly once. Read now, while the view is alive.
   try
   {
      if ( view.IsNull() || view.IsPreview() )
         return;
      Death d;
      d.seq = ++m_seq;
      d.handle = HandleAccess::Of( view );
      if ( m_deaths.size() >= 100000 )
      {
         m_deaths.clear();
         m_identityLost = true;   // backstop: every entry is treated as closed at the next DropClosed
      }
      m_deaths.push_back( d );
      m_forceScan = true;
   }
   catch ( ... )
   {
      m_identityLost = true;
   }
}

double JourneyTracker::FirstSeenForSelfTest( const IsoString& viewId ) const
{
   const std::string id( viewId.c_str() );
   for ( const std::unique_ptr<Candidate>& c : m_candidates )
      if ( c->id == id )
         return c->firstSeen;
   for ( const std::unique_ptr<Ignored>& i : m_ignored )
      if ( i->id == id )
         return i->firstSeen;
   return -1;
}
void JourneyTracker::OnImageSaved( const View& view, double now )   { QueueEvent( EventKind::Saved, view, now ); }
void JourneyTracker::OnImageFocused( const View& view, double now ) { QueueEvent( EventKind::Focused, view, now ); }

// Called only at the start of Tick(), after DropClosed(). Events are data: nothing here touches a view.
void JourneyTracker::DrainEvents()
{
   while ( !m_events.empty() )
   {
      try
      {
         ApplyEvent( m_events.front() );
      }
      catch ( ... )
      {
         m_forceScan = true;
      }
      m_events.pop_front();
   }
}

void JourneyTracker::ApplyEvent( const PendingEvent& e )
{
   if ( e.handle == nullptr )
      return;   // its window was closed (DropClosed cleared it): nothing to apply
   switch ( e.kind )
   {
   case EventKind::Created:
      if ( !e.preview && FindTracked( e.handle ) == nullptr )
         AddCandidate( e.id, e.handle, e.t, true/*fresh: it appeared*/ );
      break;
   case EventKind::Updated:
      if ( e.preview )
         break;   // Ruling 25: previews are not part of the image's journey
      if ( Tracked* t = FindTracked( e.handle ) )
      {
         t->dirty = true;
         break;
      }
      {
         double seen = -1;
         std::string id = e.id;
         for ( const std::unique_ptr<Ignored>& i : m_ignored )
            if ( i->handle == e.handle )
               seen = i->firstSeen;
         if ( seen >= 0 )
         {
            const void* h = e.handle;
            EraseIf( m_ignored, [h]( const Ignored& x ) { return x.handle == h; } );
            // Re-evaluated (a new step may now reference a tracked view), but it did not APPEAR now:
            // never linked by timing (c) (review I3), and its first sighting is kept (re-review N3).
            AddCandidate( id, e.handle, seen, false );
         }
      }
      break;
   case EventKind::Renamed:
      m_forceScan = true;   // the scan matches the handle and takes the new id
      m_renameSinceScan = true;
      if ( !e.preview )
         m_renamedHandles.insert( e.handle );
      break;
   case EventKind::Deleted:
      m_forceScan = true;   // DropClosed() (every tick) removes it and ends its journey if it was the last open image
      break;
   case EventKind::Saved:
      if ( Tracked* t = FindTracked( e.handle ) )
         t->dirty = true;   // ModifyCount was reset; the file path may have changed
      m_forceScan = true;
      break;
   case EventKind::Focused:
      NoteActive( e.mainId, e.t );   // the event's own time
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

uint64 JourneyTracker::NoteCopilotStep( const IsoString& viewFullId, const std::string& processId, const std::string& reason,
                                        const std::vector<std::string>& createdWindowIds, bool integration, double now )
{
   const std::string vid( viewFullId.c_str() );
   uint64 token = 0;
   if ( !vid.empty() && !processId.empty() )
   {
      token = ++m_noteToken;
      m_copilot.push_back( { vid, processId, reason, now, token, false } );
   }
   NoteCopilotCreated( viewFullId, createdWindowIds, integration, now );
   for ( const std::unique_ptr<Tracked>& t : m_tracked )
      if ( t->id == vid )
         t->dirty = true;
   m_forceScan = true;
   return token;
}

void JourneyTracker::NoteCopilotCreated( const IsoString& sourceViewFullId, const std::vector<std::string>& createdWindowIds,
                                         bool integration, double now )
{
   const std::string vid( sourceViewFullId.c_str() );
   // Ruling 29: of an integration run's windows only the first (the result) may become a master.
   for ( size_t i = 0; i < createdWindowIds.size(); ++i )
      m_created.push_back( { createdWindowIds[i], vid, integration, i == 0, now } );
   if ( !createdWindowIds.empty() )
      m_forceScan = true;
}

void JourneyTracker::CancelCopilotNote( uint64 token )
{
   if ( token == 0 )
      return;
   m_copilot.erase( std::remove_if( m_copilot.begin(), m_copilot.end(),
                                    [token]( const CopilotNote& n ) { return n.token == token; } ), m_copilot.end() );
}

void JourneyTracker::SetCopilotNoteNoEffect( uint64 token )
{
   for ( CopilotNote& n : m_copilot )
      if ( token != 0 && n.token == token )
         n.noEffect = true;
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
      std::vector<OpenWindow> open;
      DropClosed( open );           // first
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
         Scan( now, open );
      }
      if ( !m_freezeRequests.empty() )
      {
         std::vector<int64> requests;
         requests.swap( m_freezeRequests );
         for ( int64 jid : requests )
         {
            String e;
            FreezeNow( jid, e );
            if ( !e.IsEmpty() )
               m_joinNotes << "PI Copilot: " + e;
         }
      }
      ProcessPendingFreezes();
      FlushPendingGaps();
      ProcessDirty( now );
      ProcessCandidates( now );
      EndClosedJourneys();
      if ( m_reconcile )
         Reconcile();
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

void JourneyTracker::DropClosed( std::vector<OpenWindow>& open )
{
   // The open windows as DATA; every ImageWindow / View temporary here dies before this function returns,
   // while its window is certainly open.
   for ( const ImageWindow& w : ImageWindow::AllWindows() )
   {
      const View mv = w.MainView();
      open.push_back( { HandleAccess::Of( mv ), ViewIdOf( mv ), w.ModifyCount() } );
   }
   // Deaths name windows that are gone even if the core already reused their addresses for new windows
   // (re-review I-1r). The latest death per handle; every death is consumed here, exactly once.
   std::map<const void*, uint64> dead;
   for ( const Death& d : m_deaths )
      dead[d.handle] = std::max( dead[d.handle], d.seq );
   m_deaths.clear();
   const bool identityLost = m_identityLost;
   m_identityLost = false;
   if ( !dead.empty() || identityLost )
      m_forceScan = true;
   auto deathOf = [&dead]( const void* h ) -> uint64
   {
      const auto it = dead.find( h );
      return it == dead.end() ? 0 : it->second;
   };
   auto isOpen = [&open, &dead, identityLost]( const void* h )
   {
      if ( identityLost || dead.count( h ) > 0 )
         return false;
      for ( const OpenWindow& o : open )
         if ( o.handle == h )   // integer comparison against live handles: nothing is dereferenced
            return true;
      return false;
   };
   for ( const std::unique_ptr<Tracked>& t : m_tracked )
      if ( !isOpen( t->handle ) )
      {
         m_closed.push_back( t->journeyId );
         m_closedImages.push_back( t->imageId );
         // R4: closed with changes not recorded yet (dirty, or an Updated notification still queued) --
         // e.g. during a busy stall: the lost steps become a gap, never silently nothing.
         bool pending = t->dirty;
         const uint64 died = deathOf( t->handle );
         for ( const PendingEvent& e : m_events )   // only updates queued before its death were its own
            pending = pending || (e.kind == EventKind::Updated && e.handle == t->handle && (died == 0 || e.seq < died));
         if ( died != 0 || identityLost )
            Decision( "closed (" + std::string( died != 0 ? "death" : "identity lost" ) + "): " + t->id );
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
               // The seq could not be read (locked, row gone): the gap is written at 0, the conservative
               // "somewhere in this image" (re-review m-d). A vanished row is discarded by FlushPendingGaps.
            }
            m_pendingGaps.push_back( { t->journeyId, t->imageId, lastSeq,
                                       "closed before its last steps were recorded" } );
            Decision( "gap: " + t->id + " closed with unrecorded changes (image " + std::to_string( t->imageId )
                      + (t->dirty ? ", dirty" : ", queued update") + ")" );
         }
      }
   EraseIf( m_tracked, [&]( const Tracked& t ) { return !isOpen( t.handle ); } );
   // A frozen journey's image closed before it could join its continuation: nothing more to record for it.
   m_pendingFreeze.erase( std::remove_if( m_pendingFreeze.begin(), m_pendingFreeze.end(),
                                          [&]( const PendingFreeze& p ) { return !isOpen( p.handle ); } ), m_pendingFreeze.end() );
   EraseIf( m_candidates, [&]( const Candidate& c ) { return !isOpen( c.handle ); } );
   EraseIf( m_ignored, [&]( const Ignored& i ) { return !isOpen( i.handle ); } );
   // Events of a dead window must not act on a new window at the reused address: only those queued BEFORE
   // the death are discarded; later ones belong to the new window (re-review I-1r (c)).
   for ( auto& e : m_events )
   {
      const uint64 died = deathOf( e.handle );
      if ( died != 0 && e.seq < died )
         e.handle = nullptr;
   }
}

void JourneyTracker::Scan( double now, const std::vector<OpenWindow>& open )
{
   for ( const OpenWindow& o : open )
   {
      if ( Tracked* t = FindTracked( o.handle ) )
      {
         const std::string& id = o.id;
         if ( id != t->id )
         try
         {
            // The same live handle under a new id. With a rename notification since the last scan, a rename.
            // Without one (notifications working), it cannot be told from a new window at a reused address:
            // re-review I-1r -- the old window is closed and this one is a new window (a master re-joins its
            // own journey through the fingerprint resume).
            if ( m_renamedHandles.count( o.handle ) == 0 && PICopilotJourneyNotificationsWork )
            {
               char head[48];
               std::snprintf( head, sizeof head, "%.3f ", now );
               Decision( head + std::string( "id change without a rename notification (closed + new): " ) + t->id + " -> " + id );
               m_closed.push_back( t->journeyId );
               m_closedImages.push_back( t->imageId );
               if ( t->dirty )
                  m_pendingGaps.push_back( { t->journeyId, t->imageId, 0, "closed before its last steps were recorded" } );
               const void* h = o.handle;
               EraseIf( m_tracked, [h]( const Tracked& x ) { return x.handle == h; } );
               AddCandidate( id, h, now, m_scannedOnce );
               continue;
            }
            m_store->SetImageView( t->imageId, id, FilePathOf( id ) );
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
         if ( m_useModifyCount && o.modifyCount != t->modifyCount )
         {
            t->dirty = true;
            t->modifyCount = o.modifyCount;
         }
         continue;
      }
      bool known = false;
      for ( PendingFreeze& p : m_pendingFreeze )
         if ( p.handle == o.handle )
         {
            p.id = o.id;   // follows a rename; joins its continuation when free
            known = true;
         }
      for ( const std::unique_ptr<Candidate>& c : m_candidates )
         if ( c->handle == o.handle )
         {
            c->id = o.id;   // follows a rename
            known = true;
         }
      if ( known )
         continue;
      bool ignored = false;
      for ( const std::unique_ptr<Ignored>& ig : m_ignored )
         if ( ig->handle == o.handle )
         {
            ignored = true;
            ig->id = o.id;
            if ( ig->modifyCount != o.modifyCount )
            {
               const double seen = ig->firstSeen;   // N3: the first sighting, not the time of this change
               const void* h = o.handle;
               EraseIf( m_ignored, [h]( const Ignored& x ) { return x.handle == h; } );
               AddCandidate( o.id, o.handle, seen, false/*changed, not new: no timing (c)*/ );
            }
            break;
         }
      if ( !ignored )
         AddCandidate( o.id, o.handle, now, m_scannedOnce/*never seen before; not fresh when it was open before recording began*/ );
   }
   m_renamedHandles.clear();
   m_renameSinceScan = false;
   m_scannedOnce = true;
   if ( !m_useModifyCount )
      try
      {
         BatchCounts();
      }
      catch ( ... )
      {
         // m-l: the batch read failed; every image it would have probed is read by ProcessDirty instead.
         for ( const std::unique_ptr<Tracked>& t : m_tracked )
            t->dirty = true;
      }
}

void JourneyTracker::BatchCounts()
{
   // Busy views are not read (global constraint, pre-flight P8): they are
   // marked dirty, and ProcessDirty() defers them until they are free.
   std::vector<Tracked*> probed;
   std::string ids = "[";
   for ( const std::unique_ptr<Tracked>& t : m_tracked )
   {
      if ( IsBusyId( t->id ) )
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
      if ( IsBusyId( t.id ) )
      {
         ++m_deferrals;   // never waited on: the next tick tries again (a closed window: DropClosed next tick)
         continue;
      }
      processed = true;
      // R2: each image's DB work is its own failure domain. A vanished row drops that image; any other
      // failure pauses that image only; the loop goes on with the next image.
      try
      {
         ProcessOne( t, now );
      }
      catch ( const JourneyRowMissing& x )   // a missing row or SQLITE_CONSTRAINT_FOREIGNKEY (m-f)
      {
         gone.push_back( { k, x.Message() } );
      }
      catch ( const pcl::Exception& x )
      {
         t.pausedReason = x.Message();
         if ( ++t.writeFailures >= 3 )
         {
            t.dirty = false;   // m-h: parked until the next change, like a read failure; the pause stays visible
            t.writeFailures = 0;
         }
      }
      catch ( const std::exception& x )
      {
         t.pausedReason = String( x.what() );
         if ( ++t.writeFailures >= 3 )
         {
            t.dirty = false;
            t.writeFailures = 0;
         }
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
         StepRow row = MakeStepRow( h, t.imageId, d.appendedState[i], n >= 0 ? "copilot" : "user",
                                    n >= 0 ? m_copilot[n].reason : std::string(), snap.ActiveCount() );
         if ( n >= 0 && m_copilot[n].noEffect )
            row.params["noEffect"] = true;   // T-graxpert: success reported, nothing changed; skipped like a base step
         const int64 sid = m_store->AddStep( row );
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
   t.writeFailures = 0;
   t.lastCounts = { snap.initialLength, snap.length, snap.historyIndex };
   if ( lastActive != 0 )
   {
      // Ruling 7: only the last active step appended in this tick is measured. Steps made between two
      // observations share this one measurement; the intermediate pixels no longer exist. A failure here
      // is this image's statistics pause (P11, re-review m4), never a lost step.
      try
      {
         const View live = LiveView( t.id );   // resolved for this call only
         if ( live.IsNull() )
            throw Error( "view " + String( t.id.c_str() ) + " is no longer open" );
         const StepStatsResult s = ComputeStepStats( live, m_store->JourneyDir( t.journeyId )
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
   const void* handle = t->handle;
   const std::string id = t->id;
   m_joinNotes << "PI Copilot: the recorded row of " + String( id.c_str() )
                  + " was removed from the journey library; recording of " + String( id.c_str() ) + " stopped (" + why + ")";
   Decision( "dropped " + id + ": " + U8( why ) );
   const int64 img = t->imageId;
   // N1: nothing queued for the vanished image may be retried on every tick.
   m_pendingGaps.erase( std::remove_if( m_pendingGaps.begin(), m_pendingGaps.end(),
                                        [img]( const GapRow& g ) { return g.imageId == img; } ), m_pendingGaps.end() );
   m_closedImages.erase( std::remove( m_closedImages.begin(), m_closedImages.end(), img ), m_closedImages.end() );
   m_recent.erase( std::remove_if( m_recent.begin(), m_recent.end(),
                                   [img]( const RecentStep& r ) { return r.imageId == img; } ), m_recent.end() );
   m_created.erase( std::remove_if( m_created.begin(), m_created.end(),
                                    [&id]( const CreatedNote& n ) { return n.sourceViewId == id; } ), m_created.end() );
   m_reconcile = true;
   m_tracked.erase( m_tracked.begin() + index );
   AddCandidate( id, handle, JourneyWallNow(), false/*not fresh: it existed; it may start a new journey by the rules*/ );
}

void JourneyTracker::ProcessCandidates( double now )
{
   bool evaluated = false;
   for ( size_t i = 0; i < m_candidates.size(); )
   {
      Candidate& c = *m_candidates[i];
      if ( evaluated && OverTickBudget() )
         break;   // review M8: the rest waits for the next tick (not counted as a look)
      if ( IsBusyId( c.id ) )
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
      catch ( const JourneyRowMissing& x )
      {
         // Re-review N2: a join into a journey (or from an image) whose row vanished. The candidate is
         // re-evaluated next tick against a reconciled tracker (it may then be a master or ignored).
         m_reconcile = true;
         m_evalNote += " rowMissing: " + U8( x.Message() );
         // I-A: the first occurrence only defers (Reconcile gets its chance); a repeat counts as a failure,
         // so every error kind is bounded by kCandidateTicks.
         r = ++c.rowMissing <= 1 ? kCandidateDeferred : (++c.failures >= kCandidateTicks ? -1 : kCandidateDeferred);
      }
      catch ( const pcl::Exception& x )
      {
         // Any other failure: this candidate only; after kCandidateTicks such failures it is ignored.
         m_evalNote += " failed: " + U8( x.Message() );
         r = ++c.failures >= kCandidateTicks ? -1 : kCandidateDeferred;
      }
      catch ( const std::exception& x )
      {
         m_evalNote += std::string( " failed: " ) + x.what();
         r = ++c.failures >= kCandidateTicks ? -1 : kCandidateDeferred;
      }
      {
         char head[96];
         std::snprintf( head, sizeof head, "%.3f r=%d ticks=%d ", now, r, c.ticks );
         Decision( head + c.id + " " + m_evalNote );
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
         ig->handle = c.handle;
         ig->id = c.id;
         ig->modifyCount = ModifyCountOf( c.id );
         ig->firstSeen = c.firstSeen;
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
   if ( FindTracked( c.handle ) != nullptr )
      return 1;   // review M1: already joined (never twice)
   const std::string id = c.id;
   // The history: re-read on the first two looks and whenever the window
   // changed; otherwise the last read is re-evaluated against the tracker's
   // current state (new tracked views, Copilot notes) at no cost (review M8).
   const size_type mc = ModifyCountOf( id );
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
            return JoinAsMaster( id, c.handle, snap, KeywordsOf( id ),
                                 "created by Copilot's run_global_process of an integration process", c.firstSeen ) != 0 ? 1 : -1;
         }
         if ( const Tracked* src = FindTrackedById( n.sourceViewId ) )
         {
            const int64 srcImage = src->imageId, srcJourney = src->journeyId;
            const std::vector<StepRow> steps = m_store->Steps( srcImage, false );
            return JoinLinked( id, c.handle, snap, srcJourney, { { srcImage, steps.empty() ? 0 : steps.back().id, 0 } },
                               "copilot", "linked by copilot" ) != 0 ? 1 : -1;
         }
      }
   // Ruling 29: an auxiliary output of a hand-run integration is never tracked.
   if ( IsAuxiliaryOutput( id, snap ) )
      return -1;
   std::vector<std::string> ids;
   for ( const HistoryStep& h : snap.steps )
      ids.push_back( h.processId );
   const FITSKeywordArray kw = KeywordsOf( id );
   const MasterEvidence me = DetectMaster( ids, kw );
   // (2) a master of its own. Ruling 28 (pre-flight P14): a window CREATED from another one (initialLength > 0)
   //     whose history does not begin with an integration is decided by link evidence first, and by the
   //     keyword master rules (Ruling 1 rules 2-4) only when no link evidence (except timing (c)) exists,
   //     so a derived window that inherited IMAGETYP='Master Light' joins its source's journey.
   const bool derived = snap.initialLength > 0 && !HistoryBeginsWithIntegration( snap );
   if ( me.isMaster && !derived )
      return JoinAsMaster( id, c.handle, snap, kw, me.why, c.firstSeen ) != 0 ? 1 : -1;
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
      return JoinLinked( id, c.handle, snap, journeyId, links, "reference", "linked by reference" ) != 0 ? 1 : -1;
   }
   // (4) timing (a)/(b) (Ruling 19.3).
   if ( snap.initialLength > 0 )
   {
      // (a) the creating step is a recorded step (it also changed its source).
      for ( const RecentStep& r : m_recent )
         if ( r.identity == snap.steps.front().identity )
            return JoinLinked( id, c.handle, snap, r.journeyId, { { r.imageId, r.stepId, 0 } }, "timing", "linked by timing" ) != 0 ? 1 : -1;
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
         if ( src != nullptr && src->handle != c.handle )
         {
            const int64 srcImage = src->imageId, srcJourney = src->journeyId;
            const std::vector<StepRow> steps = m_store->Steps( srcImage, false );
            return JoinLinked( id, c.handle, snap, srcJourney, { { srcImage, steps.empty() ? 0 : steps.back().id, 0 } },
                               "timing", "linked by timing" ) != 0 ? 1 : -1;
         }
      }
   }
   // (5) Ruling 28: a derived window with master keywords and no link evidence is a master of its own.
   if ( me.isMaster )
      return JoinAsMaster( id, c.handle, snap, kw, me.why, c.firstSeen ) != 0 ? 1 : -1;
   // (6) timing (c), last: a window that APPEARED (review I3: a Created notification or the first sight of
   //     a view, never an ignored window that changed later) and is no opened file, first seen inside
   //     exactly one recorded step's time window. Never decided on the first look, and 2+ hits wait
   //     instead of rejecting: a window can be seen by a tick running while its creating process still
   //     executes, before its creating step is attached (J6 (f3)). Still ignored after kCandidateTicks.
   if ( !c.fresh || !FilePathOf( id ).empty() )
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
      return JoinLinked( id, c.handle, snap, hit->journeyId, { { hit->imageId, hit->stepId, 0 } }, "timing", "linked by timing" ) != 0 ? 1 : -1;
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
      const View live = LiveView( t.id );   // resolved for this call only
      if ( live.IsNull() )
         throw Error( "view " + String( t.id.c_str() ) + " is no longer open" );
      const StepStatsResult s = ComputeStepStats( live, m_store->JourneyDir( t.journeyId )
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

int64 JourneyTracker::JoinAsMaster( const std::string& id, const void* handle, const HistorySnapshot& snap,
                                    const FITSKeywordArray& kw, const std::string& why, double firstSeen )
{
   const std::string path = FilePathOf( id );
   const ViewGeom g = ViewGeometry( id );
   const std::vector<std::string> identities = StepIdentities( snap );
   std::unique_ptr<Tracked> t( new Tracked );
   t->handle = handle;
   t->id = id;
   t->modifyCount = ModifyCountOf( id );
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
         try
         {
            t->hasGaps = m_store->HasReadGaps( row.id );   // re-review m-e: earlier sessions' read gaps resolve too
         }
         catch ( ... )
         {
            t->hasGaps = true;   // m-i: unknown -> only adds a ResolveGaps to its next write; never leaves it untracked
         }
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

int64 JourneyTracker::JoinLinked( const std::string& id, const void* handle, const HistorySnapshot& snap, int64 journeyId,
                                  const std::vector<PlannedLink>& links, const std::string& evidence, const std::string& why )
{
   const ViewGeom g = ViewGeometry( id );
   const std::vector<std::string> identities = StepIdentities( snap );
   const FITSKeywordArray kw = KeywordsOf( id );
   std::unique_ptr<Tracked> t( new Tracked );
   t->handle = handle;
   t->id = id;
   t->modifyCount = ModifyCountOf( id );
   t->why = why;
   t->journeyId = journeyId;
   {
      JourneyStore::Transaction tx( *m_store );   // review I5: image, steps and links together or not at all
      t->imageId = m_store->AddImage( journeyId, id, FilePathOf( id ),
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
      std::vector<OpenWindow> open;
      DropClosed( open );   // forget closed windows first (review I6)
      const std::string id = ViewIdOf( view );
      const void* handle = HandleAccess::Of( view );   // the caller's view is alive during this call
      if ( const Tracked* t = FindTracked( handle ) )
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
      if ( const Tracked* t = FindTracked( handle ) )   // re-checked after the read (review I6)
      {
         error = String().Format( "%s is already recorded in journey #%lld", id.c_str(), static_cast<long long>( t->journeyId ) );
         return 0;
      }
      const int64 jid = JoinAsMaster( id, handle, snap, KeywordsOf( id ), "started from chat (start_journey)", JourneyWallNow() );
      Forget( handle );   // re-review m5: only after the join succeeded
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

bool JourneyTracker::IsPendingFreeze( const void* handle ) const
{
   for ( const PendingFreeze& p : m_pendingFreeze )
      if ( p.handle == handle )
         return true;
   return false;
}

int64 JourneyTracker::FreezeJourney( int64 journeyId, String& error )
{
   error.Clear();
   if ( m_inTick )
   {
      // Re-entered from inside a tick (something it called pumped events): m_tracked is being iterated.
      m_freezeRequests.push_back( journeyId );
      error = String().Format( "the journey recorder was busy; journey #%lld is frozen at its next tick",
                               static_cast<long long>( journeyId ) );
      return 0;
   }
   struct Guard { bool& b; explicit Guard( bool& x ) : b( x ) { b = true; } ~Guard() { b = false; } } guard( m_inTick );
   return FreezeNow( journeyId, error );
}

int64 JourneyTracker::FreezeNow( int64 journeyId, String& error )
{
   if ( m_store == nullptr )
   {
      error = "the journey library is not available: " + m_storeError;
      return 0;
   }
   std::shared_ptr<Continuation> to( new Continuation );
   to->keptJourneyId = journeyId;
   try
   {
      JourneyRow kept;
      if ( !m_store->GetJourney( journeyId, kept ) )
      {
         error = String().Format( "no journey #%lld to freeze", static_cast<long long>( journeyId ) );
         return 0;
      }
      to->name = kept.name + " (continued)";
      to->target = kept.target;
      for ( const ImageRow& i : m_store->Images( journeyId ) )
         if ( i.isMaster && m_store->Acquisition( i.id, to->acq ) )
            break;
   }
   catch ( const pcl::Exception& x )
   {
      // The kept journey's rows could not be read (locked): its images still stop recording into it now;
      // the continuation is named after the id and made when the library answers.
      to->name = "journey #" + std::to_string( journeyId ) + " (continued)";
      error = "the kept journey could not be read (" + x.Message() + ")";
   }
   // Every image of the kept journey leaves it NOW, in memory: nothing more is recorded into it.
   size_t moved = 0;
   for ( size_t k = 0; k < m_tracked.size(); )
   {
      const Tracked& t = *m_tracked[k];
      if ( t.journeyId != journeyId )
      {
         ++k;
         continue;
      }
      m_pendingFreeze.push_back( { t.handle, t.id, to, String() } );
      m_closedImages.push_back( t.imageId );   // its kept row is no longer recorded by this instance (owner cleared)
      m_tracked.erase( m_tracked.begin() + k );
      ++moved;
   }
   // Remembered steps of the kept journey can no longer be a link's source (a new window must not join it).
   m_recent.erase( std::remove_if( m_recent.begin(), m_recent.end(),
                                   [journeyId]( const RecentStep& r ) { return r.journeyId == journeyId; } ), m_recent.end() );
   m_closed.push_back( journeyId );   // EndClosedJourneys: status "ended" (retried there on failure)
   if ( moved == 0 )
      return 0;
   // The continuation now, so the caller can name it even when every image is busy.
   try
   {
      JourneyStore::Transaction tx( *m_store );
      const int64 jid = m_store->CreateJourney( to->name, to->target, NowIso() );
      tx.Commit();
      to->journeyId = jid;
   }
   catch ( const pcl::Exception& x )
   {
      error = "the continued journey could not be created yet (" + x.Message() + "); its images join it at a later tick";
      for ( PendingFreeze& p : m_pendingFreeze )
         if ( p.to == to )
            p.reason = x.Message();
   }
   ProcessPendingFreezes();
   return to->journeyId;
}

void JourneyTracker::ProcessPendingFreezes()
{
   for ( size_t k = 0; k < m_pendingFreeze.size(); )
   {
      PendingFreeze& p = m_pendingFreeze[k];
      if ( LiveView( p.id ).IsNull() )
      {
         ++k;   // closed or renamed: DropClosed / Scan settle it on the next tick
         continue;
      }
      if ( IsBusyId( p.id ) )
      {
         ++m_deferrals;   // never waited on (P9)
         ++k;
         continue;
      }
      bool joined = false;
      try
      {
         joined = JoinContinued( p );
      }
      catch ( const ViewBusy& )
      {
         ++m_deferrals;
      }
      catch ( const JourneyRowMissing& x )
      {
         p.to->journeyId = 0;   // the continuation vanished: made again at the next attempt
         p.reason = x.Message();
      }
      catch ( const pcl::Exception& x )
      {
         p.reason = x.Message();   // visible in StatusFor; retried next tick, never dropped silently
      }
      catch ( const std::exception& x )
      {
         p.reason = String( x.what() );
      }
      if ( joined )
         m_pendingFreeze.erase( m_pendingFreeze.begin() + k );
      else
         ++k;
   }
}

bool JourneyTracker::JoinContinued( PendingFreeze& p )
{
   const std::string id = p.id;
   const HistorySnapshot snap = m_read( IsoString( id.c_str() ), 0 );
   if ( snap.busy )
   {
      ++m_deferrals;
      return false;
   }
   if ( !snap.ok )
      throw Error( snap.error );
   const ViewGeom g = ViewGeometry( id );
   const FITSKeywordArray kw = KeywordsOf( id );
   const std::string now = NowIso();
   std::unique_ptr<Tracked> t( new Tracked );
   t->handle = p.handle;
   t->id = id;
   t->modifyCount = ModifyCountOf( id );
   t->why = "continues kept journey #" + std::to_string( p.to->keptJourneyId );
   int64 jid = p.to->journeyId;
   {
      JourneyStore::Transaction tx( *m_store );   // the continuation's image exists whole or not at all
      if ( jid == 0 )
         jid = m_store->CreateJourney( p.to->name, p.to->target, now );
      t->imageId = m_store->AddImage( jid, id, FilePathOf( id ),
                                      MasterFingerprint( g.w, g.h, g.ch, g.bits, g.isFloat, StepIdentities( snap ), kw ), true, now );
      m_store->SetImageOwner( t->imageId, m_owner );
      m_store->SetAcquisition( t->imageId, p.to->acq );
      AddBaseAndSteps( t->imageId, snap, snap.TotalCount() );   // the kept result is its starting point
      m_store->SetJourneyStatus( jid, "recording" );
      m_store->TouchJourney( jid, now );
      tx.Commit();
   }
   p.to->journeyId = jid;
   t->journeyId = jid;
   t->dirty = false;
   t->lastCounts = { snap.initialLength, snap.length, snap.historyIndex };
   Tracked& tr = *t;
   m_tracked.push_back( std::move( t ) );
   StartingStats( tr );
   m_joinNotes << String().Format( "PI Copilot: %s continues kept journey #%lld as journey #%lld",
                                   id.c_str(), static_cast<long long>( p.to->keptJourneyId ), static_cast<long long>( jid ) );
   return true;
}

void JourneyTracker::FlushPendingGaps()
{
   // Fix round 3 (N1): one boundary per gap. A gap whose image / journey vanished is discarded (there is
   // nothing left to mark); any other failure keeps THAT gap for the next tick and the rest go on.
   std::vector<GapRow> keep;
   for ( const GapRow& g : m_pendingGaps )
      try
      {
         m_store->AddGap( g );
      }
      catch ( const JourneyRowMissing& x )
      {
         Decision( "gap of vanished image " + std::to_string( g.imageId ) + " discarded: " + U8( x.Message() ) );
         m_reconcile = true;
      }
      catch ( ... )
      {
         keep.push_back( g );
      }
   m_pendingGaps.swap( keep );
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
            m_closed.push_back( jid );   // this journey only: retried on the next tick; the others go on
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
         m_closedImages.push_back( images[k] );   // this image only, next tick
      }
}

void JourneyTracker::Reconcile()
{
   bool retry = false;
   for ( size_t k = m_tracked.size(); k-- > 0; )
   {
      const Tracked& t = *m_tracked[k];
      try
      {
         ImageRow ir;
         JourneyRow jr;
         if ( !m_store->GetImage( t.imageId, ir ) || !m_store->GetJourney( t.journeyId, jr ) )
         {
            DropMissing( k, "its row is no longer in the journey library" );
         }
      }
      catch ( ... )
      {
         retry = true;   // could not check (locked): again at the next tick's end
      }
   }
   m_reconcile = retry;   // DropMissing set it; the rows re-checked here are settled
   auto journeyGone = [this]( int64 jid )
   {
      try
      {
         JourneyRow jr;
         return !m_store->GetJourney( jid, jr );
      }
      catch ( ... )
      {
         return false;   // unknown: kept, checked again later
      }
   };
   auto recentGone = [this, &journeyGone]( const RecentStep& r )
   {
      // I-A: a remembered step whose journey, image or step row vanished can never be a link's source.
      try
      {
         ImageRow ir;
         StepRow sr;
         return journeyGone( r.journeyId ) || !m_store->GetImage( r.imageId, ir ) || (r.stepId != 0 && !m_store->GetStep( r.stepId, sr ));
      }
      catch ( ... )
      {
         return false;
      }
   };
   m_recent.erase( std::remove_if( m_recent.begin(), m_recent.end(), recentGone ), m_recent.end() );
   m_closed.erase( std::remove_if( m_closed.begin(), m_closed.end(), journeyGone ), m_closed.end() );
   m_pendingGaps.erase( std::remove_if( m_pendingGaps.begin(), m_pendingGaps.end(),
                                        [&]( const GapRow& g ) { return journeyGone( g.journeyId ); } ), m_pendingGaps.end() );
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
      for ( const PendingFreeze& p : m_pendingFreeze )
         if ( p.id == std::string( viewFullId.c_str() ) )
         {
            // Ruling 26: never shown as recording while it waits to continue a kept journey.
            s.state = RecordingState::Paused;
            s.journeyId = p.to->journeyId;
            s.name = p.to->name;
            s.target = p.to->target;
            s.reason = String().Format( "waiting to continue kept journey #%lld: ", static_cast<long long>( p.to->keptJourneyId ) )
                     + (p.reason.IsEmpty() ? String( "the image is busy" ) : p.reason);
         }
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
   if ( today != openTouchedDate && open.empty() )
      openTouchedDate = today;   // re-review m-b: nothing open, no write lock
   if ( today != openTouchedDate )
   {
      // R1: per instance, once per local date, independent of lastRun (another instance may already have
      // pruned today): every open journey's `updated` stays within a day of now, so no instance's cutoff
      // (>= 1 day) reaches it.
      try
      {
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
      }
      catch ( const pcl::Exception& x )
      {
         throw Error( String( kTouchFailurePrefix ) + x.Message() );   // m-k: retried after the back-off
      }
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
   if ( !m_host && !m_keeper && !m_tracker && !m_store )
      return;
   (void)m_host.release();
   (void)m_keeper.release();   // its job threads are never joined here either
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
   if ( m_keeper )
      m_keeper->SetStore( m_store.get() );   // a store switch cancels + waits for pending write-ups first
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
   m_keeper.reset( new KeeperExporter( nullptr ) );
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
   // The keeper goes first: a pending write-up's destructor cancels and waits
   // for its worker, and must never outlive the store it would write into.
   m_keeper.reset();
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
   const double now = JourneyWallNow();
   if ( now < m_retentionRetryAfter )
      return;   // re-review m-b: backing off after a failure (no 250 ms busy-wait on every tick)
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
      m_retentionRetryAfter = now + 60;
      if ( failure != m_lastRetentionError )   // one note per distinct failure, not per pass
      {
         // m-k: a failed touch is retried after the 60 s back-off; a failed prune waits for the next date.
         const String m = failure.StartsWith( kTouchFailurePrefix )
                        ? "PI Copilot: journey retention is waiting (retried in a minute): " + failure
                        : "PI Copilot: journey retention paused until tomorrow: " + failure;
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
   if ( m_keeper && m_store && !m_store->InTransaction() )
   {
      // Finished keeper write-ups (Task 9): journey.md, inferred reasons, the export copy.
      // Poll() turns every failure of a job into a named note; it never throws for one.
      // Deferred while a transaction is open (its reasons are a transaction of their own).
      StringList notes;
      m_keeper->Poll( notes );
      for ( const String& n : notes )
      {
         Console().NoteLn( n );
         AddNote( n );
      }
   }
   if ( m_store && m_tracker->Enabled() && !RecorderActivity().busy && !m_store->InTransaction() )
      RunRetention();
}

#define PICOPILOT_FORWARD( call ) \
   do { if ( m_tracker ) m_tracker->call; if ( m_forward != nullptr ) m_forward->call; } while ( false )
void JourneyService::OnImageCreated( const View& v ) { PICOPILOT_FORWARD( OnImageCreated( v, JourneyWallNow() ) ); }
void JourneyService::OnImageUpdated( const View& v ) { PICOPILOT_FORWARD( OnImageUpdated( v, JourneyWallNow() ) ); }
void JourneyService::OnImageRenamed( const View& v ) { PICOPILOT_FORWARD( OnImageRenamed( v, JourneyWallNow() ) ); }
void JourneyService::OnImageDeleted( const View& v ) { PICOPILOT_FORWARD( OnImageDeleted( v, JourneyWallNow() ) ); }
void JourneyService::OnImageSaved( const View& v )   { PICOPILOT_FORWARD( OnImageSaved( v, JourneyWallNow() ) ); }
void JourneyService::OnImageFocused( const View& v ) { PICOPILOT_FORWARD( OnImageFocused( v, JourneyWallNow() ) ); }
#undef PICOPILOT_FORWARD

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
