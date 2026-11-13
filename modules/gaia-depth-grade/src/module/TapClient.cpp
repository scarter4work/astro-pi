#include "TapClient.h"

#include "GaiaDepthGradeVersion.h"
#include "core/OnlineCache.h"
#include "core/TapText.h"

#include <pcl/Control.h>
#include <pcl/ErrorHandler.h>
#include <pcl/MetaModule.h>
#include <pcl/NetworkTransfer.h>
#include <pcl/Thread.h>

#include <atomic>
#include <cmath>
#include <stdexcept>
#include <thread>

#ifdef GDG_PROBES
#include <cstdlib>
#include <fstream>
#endif

namespace pcl
{

namespace
{

std::atomic<bool>            s_mainRecorded{ false };
std::thread::id              s_mainThread;

// The download event handler must be a Control member (NetworkTransfer::download_event_handler).
class TapReceiver : public Control
{
public:

   std::string data;

   bool OnData( NetworkTransfer&, const void* buffer, fsize_type size )
   {
      data.append( static_cast<const char*>( buffer ), size_t( size ) );
      return true;
   }
};

} // namespace

void RecordMainThread()
{
   s_mainThread = std::this_thread::get_id();
   s_mainRecorded.store( true );
}

bool OnMainThread()
{
   return s_mainRecorded.load() && std::this_thread::get_id() == s_mainThread && Thread::IsRootThread();
}

gdg::HttpResponse TapClient::Transfer( const std::string& url, const std::string* form )
{
   // Plan C14: the transfer's event receiver is a Control, a GUI-thread object, so the transfer runs
   // on PixInsight's main thread only (RunPrepare fetches the catalogue there, never on the
   // interface's worker thread). A call from another thread is a programming error, refused before
   // anything is attempted (gdg::TransportMisuse: the TAP flow never retries or downgrades it).
   if ( !OnMainThread() )
      throw gdg::TransportMisuse( "Gaia TAP: the online catalogue must be fetched on PixInsight's main thread" );
#ifdef GDG_PROBES
   // Test builds only: GDG_TEST_NET_LOG=<file> records every URL this transport is asked to open
   // (probes no_network / online_fallback_live); release builds never read it.
   if ( const char* netLog = std::getenv( "GDG_TEST_NET_LOG" ) )
      if ( *netLog != '\0' )
      {
         std::ofstream f( netLog, std::ios::binary | std::ios::app );
         f << url << '\n';
      }
#endif
   // Only the Gaia archive, only over HTTPS (the core never builds another URL; this is the
   // transport's own guard).
   static const std::string allowed( gdg::kGaiaArchive );
   if ( url.compare( 0, allowed.size(), allowed ) != 0 )
      throw std::runtime_error( "Gaia TAP: refusing to connect to " + url + " (only " + allowed + " is allowed)" );

   gdg::HttpResponse r;
   try
   {
      TapReceiver receiver;
      NetworkTransfer transfer;
      transfer.SetURL( String::UTF8ToUTF16( url.c_str() ) );
      transfer.SetSSL( true/*useSSL*/, true/*forceSSL*/, true/*verifyPeer*/, true/*verifyHost*/ );
      transfer.SetConnectionTimeout( 60 );
      transfer.SetCustomHTTPHeaders( "User-Agent: GaiaDepthGrade/" GDG_VERSION_STRING " (PixInsight module)" );
      transfer.OnDownloadDataAvailable( (NetworkTransfer::download_event_handler)&TapReceiver::OnData, receiver );
      const bool ok = form ? transfer.POST( String::UTF8ToUTF16( form->c_str() ) ) : transfer.Download();

      r.status = transfer.ResponseCode();
      r.content_type = transfer.ContentType().ToUTF8().c_str();
      r.body = std::move( receiver.data );
      if ( r.status <= 0 )
      {
         r.status = 0;
         r.error = ok ? "no HTTP status" : transfer.ErrorInformation().ToUTF8().c_str();
      }
   }
   catch ( const Exception& e )
   {
      // A PCL error (setup, TLS, ...) is a failed transfer: the TAP flow retries and reports it.
      r = gdg::HttpResponse{};
      r.error = e.Message().ToUTF8().c_str();
   }
   return r;
}

gdg::HttpResponse TapClient::get( const std::string& url )
{
   return Transfer( url, nullptr );
}

gdg::HttpResponse TapClient::post_form( const std::string& url, const std::string& form )
{
   return Transfer( url, &form );
}

void TapClient::sleep_seconds( double seconds )
{
   // The poll interval (1, 2, 4, 8 ... s, C13). On the main thread (always: C14) the GUI keeps
   // painting meanwhile; user input stays held back, as during the rest of a Prepare.
   unsigned left = seconds > 0 ? unsigned( std::lround( seconds*1000 ) ) : 0;
   while ( left > 0 )
   {
      const unsigned slice = left < 50 ? left : 50;
      pcl::Sleep( slice );
      left -= slice;
      if ( OnMainThread() )
         Module->ProcessEvents( true/*excludeUserInputEvents*/ );
   }
}

OnlineGaiaSource::OnlineGaiaSource( std::optional<double> magLimit, const gdg::Log& log, const std::string& cacheDir )
   : m_magLimit( magLimit )
   , m_log( log )
   , m_cacheDir( cacheDir )
{
}

gdg::Catalog OnlineGaiaSource::distances_for( const gdg::Footprint& fp )
{
   const gdg::OnlineCache cache( m_cacheDir );
   TapClient http;
   return gdg::online_gaia_distances( http, &cache, fp, m_magLimit, m_log );
}

} // namespace pcl
