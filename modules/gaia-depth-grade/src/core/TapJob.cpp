// The online Bailer-Jones source's network flow (port of distances.GaiaStarSource._run_query /
// distances_for and astroquery's launch_job_async), over an abstract transport.
#include "TapJob.h"

#include "Adql.h"
#include "Format.h"
#include "TapText.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <stdexcept>

namespace gdg {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

HttpResponse checked(HttpResponse r, const std::string& url) {
    if (r.status == 0) throw std::runtime_error("Gaia TAP: no answer from " + url + ": " + r.error);
    return r;
}

}  // namespace

std::string form_encode(const std::string& value) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string run_async_job(HttpTransport& http, const std::string& adql, const TapOptions& opt) {
    const std::string form = "REQUEST=doQuery&LANG=ADQL&FORMAT=csv&PHASE=RUN&QUERY=" + form_encode(adql);
    const HttpResponse sub = checked(http.post_form(kGaiaTapAsync, form), kGaiaTapAsync);
    // PCL's NetworkTransfer does not follow redirects; the archive puts the job URL in the body.
    if (sub.status != 303)
        throw std::runtime_error(strformat("Gaia TAP: job submission answered HTTP %d (expected 303)", sub.status));
    const std::string job = parse_uws_location(sub.body);

    double waited = 0.0, delay = 1.0;
    for (;;) {
        const HttpResponse ph = checked(http.get(job + "/phase"), job + "/phase");
        if (ph.status != 200) throw std::runtime_error(strformat("Gaia TAP: phase request answered HTTP %d", ph.status));
        const std::string phase = trim(ph.body);
        if (phase == "COMPLETED") break;
        if (phase == "ERROR" || phase == "ABORTED" || phase == "ARCHIVED")
            throw std::runtime_error("Gaia TAP: job " + job + " ended in phase " + phase);
        // PENDING, QUEUED, EXECUTING, HELD, SUSPENDED, UNKNOWN: wait 1, 2, 4, 8, 8 ... seconds.
        if (waited + delay > opt.max_wait_seconds)
            throw std::runtime_error(
                strformat("Gaia TAP: job %s did not complete within %.0f s", job.c_str(), opt.max_wait_seconds));
        http.sleep_seconds(delay);
        waited += delay;
        delay = std::min(delay * 2.0, 8.0);
    }

    const std::string url = job + "/results/result";
    const HttpResponse res = checked(http.get(url), url);
    if (res.status != 200) throw std::runtime_error(strformat("Gaia TAP: result request answered HTTP %d", res.status));
    if (res.content_type.rfind("text/csv", 0) != 0)
        throw std::runtime_error("Gaia TAP: the result is not CSV (Content-Type '" + res.content_type + "')");
    return res.body;
}

void log_gaia_notices(HttpTransport& http, const Log& log) {
    HttpResponse r;
    try {
        r = http.get(kGaiaNotices);
    } catch (const TransportMisuse&) {
        throw;  // a programming error, never "notices unavailable"
    } catch (const std::exception& e) {  // only the notices are skipped; the query still runs
        log_warning(log, std::string("Gaia archive notices unavailable (") + e.what() + ")");
        return;
    }
    if (r.status != 200) {
        log_warning(log, strformat("Gaia archive notices unavailable (HTTP %d)", r.status));
        return;
    }
    const std::string text = trim(r.body);
    if (!text.empty()) log_info(log, "Gaia archive notices: " + text);
}

namespace {

/// What differs between the archive's two queries (Bailer-Jones distances, solver positions).
struct TapQuery {
    std::string key;          // the cache key
    std::string adql;
    std::string cap_warning;  // logged when the result reaches the row cap
    std::string count_label;  // "<label>: %ld sources"
};

/// The one cache + retry flow (GaiaStarSource.distances_for): the cache (warning `using cached Gaia
/// result <path>`), else the notices, `after_notices`, up to opt.attempts async jobs (each failure
/// warns `Gaia async attempt %d/%d failed: %s`; then std::runtime_error `Gaia TAP query failed:
/// Gaia async query failed after %d attempts: <last>`), the row-cap warning and count, `parse`
/// (which refuses missing columns and zero rows: an error is never cached), and the cache write.
template <class Parse>
auto cached_tap_query(HttpTransport& http, const OnlineCache* cache, const TapQuery& q, const Log& log,
                      const TapOptions& opt, const std::function<void()>& after_notices, Parse parse)
    -> decltype(parse(std::string())) {
    if (cache) {
        if (const std::optional<std::string> hit = cache->read(q.key)) {
            log_warning(log, "using cached Gaia result " + cache->path_for(q.key));
            return parse(*hit);
        }
    }

    log_gaia_notices(http, log);
    if (after_notices) after_notices();
    std::string csv, last;
    bool ok = false;
    for (int attempt = 1; attempt <= opt.attempts; ++attempt) {
        try {
            csv = run_async_job(http, q.adql, opt);
            const long rows = csv_row_count(csv);
            if (rows >= opt.top) log_warning(log, q.cap_warning);
            log_info(log, strformat("%s: %ld sources", q.count_label.c_str(), rows));
            ok = true;
            break;
        } catch (const TransportMisuse&) {
            throw;  // a programming error: no retry, no wrapping
        } catch (const std::exception& e) {  // retry, then surface the real error
            last = e.what();
            log_warning(log, strformat("Gaia async attempt %d/%d failed: %s", attempt, opt.attempts, last.c_str()));
        }
    }
    if (!ok)
        throw std::runtime_error(strformat("Gaia TAP query failed: Gaia async query failed after %d attempts: %s",
                                           opt.attempts, last.c_str()));
    auto result = parse(csv);  // missing columns / zero rows are errors, never success
    if (cache) cache->write(q.key, csv);
    return result;
}

}  // namespace

Catalog online_gaia_distances(HttpTransport& http, const OnlineCache* cache, const Footprint& fp,
                              std::optional<double> mag_limit, const Log& log, const TapOptions& opt) {
    const TapQuery q{cache_key(fp, mag_limit), build_adql(fp, opt.top, std::nullopt, mag_limit),
                     strformat("Gaia returned the %ld-row safety cap; field may be undersampled "
                               "\xE2\x80\x94 lower gaia_mag_limit.",
                               opt.top),
                     "Gaia (async)"};
    // LOUD about the cut (no silent cap): the limit in effect, stated up front.
    const auto cut_warning = [&] {
        if (mag_limit)
            log_warning(log, strformat("Gaia: magnitude cut G<%.1f in effect \xE2\x80\x94 fainter sources excluded by "
                                       "design (Bailer-Jones distances unreliable faintward). Set gaia_mag_limit=None "
                                       "to include them.",
                                       *mag_limit));
    };
    return cached_tap_query(http, cache, q, log, opt, cut_warning,
                            [&](const std::string& csv) { return parse_tap_csv(csv, log); });
}

std::string positions_cache_key(double ra, double dec, double radius, double mag_high) {
    return "pos" + sha1_hex(strformat("pos_%.6f_%.6f_%.6f_mag%.2f", ra, dec, radius, mag_high)).substr(0, 16);
}

std::vector<GaiaPosition> online_gaia_positions(HttpTransport& http, const OnlineCache* cache, double ra, double dec,
                                                double radius, double mag_high, const Log& log,
                                                const TapOptions& opt) {
    const TapQuery q{positions_cache_key(ra, dec, radius, mag_high),
                     build_positions_adql(ra, dec, radius, mag_high, opt.top),
                     strformat("Gaia returned the %ld-row safety cap; the plate solver's catalogue may be incomplete.",
                               opt.top),
                     "Gaia DR3 positions (async)"};
    return cached_tap_query(http, cache, q, log, opt, nullptr,
                            [](const std::string& csv) { return parse_positions_csv(csv); });
}

}  // namespace gdg
