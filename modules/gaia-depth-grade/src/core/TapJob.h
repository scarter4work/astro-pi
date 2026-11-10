// The online Bailer-Jones source's network flow (port of distances.GaiaStarSource with
// astroquery's async job), over an abstract HTTP transport so the flow is unit-tested with
// recorded answers. The module's TapClient implements the transport with pcl::NetworkTransfer
// (HTTPS only, peer and host verified, no redirects followed).
#pragma once

#include "Catalog.h"
#include "Footprint.h"
#include "Log.h"
#include "OnlineCache.h"

#include <optional>
#include <string>

namespace gdg {

struct HttpResponse {
    int status = 0;            // HTTP status; 0 when no answer arrived
    std::string content_type;  // as sent by the server
    std::string body;
    std::string error;         // transport error text when status == 0
};

class HttpTransport {
public:
    virtual ~HttpTransport() = default;
    virtual HttpResponse get(const std::string& url) = 0;
    /// POST of an application/x-www-form-urlencoded body.
    virtual HttpResponse post_form(const std::string& url, const std::string& form) = 0;
    virtual void sleep_seconds(double seconds) = 0;
};

struct TapOptions {
    long top = 600000;              // row safety cap (GaiaStarSource._TOP)
    int attempts = 3;               // GaiaStarSource._RETRIES
    double max_wait_seconds = 1200; // the job must complete within 20 minutes
};

/// application/x-www-form-urlencoded value (RFC 3986 unreserved characters kept, the rest %XX).
std::string form_encode(const std::string& value);

/// One async job: POST (expects 303 + Location in the body), poll <job>/phase (1, 2, 4, 8, 8 ...
/// seconds) until COMPLETED, GET <job>/results/result (expects 200 text/csv). Throws
/// std::runtime_error with the reason on any other answer, on ERROR / ABORTED, or on timeout.
std::string run_async_job(HttpTransport& http, const std::string& adql, const TapOptions& opt = {});

/// GET the archive's status notices: any answer other than 200 logs the warning `Gaia archive
/// notices unavailable (HTTP %d)` and carries on; 200 logs the text.
void log_gaia_notices(HttpTransport& http, const Log& log);

/// GaiaStarSource.distances_for: the cache (warning `using cached Gaia result <path>`), else the
/// notices, the magnitude-cut warning, up to `attempts` async jobs (each failure warns
/// `Gaia async attempt %d/%d failed: %s`; then std::runtime_error `Gaia TAP query failed: Gaia
/// async query failed after %d attempts: <last>`), the row-cap warning, parse_tap_csv, and
/// the cache write. `cache` may be null (no cache).
Catalog online_gaia_distances(HttpTransport& http, const OnlineCache* cache, const Footprint& fp,
                              std::optional<double> mag_limit, const Log& log, const TapOptions& opt = {});

}  // namespace gdg
