#include "TapClient.h"

#include "GaiaDepthGradeVersion.h"
#include "core/OnlineCache.h"
#include "core/TapText.h"

#include <pcl/Control.h>
#include <pcl/ErrorHandler.h>
#include <pcl/NetworkTransfer.h>
#include <pcl/Thread.h>

#include <cmath>
#include <stdexcept>

namespace pcl
{

namespace
{

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

gdg::HttpResponse TapClient::Transfer( const std::string& url, const std::string* form )
{
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
   if ( seconds > 0 )
      pcl::Sleep( unsigned( std::lround( seconds*1000 ) ) );
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
