#include "QEFetcher.h"

#include <pcl/Exception.h>
#include <pcl/String.h>

namespace pcl
{

bool QEFetcher::Sink::OnData( NetworkTransfer& /*sender*/,
                              const void* buffer, fsize_type size )
{
   if ( size <= 0 )
      return true;

   if ( data.Length() + size_type( size ) > QEFetcher::kMaxBytes )
   {
      // Returning false aborts the transfer. Better to stop mid-stream than
      // to buffer an unbounded response from a wrong URL or a hostile host.
      overflow = true;
      return false;
   }

   data.Append( static_cast<const char*>( buffer ), size_type( size ) );
   return true;
}

nukex::FetchResult QEFetcher::get( const std::string& url )
{
   m_sink.Reset();

   try
   {
      NetworkTransfer transfer;
      transfer.SetURL( String( url.c_str() ) );

      // useSSL, forceSSL, verifyPeer, verifyHost -- all on. forceSSL means a
      // plain-HTTP URL or a downgrade is refused rather than silently
      // fetched, and peer/host verification is what makes TLS worth having.
      // TLS still authenticates only the HOST; the payload's own Ed25519
      // signature is what makes the data itself trustworthy (see QEUpdater).
      transfer.SetSSL( true/*useSSL*/, true/*forceSSL*/,
                       true/*verifyPeer*/, true/*verifyHost*/ );
      transfer.SetConnectionTimeout( kTimeoutSeconds );
      transfer.OnDownloadDataAvailable(
            static_cast<NetworkTransfer::download_event_handler>( &Sink::OnData ),
            m_sink );

      nukex::TransferOutcome t;
      t.performed_ok  = transfer.Download();
      t.aborted       = transfer.WasAborted();
      t.overflow      = m_sink.overflow;
      t.response_code = transfer.ResponseCode();
      t.body.assign( m_sink.data.c_str(), m_sink.data.Length() );
      t.error = IsoString( transfer.ErrorInformation() ).c_str();

      // Only a 2xx response is a fetched body; a 404 page for a missing
      // .sig must come back as OFFLINE, not reach signature verification.
      return nukex::fetch_result_from_transfer( t );
   }
   catch ( const Exception& x )
   {
      // PCL throws on some transport failures. Being offline must never
      // propagate an exception into the interface or ExecuteGlobal.
      return { false, std::string(), std::string( IsoString( x.Message() ).c_str() ) };
   }
   catch ( ... )
   {
      return { false, std::string(), "unknown network error" };
   }
}

} // namespace pcl
