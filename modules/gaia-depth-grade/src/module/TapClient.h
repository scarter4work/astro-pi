// TapClient: the HTTP transport of the online Bailer-Jones source, over pcl::NetworkTransfer
// (plan Task 13). HTTPS only, peer and host certificates verified, and only URLs on
// https://gea.esac.esa.int/ (the one host the module talks to; CPD 6.1). NetworkTransfer does
// not follow redirects: the TAP flow reads the job URL from the 303 body (core/TapJob).
#ifndef __GDG_TapClient_h
#define __GDG_TapClient_h

#include "core/TapJob.h"

#include <optional>

namespace pcl
{

/// Records the calling thread as PixInsight's main thread, once (later calls change nothing).
/// InstallPixInsightModule calls it on a full install (the core loads modules on its main
/// thread). pcl::Thread::IsRootThread() alone cannot be the test: it knows only pcl::Thread
/// objects and answers true on any std::thread (measured, probe tap_off_main_thread, 2026-11-13).
void RecordMainThread();

/// True on the thread RecordMainThread() recorded (false before it ran).
bool OnMainThread();

class TapClient : public gdg::HttpTransport
{
public:

   gdg::HttpResponse get( const std::string& url ) override;
   gdg::HttpResponse post_form( const std::string& url, const std::string& form ) override;
   void sleep_seconds( double seconds ) override;

private:

   gdg::HttpResponse Transfer( const std::string& url, const std::string* form );
};

/// The online distance source: core online_gaia_distances over a TapClient, with the private
/// cache in `cacheDir` (empty: $XDG_CACHE_HOME or ~/.cache, then GaiaDepthGrade/gaia). The cache
/// directory is created on first use, never in offline mode.
class OnlineGaiaSource : public gdg::CatalogSource
{
public:

   OnlineGaiaSource( std::optional<double> magLimit, const gdg::Log& log, const std::string& cacheDir = std::string() );

   gdg::Catalog distances_for( const gdg::Footprint& footprint ) override;

private:

   std::optional<double> m_magLimit;
   gdg::Log              m_log;
   std::string           m_cacheDir;
};

} // namespace pcl

#endif // __GDG_TapClient_h
