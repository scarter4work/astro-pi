// Task 13: the online Bailer-Jones source. ADQL and cache keys against Python (stage.json "tap"),
// the TAP text parsers against answers recorded from the live ESA archive on 2026-11-10
// (test/fixtures/tap), and the async job flow (Review Focus 3) over a scripted transport.
#include <catch_amalgamated.hpp>

#include "Golden.h"
#include "Json.h"
#include "core/Adql.h"
#include "core/OnlineCache.h"
#include "core/TapJob.h"
#include "core/TapText.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using gdg::test::Json;

namespace {

std::string fixture(const std::string& name) {
    const std::string path = std::string(GDG_GOLDEN_DIR) + "/../fixtures/tap/" + name;
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("missing fixture " + path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const Json& tap_cases() {
    static const Json j = Json::parse_file(std::string(GDG_GOLDEN_DIR) + "/synthetic/stage.json");
    return j["tap"]["cases"];
}

std::optional<double> opt(const Json& j) { return j.is_null() ? std::nullopt : std::optional<double>(j.num); }

struct Captured {
    std::vector<std::string> info, warning;
    gdg::Log log() {
        return gdg::Log{[this](const std::string& m) { info.push_back(m); },
                        [this](const std::string& m) { warning.push_back(m); }};
    }
    bool warned(const std::string& prefix) const {
        for (const auto& w : warning)
            if (w.rfind(prefix, 0) == 0) return true;
        return false;
    }
};

// A scripted server: each request pops the next expected (method, url) and its answer.
class FakeTransport : public gdg::HttpTransport {
public:
    struct Step {
        std::string method, url;
        gdg::HttpResponse resp;
    };
    std::deque<Step> script;
    std::vector<std::string> requests;  // "METHOD url"
    std::vector<std::string> forms;
    std::vector<double> sleeps;

    void expect(const std::string& method, const std::string& url, int status, const std::string& ctype,
                const std::string& body) {
        script.push_back(Step{method, url, gdg::HttpResponse{status, ctype, body, status ? "" : "connection refused"}});
    }
    gdg::HttpResponse next(const std::string& method, const std::string& url) {
        requests.push_back(method + " " + url);
        if (script.empty()) throw std::logic_error("unexpected request " + method + " " + url);
        Step s = script.front();
        script.pop_front();
        if (s.method != method || s.url != url)
            throw std::logic_error("expected " + s.method + " " + s.url + ", got " + method + " " + url);
        return s.resp;
    }
    gdg::HttpResponse get(const std::string& url) override { return next("GET", url); }
    gdg::HttpResponse post_form(const std::string& url, const std::string& form) override {
        forms.push_back(form);
        return next("POST", url);
    }
    void sleep_seconds(double s) override { sleeps.push_back(s); }
};

const std::string kJob = "https://gea.esac.esa.int/tap-server/tap/async/fdd4bbdd-dd51-11f1-b71e-bc97e148b76b-O";
const std::string kAsync = "https://gea.esac.esa.int/tap-server/tap/async";
const std::string kNotices = "https://gea.esac.esa.int/tap-server/notification?action=GetNotifications";

// One successful job, as recorded: 303, EXECUTING, COMPLETED, the CSV.
void script_success(FakeTransport& t, const std::string& csv) {
    t.expect("POST", kAsync, 303, "text/plain;charset=UTF-8", fixture("post-303.body"));
    t.expect("GET", kJob + "/phase", 200, "text/plain", fixture("phase-executing.body"));
    t.expect("GET", kJob + "/phase", 200, "text/plain", fixture("phase-completed.body"));
    t.expect("GET", kJob + "/results/result", 200, "text/csv", csv);
}

// A scratch directory under the build tree, removed at scope exit.
struct TempDir {
    std::string path;
    TempDir() {
        char tmpl[] = "gdg-tap-test.XXXXXX";
        const char* d = mkdtemp(tmpl);
        if (!d) throw std::runtime_error("mkdtemp failed");
        path = std::filesystem::absolute(d).string();
    }
    ~TempDir() { std::filesystem::remove_all(path); }
};

const gdg::Footprint kFp{350.1671987228608, 61.22006490843979, 0.02};

}  // namespace

TEST_CASE("py_float_repr_matches_python", "[tap]") {
    CHECK(gdg::py_float_repr(10.0) == "10.0");
    CHECK(gdg::py_float_repr(0.1) == "0.1");
    CHECK(gdg::py_float_repr(-0.0) == "-0.0");
    CHECK(gdg::py_float_repr(0.0001) == "0.0001");
    CHECK(gdg::py_float_repr(1e-05) == "1e-05");
    CHECK(gdg::py_float_repr(-2.5e-07) == "-2.5e-07");
    CHECK(gdg::py_float_repr(1e16) == "1e+16");
    CHECK(gdg::py_float_repr(9999999999999998.0) == "9999999999999998.0");
    CHECK(gdg::py_float_repr(1.2345678901234567e+300) == "1.2345678901234567e+300");
    CHECK(gdg::py_float_repr(0.30000000000000004) == "0.30000000000000004");
    CHECK(gdg::py_float_repr(18.0) == "18.0");
    CHECK_THROWS_AS(gdg::py_float_repr(NAN), std::invalid_argument);
}

TEST_CASE("adql_byte_identical_to_python", "[tap][golden]") {
    size_t n = 0;
    for (const Json& c : tap_cases().arr) {
        if (!c.has("adql")) continue;
        const gdg::Footprint fp{c["footprint"][0].num, c["footprint"][1].num, c["footprint"][2].num};
        const std::string got = gdg::build_adql(fp, static_cast<long>(c["top"].num), opt(c["mag_gt"]), opt(c["mag_lt"]));
        CHECK(got == c["adql"].str);
        ++n;
    }
    CHECK(n == 24);
    // The query the fixtures were recorded with (Python build_adql, TOP 5, G < 18).
    CHECK(gdg::build_adql(kFp, 5, std::nullopt, 18.0) == fixture("query.adql"));
}

TEST_CASE("cache_key_matches_python", "[tap][golden]") {
    size_t n = 0;
    for (const Json& c : tap_cases().arr) {
        if (!c.has("cache_file")) continue;
        const gdg::Footprint fp{c["footprint"][0].num, c["footprint"][1].num, c["footprint"][2].num};
        CHECK("gaia_" + gdg::cache_key(fp, opt(c["cache_mag"])) + ".ecsv" == c["cache_file"].str);
        ++n;
    }
    CHECK(n == 18);
    CHECK(gdg::sha1_hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(gdg::sha1_hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(gdg::sha1_hex(std::string(1000000, 'a')) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

TEST_CASE("tap_location_parsed", "[tap]") {
    // The body of the 303 the archive answered on 2026-11-10.
    CHECK(gdg::parse_uws_location(fixture("post-303.body")) == kJob);
    CHECK(gdg::parse_uws_location("junk\r\nLocation: " + kJob + "\r\n") == kJob);
}

TEST_CASE("tap_location_foreign_host_rejected", "[tap]") {
    for (const std::string url : {"https://evil.example.com/tap-server/tap/async/abc",
                                  "http://gea.esac.esa.int/tap-server/tap/async/abc",
                                  "https://gea.esac.esa.int.evil.com/tap-server/tap/async/abc",
                                  "https://gea.esac.esa.int@evil.com/tap-server/tap/async/abc",
                                  "https://gea.esac.esa.int/tap-server/tap/async/abc/../../../x",
                                  "https://gea.esac.esa.int/tap-server/tap/async/abc?x=1",
                                  "https://gea.esac.esa.int/tap-server/tap/async/",
                                  "https://gea.esac.esa.int/other/abc"})
        CHECK_THROWS_AS(gdg::parse_uws_location("Location: " + url), std::runtime_error);
}

TEST_CASE("tap_location_missing_rejected", "[tap]") {
    CHECK_THROWS_AS(gdg::parse_uws_location(""), std::runtime_error);
    CHECK_THROWS_AS(gdg::parse_uws_location(fixture("result-500-executing.html")), std::runtime_error);
}

TEST_CASE("tap_csv_parses_with_empty_bp_rp", "[tap]") {
    // As recorded: REAL columns print with float precision and parse as float.
    Captured cap;
    const gdg::Catalog c = gdg::parse_tap_csv(fixture("result.csv"), cap.log());
    REQUIRE(c.size() == 5);
    CHECK(c[0].ra == 350.1591429149622);
    CHECK(c[0].dec == 61.236126273547846);
    CHECK(c[0].r_med == static_cast<double>(575.11194f));
    CHECK(c[0].r_lo == static_cast<double>(571.57025f));
    CHECK(c[0].r_hi == static_cast<double>(578.3638f));
    CHECK(c[0].source_id == 2014150992505616640LL);
    CHECK(c[0].bp_rp == static_cast<double>(0.8860359f));
    CHECK(c[4].source_id == 2014156047686426752LL);
    // Empty bp_rp (and r_hi) is NaN; column order, extra columns and CRLF do not matter.
    const std::string csv =
        "source_id,bp_rp,phot_g_mean_mag,r_hi_geo,r_lo_geo,r_med_geo,dec,ra\r\n"
        "17,,12.5,,9.5,10.25,-1.5,2.25\r\n"
        "18,0.75,13.0,30.0,20.0,25.0,1.5,3.5\r\n";
    const gdg::Catalog d = gdg::parse_tap_csv(csv, cap.log());
    REQUIRE(d.size() == 2);
    CHECK(d[0].source_id == 17);
    CHECK(std::isnan(d[0].bp_rp));
    CHECK(std::isnan(d[0].r_hi));
    const gdg::Catalog nulls = gdg::parse_tap_csv("ra,dec,r_med_geo,r_lo_geo,r_hi_geo,source_id,bp_rp\n1,2,3,null,NaN,6,--\n", cap.log());
    CHECK(std::isnan(nulls[0].r_lo));
    CHECK(std::isnan(nulls[0].r_hi));
    CHECK(std::isnan(nulls[0].bp_rp));
    CHECK(d[0].r_med == 10.25);
    CHECK(d[0].ra == 2.25);
    CHECK(d[0].dec == -1.5);
    CHECK(d[1].bp_rp == 0.75);
    // Without a bp_rp column every row is NaN.
    const gdg::Catalog e = gdg::parse_tap_csv("ra,dec,r_med_geo,r_lo_geo,r_hi_geo,source_id\n1,2,3,4,5,6\n", cap.log());
    REQUIRE(e.size() == 1);
    CHECK(std::isnan(e[0].bp_rp));
    CHECK(gdg::csv_row_count(fixture("result.csv")) == 5);
}

TEST_CASE("tap_csv_missing_column_throws", "[tap]") {
    try {
        gdg::parse_tap_csv("ra,dec,r_med_geo,r_hi_geo,bp_rp\n1,2,3,4,5\n", gdg::Log{});
        FAIL("no exception");
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()) == "Gaia result missing columns: ['r_lo_geo', 'source_id']");
    }
    // An HTML error page is not a result.
    CHECK_THROWS_AS(gdg::parse_tap_csv(fixture("result-500-executing.html"), gdg::Log{}), std::runtime_error);
}

TEST_CASE("tap_csv_zero_rows_throws", "[tap]") {
    try {
        gdg::parse_tap_csv("ra,dec,r_med_geo,r_lo_geo,r_hi_geo,source_id,phot_g_mean_mag,bp_rp\n", gdg::Log{});
        FAIL("no exception");
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()) == "Gaia query returned zero rows for this field");
    }
}

TEST_CASE("tap_form_encoding", "[tap]") {
    CHECK(gdg::form_encode("SELECT TOP 5 g.ra FROM t WHERE x < 18.0 AND y = 'ICRS'") ==
          "SELECT%20TOP%205%20g.ra%20FROM%20t%20WHERE%20x%20%3C%2018.0%20AND%20y%20%3D%20%27ICRS%27");
    CHECK(gdg::form_encode("a+b&c=d/e~f_g-h.i") == "a%2Bb%26c%3Dd%2Fe~f_g-h.i");
}

TEST_CASE("tap_flow_success_and_cache", "[tap][flow]") {
    TempDir tmp;
    const gdg::OnlineCache cache(tmp.path);
    FakeTransport t;
    t.expect("GET", kNotices, 404, "text/html", fixture("notices-404.html"));
    script_success(t, fixture("result.csv"));
    Captured cap;
    const gdg::Catalog c = gdg::online_gaia_distances(t, &cache, kFp, 18.0, cap.log());
    CHECK(c.size() == 5);
    CHECK(t.script.empty());
    REQUIRE(t.forms.size() == 1);
    CHECK(t.forms[0] == "REQUEST=doQuery&LANG=ADQL&FORMAT=csv&PHASE=RUN&QUERY=" +
                            gdg::form_encode(gdg::build_adql(kFp, 600000, std::nullopt, 18.0)));
    CHECK(t.sleeps == std::vector<double>{1.0});  // first poll right after the POST
    CHECK(cap.warned("Gaia archive notices unavailable (HTTP 404)"));
    CHECK(cap.warned("Gaia: magnitude cut G<18.0 in effect \xE2\x80\x94 fainter sources excluded by design "
                     "(Bailer-Jones distances unreliable faintward). Set gaia_mag_limit=None to include them."));
    CHECK(std::find(cap.info.begin(), cap.info.end(), "Gaia (async): 5 sources") != cap.info.end());
    // The cache now answers without a single request.
    const std::string path = cache.path_for(gdg::cache_key(kFp, 18.0));
    FakeTransport none;
    Captured cap2;
    const gdg::Catalog again = gdg::online_gaia_distances(none, &cache, kFp, 18.0, cap2.log());
    CHECK(none.requests.empty());
    CHECK(again.size() == 5);
    CHECK(again[3].r_med == c[3].r_med);
    CHECK(cap2.warning == std::vector<std::string>{"using cached Gaia result " + path});
    // A different magnitude limit is a different cache entry.
    CHECK(cache.path_for(gdg::cache_key(kFp, std::nullopt)) != path);
}

TEST_CASE("tap_notices_failure_does_not_stop_the_query", "[tap][flow]") {
    // No answer (status 0) and a transport exception both only skip the notices.
    FakeTransport t;
    t.expect("GET", kNotices, 0, "", "");
    script_success(t, fixture("result.csv"));
    Captured cap;
    CHECK(gdg::online_gaia_distances(t, nullptr, kFp, 18.0, cap.log()).size() == 5);
    CHECK(cap.warned("Gaia archive notices unavailable (HTTP 0)"));

    class Throwing : public FakeTransport {
    public:
        bool thrown = false;
        gdg::HttpResponse get(const std::string& url) override {
            if (!thrown && url == kNotices) {
                thrown = true;
                throw std::runtime_error("TLS handshake failed");
            }
            return FakeTransport::get(url);
        }
    } t2;
    script_success(t2, fixture("result.csv"));
    Captured cap2;
    CHECK(gdg::online_gaia_distances(t2, nullptr, kFp, 18.0, cap2.log()).size() == 5);
    CHECK(cap2.warned("Gaia archive notices unavailable (TLS handshake failed)"));
}

TEST_CASE("tap_no_magnitude_cut_no_warning", "[tap][flow]") {
    FakeTransport t;
    t.expect("GET", kNotices, 200, "text/plain", "");
    script_success(t, fixture("result.csv"));
    Captured cap;
    gdg::online_gaia_distances(t, nullptr, kFp, std::nullopt, cap.log());
    CHECK(cap.warning.empty());
    CHECK(t.forms[0].find("%3C%2018.0") == std::string::npos);
}

TEST_CASE("tap_job_error_retries_then_fails", "[tap][flow]") {
    for (const std::string phase : {"ERROR", "ABORTED"}) {
        FakeTransport t;
        t.expect("GET", kNotices, 404, "text/html", "");
        for (int i = 0; i < 3; ++i) {
            t.expect("POST", kAsync, 303, "text/plain", fixture("post-303.body"));
            t.expect("GET", kJob + "/phase", 200, "text/plain", phase);
        }
        Captured cap;
        try {
            gdg::online_gaia_distances(t, nullptr, kFp, 18.0, cap.log());
            FAIL("no exception");
        } catch (const std::runtime_error& e) {
            CHECK(std::string(e.what()) == "Gaia TAP query failed: Gaia async query failed after 3 attempts: "
                                           "Gaia TAP: job " + kJob + " ended in phase " + phase);
        }
        CHECK(t.script.empty());
        CHECK(cap.warned("Gaia async attempt 1/3 failed: Gaia TAP: job " + kJob + " ended in phase " + phase));
        CHECK(cap.warned("Gaia async attempt 3/3 failed: "));
    }
}

TEST_CASE("tap_post_non_303_and_missing_location", "[tap][flow]") {
    FakeTransport t;
    t.expect("GET", kNotices, 404, "text/html", "");
    t.expect("POST", kAsync, 200, "text/html", "<html>busy</html>");
    t.expect("POST", kAsync, 303, "text/plain", "no location here");
    t.expect("POST", kAsync, 0, "", "");
    Captured cap;
    try {
        gdg::online_gaia_distances(t, nullptr, kFp, 18.0, cap.log());
        FAIL("no exception");
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()).rfind("Gaia TAP query failed: Gaia async query failed after 3 attempts: Gaia TAP: no answer from " +
                                              kAsync,
                                          0) == 0);
    }
    CHECK(cap.warned("Gaia async attempt 1/3 failed: Gaia TAP: job submission answered HTTP 200 (expected 303)"));
    CHECK(cap.warned("Gaia async attempt 2/3 failed: Gaia TAP: the job submission answer has no Location line"));
}

TEST_CASE("tap_foreign_location_never_followed", "[tap][flow]") {
    FakeTransport t;
    t.expect("GET", kNotices, 404, "text/html", "");
    for (int i = 0; i < 3; ++i)
        t.expect("POST", kAsync, 303, "text/plain", "Location: https://evil.example.com/tap-server/tap/async/x");
    CHECK_THROWS_AS(gdg::online_gaia_distances(t, nullptr, kFp, 18.0, gdg::Log{}), std::runtime_error);
    for (const auto& r : t.requests) CHECK(r.find("evil") == std::string::npos);
}

TEST_CASE("tap_recovers_on_a_later_attempt", "[tap][flow]") {
    // The archive's "lost result" 500 (as recorded while the job still ran), then a good job.
    FakeTransport t;
    t.expect("GET", kNotices, 404, "text/html", "");
    t.expect("POST", kAsync, 303, "text/plain", fixture("post-303.body"));
    t.expect("GET", kJob + "/phase", 200, "text/plain", "COMPLETED");
    t.expect("GET", kJob + "/results/result", 500, "text/html;charset=UTF-8", fixture("result-500-executing.html"));
    script_success(t, fixture("result.csv"));
    Captured cap;
    CHECK(gdg::online_gaia_distances(t, nullptr, kFp, 18.0, cap.log()).size() == 5);
    CHECK(cap.warned("Gaia async attempt 1/3 failed: Gaia TAP: result request answered HTTP 500"));
    CHECK(!cap.warned("Gaia async attempt 2/3"));
}

TEST_CASE("tap_result_not_csv_is_retried", "[tap][flow]") {
    FakeTransport t;
    t.expect("GET", kNotices, 404, "text/html", "");
    t.expect("POST", kAsync, 303, "text/plain", fixture("post-303.body"));
    t.expect("GET", kJob + "/phase", 200, "text/plain", "COMPLETED");
    t.expect("GET", kJob + "/results/result", 200, "text/html", "<html>maintenance</html>");
    script_success(t, fixture("result.csv"));
    Captured cap;
    CHECK(gdg::online_gaia_distances(t, nullptr, kFp, 18.0, cap.log()).size() == 5);
    CHECK(cap.warned("Gaia async attempt 1/3 failed: Gaia TAP: the result is not CSV (Content-Type 'text/html')"));
}

TEST_CASE("tap_empty_table_is_an_error_not_success", "[tap][flow]") {
    TempDir tmp;
    const gdg::OnlineCache cache(tmp.path);
    FakeTransport t;
    t.expect("GET", kNotices, 404, "text/html", "");
    script_success(t, "ra,dec,r_med_geo,r_lo_geo,r_hi_geo,source_id,phot_g_mean_mag,bp_rp\n");
    try {
        gdg::online_gaia_distances(t, &cache, kFp, 18.0, gdg::Log{});
        FAIL("no exception");
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()) == "Gaia query returned zero rows for this field");
    }
    CHECK(!cache.read(gdg::cache_key(kFp, 18.0)));  // nothing cached
}

TEST_CASE("tap_row_cap_warning", "[tap][flow]") {
    FakeTransport t;
    t.expect("GET", kNotices, 404, "text/html", "");
    script_success(t, fixture("result.csv"));
    gdg::TapOptions o;
    o.top = 5;
    Captured cap;
    gdg::online_gaia_distances(t, nullptr, kFp, 18.0, cap.log(), o);
    CHECK(cap.warned("Gaia returned the 5-row safety cap; field may be undersampled \xE2\x80\x94 lower gaia_mag_limit."));
    CHECK(t.forms[0].find("TOP%205%20") != std::string::npos);
}

TEST_CASE("tap_poll_timeout", "[tap][flow]") {
    FakeTransport t;
    t.expect("POST", kAsync, 303, "text/plain", fixture("post-303.body"));
    for (int i = 0; i < 8; ++i) t.expect("GET", kJob + "/phase", 200, "text/plain", "EXECUTING");
    gdg::TapOptions o;
    o.max_wait_seconds = 40;  // 1 + 2 + 4 + 8 + 8 + 8 + 8 = 39, the next 8 would pass 40
    try {
        gdg::run_async_job(t, "SELECT 1", o);
        FAIL("no exception");
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()) == "Gaia TAP: job " + kJob + " did not complete within 40 s");
    }
    CHECK(t.sleeps == std::vector<double>{1.0, 2.0, 4.0, 8.0, 8.0, 8.0, 8.0});
}

TEST_CASE("online_cache_private_dir", "[tap][cache]") {
    TempDir tmp;
    const unsigned uid = geteuid();
    const std::string d = tmp.path + "/a";
    gdg::ensure_private_dir(d, uid);
    struct stat st;
    REQUIRE(lstat(d.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0700);
    chmod(d.c_str(), 0755);  // repaired
    gdg::ensure_private_dir(d, uid);
    REQUIRE(lstat(d.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0700);
    // A symbolic link, a plain file, or another owner is refused.
    const std::string link = tmp.path + "/link";
    REQUIRE(symlink(d.c_str(), link.c_str()) == 0);
    CHECK_THROWS_WITH(gdg::ensure_private_dir(link, uid), Catch::Matchers::ContainsSubstring("symbolic link"));
    const std::string file = tmp.path + "/file";
    std::ofstream(file) << "x";
    CHECK_THROWS_WITH(gdg::ensure_private_dir(file, uid), Catch::Matchers::ContainsSubstring("not a directory"));
    CHECK_THROWS_WITH(gdg::ensure_private_dir(d, uid + 1), Catch::Matchers::ContainsSubstring("owned by user id"));

    const gdg::OnlineCache cache(d);
    CHECK(!cache.read("k"));
    cache.write("k", "ra,dec\n1,2\n");
    CHECK(cache.read("k") == std::optional<std::string>("ra,dec\n1,2\n"));
    REQUIRE(lstat(cache.path_for("k").c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
    CHECK(cache.path_for("k") == d + "/gaia_k.csv");
    // A planted link in place of an entry is refused, not followed.
    REQUIRE(symlink(file.c_str(), cache.path_for("p").c_str()) == 0);
    CHECK_THROWS_AS(cache.read("p"), std::runtime_error);
}

TEST_CASE("online_cache_default_location", "[tap][cache]") {
    TempDir tmp;
    const char* old = std::getenv("XDG_CACHE_HOME");
    const std::string saved = old ? old : "";
    setenv("XDG_CACHE_HOME", tmp.path.c_str(), 1);
    CHECK(gdg::default_cache_dir() == tmp.path + "/GaiaDepthGrade/gaia");
    const gdg::OnlineCache cache;  // creates and verifies the default directory
    CHECK(cache.dir() == tmp.path + "/GaiaDepthGrade/gaia");
    struct stat st;
    REQUIRE(lstat((tmp.path + "/GaiaDepthGrade").c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0700);
    setenv("XDG_CACHE_HOME", "relative/path", 1);  // not absolute: $HOME/.cache
    CHECK(gdg::default_cache_dir() == std::string(std::getenv("HOME")) + "/.cache/GaiaDepthGrade/gaia");
    if (old)
        setenv("XDG_CACHE_HOME", saved.c_str(), 1);
    else
        unsetenv("XDG_CACHE_HOME");
}
