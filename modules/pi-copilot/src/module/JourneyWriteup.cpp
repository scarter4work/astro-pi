// PI Copilot — Native PCL Module for PixInsight
// Copyright (c) 2026 Scott Carter. MIT License.

#include "JourneyWriteup.h"
#include "SafeFileWrite.h"
#include "Utf8.h"

#include <pcl/AutoPointer.h>
#include <pcl/Exception.h>
#include <pcl/File.h>

#include <algorithm>

namespace pcl
{

namespace
{

const char* const kWriteupSystem =
   "You write journey.md: a short, readable account of how one astrophotography image was made in PixInsight, for "
   "the photographer and the people they share it with. You receive a condensed recipe (JSON): the acquisition facts, "
   "the processing steps in order (process, who did it: the user or PI Copilot, the stated reason if any, parameters, "
   "and the median/noise before and after), the links between images and any gaps. You receive no images.\n"
   "Write Markdown with exactly these sections: '# <journey name>', '## Equipment', '## Acquisition', '## Processing', "
   "'## Notes'. In Processing, describe the steps in order, grouped by image, in plain language; say what each step did "
   "to the numbers (e.g. median 0.08 -> 0.12) when the recipe has them; name the steps marked manual and why. Reference "
   "thumbnails as Markdown images using the thumbnail paths given (e.g. ![step 12](thumbs/12.jpg)). Where a link's "
   "evidence is timing or reference, write 'linked by timing' or 'linked by reference' instead of asserting how the "
   "image was made. Where there are gaps, write 'unknown steps here'. If omittedSteps is above zero, say that many "
   "later steps are not described. Never invent a step, a value or equipment that is not in the recipe.\n"
   "For a user step with no stated reason you may infer a likely reason; mark every inferred reason in the text with "
   "'(inferred)'. End your reply with exactly one fenced json block of the form "
   "{\"inferredReasons\": [{\"step\": <step id>, \"reason\": \"<one short sentence>\"}]} listing only the reasons you "
   "inferred (an empty list if none). Nothing after that block.";

const char* const kUserLead = "Write journey.md for this PI Copilot image journey.\n\nCONDENSED_RECIPE_JSON:\n";

const char* const kGenerator = "PI Copilot";

// {from: [per channel], to: [per channel]} of one statistic, or null.
nlohmann::json StatPair( const nlohmann::json& before, const nlohmann::json& after, const char* key )
{
   if ( !before.is_array() || !after.is_array() )
      return nullptr;
   nlohmann::json from = nlohmann::json::array(), to = nlohmann::json::array();
   for ( const nlohmann::json& c : before ) from.push_back( c.at( key ) );
   for ( const nlohmann::json& c : after )  to.push_back( c.at( key ) );
   return { { "from", from }, { "to", to } };
}

// At most `max` bytes, never splitting a UTF-8 sequence, "..." when cut.
std::string CutUtf8( const std::string& s, size_t max )
{
   if ( s.size() <= max )
      return s;
   size_t n = max;
   while ( n > 0 && (static_cast<unsigned char>( s[n] ) & 0xC0) == 0x80 )
      --n;
   return s.substr( 0, n ) + "...";
}

std::string FileText( const String& path )
{
   const ByteArray b = File::ReadFile( path );
   return std::string( reinterpret_cast<const char*>( b.Begin() ), b.Length() );
}

String MarkerPath( JourneyStore& store, int64 journeyId )
{
   // In the journey folder, OUTSIDE export/: the marker holds an absolute path,
   // and export/ is what gets copied out (pre-flight P7).
   return store.JourneyDir( journeyId ) + "/.copied-to";
}

} // namespace

// One write-up request: a ChatThread built and destroyed on the root thread.
class JourneyWriteupJob
{
public:

   JourneyWriteupJob( int64 journeyId, const String& apiKey, const nlohmann::json& recipe, const String& exportFolder,
                      const String& url )
      : m_journeyId( journeyId ), m_exportFolder( exportFolder )
   {
      m_thread = new ChatThread( apiKey, JourneyWriteupSystemPrompt(), JourneyWriteupHistory( recipe ),
                                 PICopilotJourneyWriteupModel, url, PICopilotRequestTimeoutSeconds, nlohmann::json(),
                                 JourneyWriteupShape() );
      m_thread->Start();
   }

   ~JourneyWriteupJob()
   {
      if ( m_thread && m_thread->IsActive() )
      {
         m_thread->RequestCancel();
         m_thread->Wait();
      }
   }

   JourneyWriteupJob( const JourneyWriteupJob& ) = delete;
   JourneyWriteupJob& operator =( const JourneyWriteupJob& ) = delete;

   int64 JourneyId() const { return m_journeyId; }
   const String& ExportFolder() const { return m_exportFolder; }
   bool Done() const { return !m_thread || !m_thread->IsActive(); }

   AnthropicResult Take()
   {
      AnthropicResult r;
      if ( !m_thread || !m_thread->TryTakeResult( r ) )
      {
         r = AnthropicResult();
         r.errorKind = RequestErrorKind::Internal;
         r.error = "the write-up request ended without a result";
      }
      m_thread.Destroy();
      return r;
   }

private:

   int64                   m_journeyId;
   String                  m_exportFolder;
   AutoPointer<ChatThread> m_thread;
};

std::string CondensedRecipeForWriteup( const nlohmann::json& recipe )
{
   nlohmann::json c = {
      { "journey", { { "name", recipe.at( "journey" ).at( "name" ) }, { "target", recipe.at( "journey" ).at( "target" ) } } },
      { "images", nlohmann::json::array() }, { "links", recipe.at( "links" ) }, { "gaps", recipe.at( "gaps" ) },
      { "steps", nlohmann::json::array() }, { "omittedSteps", 0 } };
   for ( const nlohmann::json& im : recipe.at( "images" ) )
   {
      const nlohmann::json& s = im.at( "startStats" );
      c["images"].push_back( { { "key", im.at( "key" ) }, { "viewId", im.at( "viewId" ) }, { "fileName", im.at( "fileName" ) },
                               { "isMaster", im.at( "isMaster" ) }, { "acquisition", im.at( "acquisition" ) },
                               { "startMedian", s.is_array() && !s.empty() ? s.at( 0 ).at( "median" ) : nlohmann::json() },
                               { "startNoise", s.is_array() && !s.empty() ? s.at( 0 ).at( "noise" ) : nlohmann::json() },
                               { "thumbnail", im.at( "thumbnail" ) } } );
   }
   for ( const nlohmann::json& st : recipe.at( "steps" ) )
   {
      const nlohmann::json p = { { "p", st.at( "parameters" ) }, { "t", st.at( "tableParameters" ) } };
      c["steps"].push_back( { { "id", st.at( "id" ) }, { "image", st.at( "image" ) }, { "seq", st.at( "seq" ) },
                              { "processId", st.at( "processId" ) }, { "actor", st.at( "actor" ) }, { "reason", st.at( "reason" ) },
                              { "reasonInferred", st.at( "reasonInferred" ) }, { "manual", st.at( "manual" ) },
                              { "manualWhy", st.at( "manualWhy" ) },
                              { "median", StatPair( st.at( "statsBefore" ), st.at( "statsAfter" ), "median" ) },
                              { "noise", StatPair( st.at( "statsBefore" ), st.at( "statsAfter" ), "noise" ) },
                              { "parameters", CutUtf8( p.dump(), PICopilotJourneyWriteupParamChars ) },
                              { "thumbnail", st.at( "thumbnail" ) } } );
   }
   std::string out = c.dump();
   int omitted = 0;
   while ( out.size() > PICopilotJourneyWriteupInputChars && !c["steps"].empty() )
   {
      // Drop from the end in chunks of a tenth; the count tells the model.
      const size_t drop = std::max<size_t>( 1, c["steps"].size()/10 );
      for ( size_t i = 0; i < drop && !c["steps"].empty(); ++i, ++omitted )
         c["steps"].erase( c["steps"].size() - 1 );
      c["omittedSteps"] = omitted;
      out = c.dump();
   }
   return out;
}

String JourneyWriteupSystemPrompt()
{
   return FromU8( kWriteupSystem );
}

Array<AnthropicMessage> JourneyWriteupHistory( const nlohmann::json& recipe )
{
   Array<AnthropicMessage> h;
   AnthropicMessage m;
   m.role = "user";
   m.content = FromU8( std::string( kUserLead ) + CondensedRecipeForWriteup( recipe ) );
   h << m;
   return h;
}

RequestShape JourneyWriteupShape()
{
   RequestShape s;   // stream = false, no caching, no thinking binding
   s.maxTokens = PICopilotJourneyWriteupMaxTokens;
   return s;
}

WriteupReply ParseWriteupReply( const std::string& text )
{
   WriteupReply r;
   r.ok = true;
   r.markdown = text;
   const std::string fence( 3, '`' );   // spelled this way so no source line holds three literal backticks
   const size_t at = text.rfind( fence + "json" );
   if ( at == std::string::npos )
   {
      r.note = "the reply had no inferredReasons block; inferred reasons were not recorded";
      return r;
   }
   const size_t bodyStart = text.find( '\n', at );
   const size_t close = bodyStart == std::string::npos ? std::string::npos : text.find( fence, bodyStart );
   if ( close == std::string::npos )
   {
      r.note = "the reply's inferredReasons block is not closed; inferred reasons were not recorded";
      return r;
   }
   std::string md = text.substr( 0, at );
   while ( !md.empty() && (md.back() == '\n' || md.back() == ' ') )
      md.pop_back();
   r.markdown = md + "\n" + text.substr( std::min( text.size(), close + fence.size() ) );
   while ( r.markdown.size() > 1 && r.markdown.back() == '\n' && r.markdown[r.markdown.size() - 2] == '\n' )
      r.markdown.pop_back();
   try
   {
      const nlohmann::json j = nlohmann::json::parse( text.substr( bodyStart + 1, close - bodyStart - 1 ) );
      for ( const nlohmann::json& e : j.at( "inferredReasons" ) )
         if ( e.is_object() && e.contains( "step" ) && e.at( "step" ).is_number_integer()
           && e.contains( "reason" ) && e.at( "reason" ).is_string() && !e.at( "reason" ).get<std::string>().empty() )
            r.inferred.push_back( { e.at( "step" ).get<int64>(), e.at( "reason" ).get<std::string>() } );
   }
   catch ( const std::exception& x )
   {
      r.inferred.clear();
      r.note = std::string( "the reply's inferredReasons block is not valid (" ) + x.what() + "); inferred reasons were not recorded";
   }
   return r;
}

KeeperExporter::KeeperExporter( JourneyStore* store ) : m_store( store )
{
}

KeeperExporter::~KeeperExporter() = default;   // each job's destructor cancels and waits

void KeeperExporter::SetStore( JourneyStore* store )
{
   if ( store != m_store )
      m_jobs.clear();   // their store is going away: cancel + wait, nothing is written
   m_store = store;
}

bool KeeperExporter::Busy() const
{
   return !m_jobs.empty();
}

bool KeeperExporter::StartWriteup( int64 journeyId, const String& apiKey, const String& exportFolder, const String& url,
                                   KeepOutcome& o )
{
   if ( apiKey.IsEmpty() )
   {
      o.writeupError = "journey.md was not written: no Anthropic API key is set (PI Copilot settings); mark the journey "
                       "best again once it is, to write it";
      return false;
   }
   try
   {
      const nlohmann::json recipe = BuildRecipe( *m_store, journeyId, kGenerator );
      m_jobs.push_back( std::unique_ptr<JourneyWriteupJob>( new JourneyWriteupJob( journeyId, apiKey, recipe, exportFolder, url ) ) );
      o.writeupStarted = true;
      return true;
   }
   catch ( const pcl::Exception& x )
   {
      o.writeupError = "journey.md could not be started: " + x.Message();
   }
   catch ( const std::exception& x )
   {
      o.writeupError = "journey.md could not be started: " + String( x.what() );
   }
   return false;
}

void KeeperExporter::CopyNow( int64 journeyId, const String& exportFolder, KeepOutcome& o )
{
   if ( exportFolder.Trimmed().IsEmpty() )
      return;   // off (the default)
   o.copyError = CopyKeeperToExportFolder( *m_store, journeyId, exportFolder, o.copiedTo );
   o.copyDone = o.copyError.IsEmpty();
   if ( o.copyDone )
   {
      const String e = SafeWriteTextFile( MarkerPath( *m_store, journeyId ), U8( o.copiedTo ), SafeFileMode::Private );
      if ( !e.IsEmpty() )
         o.copyError = "the keeper was copied to " + o.copiedTo + ", but where it went could not be remembered: " + e;
   }
}

KeepOutcome KeeperExporter::Keep( int64 journeyId, int64 endImageId, const String& apiKey, const String& exportFolder,
                                  const String& url )
{
   KeepOutcome o;
   JourneyRow j;
   if ( m_store == nullptr || !m_store->GetJourney( journeyId, j ) )
   {
      o.files.xpsmError = o.files.recipeError = o.files.thumbsError =
         String().Format( "no journey #%lld", static_cast<long long>( journeyId ) );
      return o;
   }
   o.alreadyKept = j.kept;
   if ( !j.kept )
   {
      m_store->MarkKept( journeyId, endImageId, NowIso() );
      o.marked = true;
   }
   o.files = WriteKeeperFiles( *m_store, journeyId, kGenerator );
   if ( !StartWriteup( journeyId, apiKey, exportFolder, url, o ) )
      CopyNow( journeyId, exportFolder, o );   // no write-up coming: copy what exists now
   return o;
}

KeepOutcome KeeperExporter::Retry( int64 journeyId, const String& apiKey, const String& exportFolder, const String& url )
{
   KeepOutcome o;
   JourneyRow j;
   if ( m_store == nullptr || !m_store->GetJourney( journeyId, j ) )
   {
      o.files.xpsmError = o.files.recipeError = o.files.thumbsError =
         String().Format( "no journey #%lld", static_cast<long long>( journeyId ) );
      return o;
   }
   o.alreadyKept = j.kept;
   const String dir = ExportDirOf( *m_store, journeyId );
   const String xpsmName = String( ExportBaseName( j ) ) + ".xpsm";
   const bool needXpsm = !File::Exists( dir + "/" + xpsmName );
   const bool needRecipe = !File::Exists( dir + "/recipe.json" );
   const bool needThumbs = File::DirectoryExists( m_store->JourneyDir( journeyId ) + "/thumbs" )
                        && !File::DirectoryExists( dir + "/thumbs" );
   const bool needFiles = needXpsm || needRecipe || needThumbs;
   if ( needFiles )
   {
      if ( needXpsm ) o.redone << xpsmName;
      if ( needRecipe ) o.redone << "recipe.json";
      if ( needThumbs ) o.redone << "thumbs";
      o.files = WriteKeeperFiles( *m_store, journeyId, kGenerator );
   }
   else
   {
      o.files.xpsmOk = o.files.recipeOk = o.files.thumbsOk = true;
      o.files.dir = dir;
   }
   if ( !File::Exists( dir + "/journey.md" ) )
   {
      o.redone << "journey.md";
      if ( StartWriteup( journeyId, apiKey, exportFolder, url, o ) )
         return o;   // the copy follows the write-up (Poll)
   }
   if ( !exportFolder.Trimmed().IsEmpty() )
   {
      const String marker = MarkerPath( *m_store, journeyId );
      const String last = File::Exists( marker ) ? FromU8( FileText( marker ) ) : String();
      if ( needFiles || last.IsEmpty() || !File::DirectoryExists( last ) )
      {
         o.redone << "export copy";
         CopyNow( journeyId, exportFolder, o );
      }
   }
   return o;
}

void KeeperExporter::Finish( JourneyWriteupJob& job, StringList& notes )
{
   const int64 jid = job.JourneyId();
   const AnthropicResult r = job.Take();
   const String dir = ExportDirOf( *m_store, jid );
   if ( !r.ok )
      notes << "PI Copilot: journey.md was not written (" + r.error + "); the other keeper files are saved in " + dir
               + ". Mark the journey best again to retry.";
   else
   {
      const WriteupReply w = ParseWriteupReply( U8( r.text ) );
      String extra;
      if ( r.truncated )
         extra = String().Format( "the write-up reached its %d-token limit and is cut off", PICopilotJourneyWriteupMaxTokens );
      if ( !w.note.empty() )
         extra += (extra.IsEmpty() ? "" : "; ") + FromU8( w.note );
      const String e = SafeWriteTextFile( dir + "/journey.md", w.markdown, SafeFileMode::Shared );
      if ( !e.IsEmpty() )
         notes << "PI Copilot: journey.md could not be saved: " + e;
      else
      {
         try
         {
            // Only a user step of THIS journey with no stated reason takes an inferred one.
            int stored = 0;
            for ( const auto& in : w.inferred )
            {
               StepRow s;
               ImageRow im;
               if ( m_store->GetStep( in.first, s ) && m_store->GetImage( s.imageId, im ) && im.journeyId == jid
                 && s.actor == "user" && s.reason.empty() )
               {
                  m_store->SetStepReason( in.first, in.second, true/*inferred*/ );
                  ++stored;
               }
            }
            if ( stored > 0 )
            {
               const nlohmann::json recipe = BuildRecipe( *m_store, jid, kGenerator );
               std::string why;
               if ( !ValidateRecipe( recipe, why ) )
                  throw Error( "the recipe with the inferred reasons does not validate (" + FromU8( why ) + ")" );
               const String re = SafeWriteTextFile( dir + "/recipe.json", recipe.dump( 2 ) + "\n", SafeFileMode::Shared );
               if ( !re.IsEmpty() )
                  throw Error( re );
            }
         }
         catch ( const pcl::Exception& x )
         {
            extra += (extra.IsEmpty() ? "" : "; ") + ("recipe.json was not updated with the inferred reasons: " + x.Message());
         }
         catch ( const std::exception& x )
         {
            extra += (extra.IsEmpty() ? "" : "; ") + ("recipe.json was not updated with the inferred reasons: " + String( x.what() ));
         }
         notes << "PI Copilot: journey.md written to " + dir + (extra.IsEmpty() ? String() : " (" + extra + ")");
      }
   }
   if ( !job.ExportFolder().Trimmed().IsEmpty() )
   {
      KeepOutcome o;
      CopyNow( jid, job.ExportFolder(), o );
      if ( !o.copyError.IsEmpty() )
         notes << "PI Copilot: " + o.copyError;
      else
         notes << "PI Copilot: keeper copied to " + o.copiedTo;
   }
}

void KeeperExporter::Poll( StringList& notes )
{
   // Take the finished jobs out first: Finish() does file and store work.
   std::vector<std::unique_ptr<JourneyWriteupJob>> done;
   for ( auto it = m_jobs.begin(); it != m_jobs.end(); )
      if ( (*it)->Done() )
      {
         done.push_back( std::move( *it ) );
         it = m_jobs.erase( it );
      }
      else
         ++it;
   for ( std::unique_ptr<JourneyWriteupJob>& job : done )
   {
      if ( m_store == nullptr )
      {
         notes << "PI Copilot: journey.md was not written: the journey library was closed while it was being written";
         continue;
      }
      try
      {
         Finish( *job, notes );
      }
      catch ( const pcl::Exception& x )
      {
         notes << "PI Copilot: journey.md could not be finished: " + x.Message();
      }
      catch ( const std::exception& x )
      {
         notes << "PI Copilot: journey.md could not be finished: " + String( x.what() );
      }
   }
}

} // namespace pcl
