#include "nukex/stacker/calibration_masters.hpp"
#include "nukex/stacker/light_calibration.hpp"
#include "nukex/stacker/frame_cache.hpp"
#include "nukex/core/cancel_poll.hpp"
#include "nukex/core/parallel_for.hpp"
#include "nukex/io/fits_reader.hpp"
#include "nukex/io/fits_writer.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

namespace nukex {

namespace {

// 2: a master flat records its full pedestal, pedestal plan and pedestal
//    signature (HISTORY "NKXP... " records), so it can stand in for the flat
//    frames in StackingEngine::execute. A version-1 file is rebuilt.
constexpr int kMasterFormatVersion = 2;

// A string too long for one card, as HISTORY records "<tag> <up to 60 chars>|".
// The '|' keeps trailing spaces, which the HISTORY reader trims.
constexpr std::size_t kHistoryChunk = 60;

void push_long(std::vector<std::string>& history, const char* tag, const std::string& s) {
    for (std::size_t i = 0; i < s.size(); i += kHistoryChunk)
        history.push_back(std::string(tag) + " " + s.substr(i, kHistoryChunk) + "|");
}

std::string read_long(const std::vector<std::string>& history, const char* tag) {
    const std::string prefix = std::string(tag) + " ";
    std::string out;
    for (const auto& line : history)
        if (line.rfind(prefix, 0) == 0 && line.size() > prefix.size() && line.back() == '|')
            out += line.substr(prefix.size(), line.size() - prefix.size() - 1);
    return out;
}

std::string fmt(double v, int prec = 1) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", prec, v);
    return b;
}

std::string fmt_g(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%g", v);
    return b;
}

std::string base_name(const std::string& p) {
    return std::filesystem::path(p).filename().string();
}

std::string normalized_type(std::string s) {
    std::string out;
    for (char c : s) {
        if (c == ' ' || c == '_' || c == '-') continue;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

/// Does a frame's IMAGETYP fit the kind of master it was given for? An
/// absent IMAGETYP cannot be checked and is accepted.
bool image_type_fits(MasterKind kind, const std::string& image_type) {
    const std::string t = normalized_type(image_type);
    if (t.empty()) return true;
    auto any = [&](std::initializer_list<const char*> names) {
        for (const char* n : names) if (t == n) return true;
        return false;
    };
    switch (kind) {
        case MasterKind::BIAS:      return any({"bias", "biasframe", "offset", "offsetframe"});
        case MasterKind::DARK:      return any({"dark", "darkframe"});
        case MasterKind::DARK_FLAT: return any({"dark", "darkframe", "darkflat", "flatdark", "darkflatframe"});
        case MasterKind::FLAT:      return any({"flat", "flatframe", "flatfield"});
    }
    return false;
}

const char* master_image_type(MasterKind kind) {
    switch (kind) {
        case MasterKind::BIAS:      return "Master Bias";
        case MasterKind::DARK:      return "Master Dark";
        case MasterKind::DARK_FLAT: return "Master Dark Flat";
        case MasterKind::FLAT:      return "Master Flat";
    }
    return "Master";
}

bool kind_from_name(const std::string& n, MasterKind& out) {
    for (MasterKind k : {MasterKind::BIAS, MasterKind::DARK, MasterKind::DARK_FLAT, MasterKind::FLAT})
        if (n == master_kind_name(k)) { out = k; return true; }
    return false;
}

/// FNV-1a, 64 bit.
struct Fnv {
    std::uint64_t h = 1469598103934665603ULL;
    void add(const void* p, std::size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
    }
    void add(const std::string& s) { add(s.data(), s.size()); add("\0", 1); }
    template <typename T> void add_value(T v) { add(&v, sizeof v); }
    std::string hex() const {
        char b[17];
        std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h));
        return b;
    }
};

bool parse_double(const std::string& s, double& out) {
    std::string t = s;
    for (char& c : t) if (c == 'D' || c == 'd') c = 'E';
    const char* b = t.data();
    const char* e = b + t.size();
    while (b < e && *b == ' ') ++b;
    if (b < e && *b == '+') ++b;
    auto r = std::from_chars(b, e, out);
    return r.ec == std::errc() && r.ptr != b;
}

std::string estimator_name(MasterBuilder::Config::Estimator e) {
    return e == MasterBuilder::Config::Estimator::HUBER ? "Huber" : "model race";
}

std::string utc_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S", &tm);
    return b;
}

/// Huber location of a whole image's values (all channels) -- the level a
/// flat is scaled by. Same estimator, same seed and scale as a pixel's fit.
float image_location(const float* data, std::size_t n, const HuberEstimator::Config& c) {
    std::vector<float> scratch(n);
    return HuberEstimator::location(data, nullptr, static_cast<int>(n), c, scratch.data());
}

} // namespace

const char* master_kind_name(MasterKind kind) {
    switch (kind) {
        case MasterKind::BIAS:      return "bias";
        case MasterKind::DARK:      return "dark";
        case MasterKind::DARK_FLAT: return "dark-flat";
        case MasterKind::FLAT:      return "flat";
    }
    return "?";
}

CalibrationHeader CalibrationHeader::from_metadata(const FrameMetadata& m, int channels,
                                                   double full_scale) {
    CalibrationHeader h;
    h.image_type    = m.image_type;
    h.instrument    = m.instrument;
    h.bayer_pattern = m.bayer_pattern;
    h.width = m.width; h.height = m.height; h.channels = channels;
    h.xbin = m.xbinning; h.ybin = m.ybinning;
    h.has_exposure = m.has_exposure; h.exposure = m.exposure;
    if (m.has_ccd_temp) {
        h.has_temperature = true; h.temperature = m.ccd_temp;
    } else if (m.has_set_temp) {
        h.has_temperature = true; h.temperature = m.set_temp; h.temperature_is_setpoint = true;
    }
    h.has_gain = m.has_gain_setting; h.gain = m.gain_setting;
    h.has_offset = m.has_offset;     h.offset = m.offset;
    h.full_scale = full_scale;
    return h;
}

std::string CalibrationHeader::describe() const {
    std::ostringstream o;
    o << (instrument.empty() ? "(no INSTRUME)" : instrument) << " " << width << "x" << height
      << " bin " << xbin << "x" << ybin;
    if (!bayer_pattern.empty()) o << " " << bayer_pattern;
    o << ", " << (has_exposure ? fmt_g(exposure) + " s" : "no exposure");
    o << ", gain " << (has_gain ? fmt_g(gain) : "?");
    o << ", offset " << (has_offset ? fmt_g(offset) : "?");
    o << ", " << (has_temperature ? fmt(temperature) + " C" + (temperature_is_setpoint ? " (set point)" : "")
                                  : "no temperature");
    return o.str();
}

// ── Signature ───────────────────────────────────────────────────────

std::string MasterBuilder::signature(MasterKind kind, const std::vector<std::string>& paths_in,
                                     const Config& config,
                                     const std::string& pedestal_signature) {
    std::vector<std::string> paths = paths_in;
    std::sort(paths.begin(), paths.end());
    Fnv f;
    f.add("nukex-master");
    f.add_value(kMasterFormatVersion);
    f.add(master_kind_name(kind));
    f.add(estimator_name(config.estimator));
    if (config.estimator == Config::Estimator::HUBER) {
        f.add_value(config.huber.tuning);
        f.add_value(config.huber.iterations);
        f.add_value(config.huber.min_samples);
    } else {
        f.add_value(config.race.aicc_threshold);
        f.add_value(config.race.min_samples_for_gmm);
        f.add_value(config.race.min_samples_for_fit);
    }
    for (const auto& p : paths) {
        std::error_code ec;
        const auto abs = std::filesystem::absolute(p, ec).lexically_normal().string();
        f.add(ec ? p : abs);
        const auto size = std::filesystem::file_size(p, ec);
        f.add_value(static_cast<std::uint64_t>(ec ? 0 : size));
        const auto mtime = std::filesystem::last_write_time(p, ec);
        f.add_value(static_cast<std::int64_t>(ec ? 0 : mtime.time_since_epoch().count()));
    }
    f.add(pedestal_signature);
    return f.hex();
}

// ── Build ───────────────────────────────────────────────────────────

MasterBuilder::Result MasterBuilder::build(MasterKind kind, const std::vector<std::string>& paths_in,
                                           const Config& config, const Image* pedestal,
                                           const std::string& pedestal_label,
                                           const std::string& pedestal_signature,
                                           ProgressObserver* progress) {
    ProgressObserver& obs = progress ? *progress : null_progress_observer();
    CancelPoll cancel(obs);
    Result res;
    const std::string what = std::string("master ") + master_kind_name(kind);

    const int n = static_cast<int>(paths_in.size());
    if (n < std::max(config.min_frames, 1)) {
        res.error = "Cannot build a " + what + " from " + std::to_string(n) + " frame"
                  + (n == 1 ? "" : "s") + ": fitting each pixel's distribution needs at least "
                  + std::to_string(std::max(config.min_frames, 1)) + ".";
        return res;
    }
    if (kind == MasterKind::FLAT && pedestal == nullptr) {
        res.error = "Cannot build a master flat for calibrated lights without a pedestal "
                    "(master dark-flat or master bias): the flats' bias would stay in the "
                    "flat and the division would no longer be a pure gain correction.";
        return res;
    }

    // Canonical order: the result does not depend on how the caller listed
    // the files (the fit sums in frame order, so order is bits).
    std::vector<std::string> paths = paths_in;
    std::sort(paths.begin(), paths.end());

    // ── 1. Headers: one set, one camera, one configuration ──────────
    std::vector<CalibrationHeader> hdr(static_cast<std::size_t>(n));
    std::vector<std::string> problems;
    for (int i = 0; i < n; ++i) {
        const FrameMetadata m = FITSReader::read_headers(paths[i]);
        if (m.width <= 0 || m.height <= 0) {
            problems.push_back(base_name(paths[i]) + ": cannot be read as a FITS image");
            continue;
        }
        hdr[i] = CalibrationHeader::from_metadata(m, 1,
                     FITSReader::full_scale_for_bitpix(m.equiv_bitpix));
        if (!image_type_fits(kind, m.image_type))
            problems.push_back(base_name(paths[i]) + ": IMAGETYP='" + m.image_type
                               + "' but it was given as a " + master_kind_name(kind) + " frame");
    }
    if (problems.empty()) {
        const CalibrationHeader& h0 = hdr[0];
        double tmin = 1e300, tmax = -1e300;
        int n_temp = 0;
        for (int i = 0; i < n; ++i) {
            const CalibrationHeader& h = hdr[i];
            const std::string c = CalibrationMatcher::compatibility(h, h0, nullptr);
            if (!c.empty())
                problems.push_back(base_name(paths[i]) + " differs from " + base_name(paths[0]) + ": " + c);
            if (kind != MasterKind::BIAS) {
                if (!h.has_exposure || !h0.has_exposure) {
                    if (h.has_exposure != h0.has_exposure)
                        problems.push_back(base_name(paths[i]) + ": exposure keyword present in only some frames");
                } else if (std::fabs(h.exposure - h0.exposure) > config.exposure_rel_tolerance * std::max(h0.exposure, 1e-9)) {
                    problems.push_back(base_name(paths[i]) + ": exposure " + fmt_g(h.exposure) + " s, but "
                                       + base_name(paths[0]) + " is " + fmt_g(h0.exposure) + " s");
                }
            }
            if (h.has_temperature) { ++n_temp; tmin = std::min(tmin, h.temperature); tmax = std::max(tmax, h.temperature); }
        }
        if (n_temp != 0 && n_temp != n)
            problems.push_back("temperature keyword present in " + std::to_string(n_temp) + " of "
                               + std::to_string(n) + " frames");
        if (n_temp == n && kind != MasterKind::BIAS && tmax - tmin > config.temperature_spread_c)
            problems.push_back("temperatures span " + fmt(tmin) + " to " + fmt(tmax) + " C, more than the "
                               + fmt(config.temperature_spread_c) + " C one master may cover; split the set by temperature");
    }
    if (!problems.empty()) {
        std::string e = "Cannot build the " + what + " (" + std::to_string(n) + " frames):";
        const std::size_t show = std::min<std::size_t>(problems.size(), 8);
        for (std::size_t i = 0; i < show; ++i) e += "\n  - " + problems[i];
        if (problems.size() > show) e += "\n  - ... and " + std::to_string(problems.size() - show) + " more";
        res.error = e;
        return res;
    }

    const int W = hdr[0].width, H = hdr[0].height;

    // ── 2. Read every frame into the frame cache ────────────────────
    // The cache's uint16 encoding is round(v * 65535): exact for 8- and
    // 16-bit integer data on its fixed scale, which is what calibration
    // frames are.
    // Channel count from the first frame's data (headers give NAXIS1/2 only).
    FITSReadResult first = FITSReader::read_fixed_scale(paths[0]);
    if (!first.success) {
        res.error = "Cannot build the " + what + ": " + base_name(paths[0]) + ": " + first.error;
        return res;
    }
    const int C = first.image.n_channels();
    const float full_scale = first.full_scale;
    if (pedestal && (pedestal->width() != W || pedestal->height() != H || pedestal->n_channels() != C)) {
        res.error = "Cannot build the master flat: the pedestal is " + std::to_string(pedestal->width()) + "x"
                  + std::to_string(pedestal->height()) + "x" + std::to_string(pedestal->n_channels())
                  + " and the flats are " + std::to_string(W) + "x" + std::to_string(H) + "x" + std::to_string(C);
        return res;
    }

    std::unique_ptr<FrameCache> cache;
    try {
        cache = std::make_unique<FrameCache>(W, H, C, n, config.cache_dir);
    } catch (const std::exception& e) {
        res.error = "Cannot build the " + what + ": " + e.what();
        return res;
    }

    std::vector<float> level(static_cast<std::size_t>(n), 1.0f);   // flats: each frame's own level
    std::vector<std::string> frame_error(static_cast<std::size_t>(n));
    std::mutex cache_mutex;
    std::atomic<bool> float_quantized{false};
    const int hw = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    const int read_workers = config.max_read_workers > 0 ? config.max_read_workers : std::min(hw, 8);

    obs.begin_phase(std::string("Reading ") + master_kind_name(kind) + " frames", n);
    std::atomic<int> read_done{0};
    int read_reported = 0;
    parallel_for_dynamic(n, 1, read_workers, [&](int f, int worker) {
        if (cancel.poll(worker)) return;
        FITSReadResult rr = (f == 0) ? std::move(first) : FITSReader::read_fixed_scale(paths[f]);
        std::string& err = frame_error[static_cast<std::size_t>(f)];
        if (!rr.success) { err = rr.error; return; }
        const Image& img = rr.image;
        if (img.width() != W || img.height() != H || img.n_channels() != C) {
            err = "geometry " + std::to_string(img.width()) + "x" + std::to_string(img.height()) + "x"
                + std::to_string(img.n_channels()) + " differs from the set's";
            return;
        }
        if (rr.full_scale != full_scale) {
            err = "data scale " + fmt_g(rr.full_scale) + " differs from the set's " + fmt_g(full_scale);
            return;
        }
        const float* d = img.data();
        const std::size_t np = img.data_size();
        for (std::size_t i = 0; i < np; ++i) {
            if (!(d[i] >= 0.0f && d[i] <= 1.0f)) {
                err = "pixel value " + fmt_g(d[i]) + " is outside [0, 1] on the frame's fixed scale "
                      "(float data that is neither normalised nor raw ADU)";
                return;
            }
        }
        if (rr.full_scale == 1.0f) float_quantized.store(true, std::memory_order_relaxed);
        if (kind == MasterKind::FLAT) {
            std::vector<float> tmp(np);
            const float* p = pedestal->data();
            for (std::size_t i = 0; i < np; ++i) tmp[i] = d[i] - p[i];
            const float lv = image_location(tmp.data(), np, config.huber);
            if (!(lv > 0.0f)) {
                err = "no light left after subtracting the pedestal (level " + fmt_g(lv) + ")";
                return;
            }
            level[static_cast<std::size_t>(f)] = lv;
        }
        {
            std::lock_guard<std::mutex> lk(cache_mutex);
            cache->write_frame(img, f);
        }
        read_done.fetch_add(1, std::memory_order_relaxed);
        if (worker == 0) {
            const int done = read_done.load(std::memory_order_relaxed);
            if (done > read_reported) { obs.advance(done - read_reported); read_reported = done; }
        }
    });
    obs.end_phase();
    if (cancel.cancelled()) { res.error = "Cancelled while building the " + what + "."; return res; }
    {
        std::string e;
        for (int f = 0; f < n; ++f)
            if (!frame_error[f].empty()) e += "\n  - " + base_name(paths[f]) + ": " + frame_error[f];
        if (!e.empty()) { res.error = "Cannot build the " + what + ":" + e; return res; }
    }
    if (float_quantized.load())
        obs.message("The " + what + "'s frames are floating point; they are held at 16-bit "
                    "precision (1/65535) for the fit, as the stacker holds lights.");

    // ── 3. Fit every pixel's distribution over all N frames ─────────
    MasterFrame m;
    m.kind = kind;
    m.data = Image(W, H, C);
    const bool huber = config.estimator == Config::Estimator::HUBER;
    // Per-worker buffers: N frame-rows, one pixel's N values, scratch.
    struct Buffers { std::vector<float> rows, vals, scratch, ones; std::unique_ptr<ModelSelector> race; };
    std::vector<Buffers> buffers(static_cast<std::size_t>(hw));
    // Local cache slot -> position in the canonical frame order.
    std::vector<int> global_of(static_cast<std::size_t>(n));
    for (int l = 0; l < n; ++l) global_of[l] = cache->global_frame(l);

    obs.begin_phase(std::string("Fitting master ") + master_kind_name(kind) + " ("
                    + estimator_name(config.estimator) + ", " + std::to_string(n) + " frames)", H);
    std::atomic<int> rows_done{0};
    int rows_reported = 0;
    std::atomic<bool> read_failed{false};
    parallel_for_dynamic(H, 4, [&](int y, int worker) {
        if (cancel.poll(worker)) return;
        Buffers& b = buffers[static_cast<std::size_t>(worker)];
        if (b.rows.empty()) {
            b.rows.resize(static_cast<std::size_t>(n) * W);
            b.vals.resize(static_cast<std::size_t>(n));
            b.scratch.resize(static_cast<std::size_t>(n));
            b.ones.assign(static_cast<std::size_t>(n), 1.0f);
            if (!huber) b.race = std::make_unique<ModelSelector>(config.race);
        }
        for (int c = 0; c < C; ++c) {
            for (int l = 0; l < n; ++l) {
                const int g = global_of[l];
                if (!cache->read_frame_range(l, y * W, W, c, b.rows.data() + static_cast<std::size_t>(g) * W)) {
                    read_failed.store(true);
                    return;
                }
            }
            const float* ped = (kind == MasterKind::FLAT) ? pedestal->channel_data(c) + static_cast<std::size_t>(y) * W : nullptr;
            float* out = m.data.channel_data(c) + static_cast<std::size_t>(y) * W;
            for (int x = 0; x < W; ++x) {
                for (int g = 0; g < n; ++g) {
                    float v = b.rows[static_cast<std::size_t>(g) * W + x];
                    if (ped) v = (v - ped[x]) / level[g];
                    b.vals[g] = v;
                }
                out[x] = huber
                    ? HuberEstimator::location(b.vals.data(), nullptr, n, config.huber, b.scratch.data())
                    : b.race->select_best(b.vals.data(), b.ones.data(), n).distribution.true_signal_estimate;
            }
        }
        rows_done.fetch_add(1, std::memory_order_relaxed);
        if (worker == 0) {
            const int done = rows_done.load(std::memory_order_relaxed);
            if (done > rows_reported) { obs.advance(done - rows_reported); rows_reported = done; }
        }
    });
    obs.end_phase();
    if (cancel.cancelled()) { res.error = "Cancelled while building the " + what + "."; return res; }
    if (read_failed.load()) { res.error = "Cannot build the " + what + ": the frame cache refused a read."; return res; }

    // ── 4. A flat is a gain map: scale it to unit level ─────────────
    double flat_level = 1.0;
    if (kind == MasterKind::FLAT) {
        const float lv = image_location(m.data.data(), m.data.data_size(), config.huber);
        if (!(lv > 0.0f)) { res.error = "The master flat's level is " + fmt_g(lv) + "; it cannot be normalised."; return res; }
        const float inv = 1.0f / lv;
        m.data.apply([inv](float v) { return v * inv; });
        flat_level = lv;
    }

    // ── 5. What the master is ───────────────────────────────────────
    m.header = hdr[0];
    m.header.channels = C;
    m.header.full_scale = full_scale;
    m.header.image_type = master_image_type(kind);
    {
        std::vector<float> temps;
        for (const auto& h : hdr) if (h.has_temperature) temps.push_back(static_cast<float>(h.temperature));
        if (!temps.empty()) {
            std::vector<float> scratch(temps.size());
            m.header.temperature = HuberEstimator::location(temps.data(), nullptr, static_cast<int>(temps.size()),
                                                            config.huber, scratch.data());
            m.temperature_min = *std::min_element(temps.begin(), temps.end());
            m.temperature_max = *std::max_element(temps.begin(), temps.end());
        }
    }
    m.n_frames  = n;
    m.estimator = estimator_name(config.estimator);
    m.signature = signature(kind, paths, config, kind == MasterKind::FLAT ? pedestal_signature : std::string());
    if (kind == MasterKind::FLAT) {
        m.pedestal = pedestal_label + " (flat level " + fmt_g(flat_level) + ")";
        m.pedestal_plan = pedestal_label;
        m.pedestal_signature = pedestal_signature;
    }
    for (const auto& p : paths) m.sources.push_back(base_name(p));
    obs.message("Built " + what + " from " + std::to_string(n) + " frames (" + m.header.describe()
                + "), per-pixel " + m.estimator + " fit, no frame or pixel rejected.");
    res.ok = true;
    res.master = std::move(m);
    return res;
}

// ── File format ─────────────────────────────────────────────────────

bool MasterBuilder::write(const MasterFrame& m, const std::string& path, std::string& error) {
    std::vector<FITSCard> cards;
    const CalibrationHeader& h = m.header;
    cards.push_back(FITSCard::str("IMAGETYP", master_image_type(m.kind), "NukeX calibration master"));
    cards.push_back(FITSCard::str("NKXMSTR", master_kind_name(m.kind), "NukeX master kind"));
    cards.push_back(FITSCard::integ("NKXMVER", kMasterFormatVersion, "NukeX master format version"));
    if (!h.instrument.empty())    cards.push_back(FITSCard::str("INSTRUME", h.instrument));
    if (!h.bayer_pattern.empty()) cards.push_back(FITSCard::str("BAYERPAT", h.bayer_pattern));
    cards.push_back(FITSCard::integ("XBINNING", h.xbin));
    cards.push_back(FITSCard::integ("YBINNING", h.ybin));
    if (h.has_exposure) cards.push_back(FITSCard::num("EXPTIME", h.exposure, "[s] exposure of the input frames"));
    if (h.has_temperature) {
        cards.push_back(FITSCard::num("CCD-TEMP", h.temperature, "[C] Huber location of the inputs' temperatures"));
        cards.push_back(FITSCard::flag("NKXTSETP", h.temperature_is_setpoint, "temperature is the cooler set point"));
        cards.push_back(FITSCard::num("NKXTMIN", m.temperature_min, "[C] coldest input"));
        cards.push_back(FITSCard::num("NKXTMAX", m.temperature_max, "[C] warmest input"));
    }
    if (h.has_gain)   cards.push_back(FITSCard::num("GAIN", h.gain, "camera gain setting as written"));
    if (h.has_offset) cards.push_back(FITSCard::num("OFFSET", h.offset, "camera offset as written"));
    cards.push_back(FITSCard::integ("NCOMBINE", m.n_frames, "frames fitted, none rejected"));
    cards.push_back(FITSCard::num("NKXFSCAL", h.full_scale, "data = ADU / NKXFSCAL"));
    cards.push_back(FITSCard::str("NKXEST", m.estimator, "per-pixel estimator"));
    cards.push_back(FITSCard::str("NKXSIG", m.signature, "provenance signature of the inputs"));
    if (!m.pedestal.empty()) cards.push_back(FITSCard::str("NKXPED", m.pedestal.substr(0, 60), "subtracted from each flat"));
    cards.push_back(FITSCard::str("DATE", utc_now(), "UTC when written"));

    std::vector<std::string> history;
    history.push_back(std::string("NukeX master ") + master_kind_name(m.kind) + ": per-pixel "
                      + m.estimator + " fit of " + std::to_string(m.n_frames) + " frames, no frame or pixel rejected");
    if (!m.pedestal.empty()) history.push_back("pedestal subtracted from each flat: " + m.pedestal);
    push_long(history, "NKXPED", m.pedestal);
    push_long(history, "NKXPPLAN", m.pedestal_plan);
    push_long(history, "NKXPSIG", m.pedestal_signature);
    for (const auto& s : m.sources) history.push_back("source: " + s);
    return FITSWriter::write_float(path, m.data, cards, history, error);
}

MasterBuilder::Result MasterBuilder::read(const std::string& path) {
    Result res;
    auto kw = FITSReader::read_keywords(path);
    if (kw.empty()) { res.error = "Cannot read master " + path; return res; }
    MasterFrame m;
    if (!kind_from_name(kw["NKXMSTR"], m.kind)) {
        res.error = path + " is not a NukeX calibration master (no NKXMSTR keyword)";
        return res;
    }
    double ver = 0;
    if (!parse_double(kw["NKXMVER"], ver) || static_cast<int>(ver) != kMasterFormatVersion) {
        res.error = path + ": master format version '" + kw["NKXMVER"] + "', this build reads "
                  + std::to_string(kMasterFormatVersion);
        return res;
    }
    FITSReadResult rr = FITSReader::read_raw(path);
    if (!rr.success) { res.error = "Cannot read master " + path + ": " + rr.error; return res; }
    m.data = std::move(rr.image);
    CalibrationHeader& h = m.header;
    h = CalibrationHeader::from_metadata(rr.metadata, m.data.n_channels(), 0.0);
    h.image_type = kw["IMAGETYP"];
    double v = 0;
    if (!parse_double(kw["NKXFSCAL"], v) || !(v > 0)) { res.error = path + ": missing NKXFSCAL"; return res; }
    h.full_scale = v;
    if (kw.count("NKXTSETP")) h.temperature_is_setpoint = (kw["NKXTSETP"] == "T");
    if (parse_double(kw["NKXTMIN"], v)) m.temperature_min = v;
    if (parse_double(kw["NKXTMAX"], v)) m.temperature_max = v;
    if (parse_double(kw["NCOMBINE"], v)) m.n_frames = static_cast<int>(v);
    m.estimator = kw["NKXEST"];
    m.signature = kw["NKXSIG"];
    m.pedestal  = kw["NKXPED"];   // a card holds 60 characters of it; HISTORY holds all
    const std::vector<std::string> history = FITSReader::read_history(path);
    if (std::string full = read_long(history, "NKXPED"); !full.empty()) m.pedestal = std::move(full);
    m.pedestal_plan = read_long(history, "NKXPPLAN");
    m.pedestal_signature = read_long(history, "NKXPSIG");
    for (const auto& line : history)
        if (line.rfind("source: ", 0) == 0) m.sources.push_back(line.substr(8));
    m.path = path;
    res.ok = true;
    res.master = std::move(m);
    return res;
}

MasterBuilder::Result MasterBuilder::build_or_load(MasterKind kind, const std::vector<std::string>& paths,
                                                   const std::string& cache_path, const Config& config,
                                                   const Image* pedestal,
                                                   const std::string& pedestal_label,
                                                   const std::string& pedestal_signature,
                                                   ProgressObserver* progress) {
    ProgressObserver& obs = progress ? *progress : null_progress_observer();
    const std::string want = signature(kind, paths, config, kind == MasterKind::FLAT ? pedestal_signature : std::string());
    std::error_code ec;
    if (std::filesystem::exists(cache_path, ec)) {
        Result cached = read(cache_path);
        if (cached.ok && cached.master.kind == kind && cached.master.signature == want) {
            obs.message(std::string("Using cached master ") + master_kind_name(kind) + " " + cache_path
                        + " (" + std::to_string(cached.master.n_frames) + " frames, signature " + want + ").");
            cached.reused = true;
            return cached;
        }
        obs.message(std::string("Rebuilding master ") + master_kind_name(kind) + " " + cache_path + ": "
                    + (cached.ok ? "it was built from different frames or settings (signature "
                                   + cached.master.signature + ", wanted " + want + ")"
                                 : cached.error));
    }
    Result built = build(kind, paths, config, pedestal, pedestal_label, pedestal_signature, progress);
    if (!built.ok) return built;
    std::string err;
    if (!write(built.master, cache_path, err)) {
        built.ok = false;
        built.error = "Built the master " + std::string(master_kind_name(kind)) + " but could not cache it: " + err;
        return built;
    }
    built.master.path = cache_path;
    return built;
}

} // namespace nukex
