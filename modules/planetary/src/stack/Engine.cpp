#include "Engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "Align.h"
#include "Demosaic.h"
#include "Parallel.h"
#include "Regions.h"
#include "SerFile.h"

namespace planetary::stack {

const char* const kUndeclaredMosaicWarning =
    "No Bayer pattern is known for this AVI (no capture sidecar names one), so the raw colour mosaic was stacked as "
    "mono: the result is a mono image of the mosaic and cannot be debayered afterwards (every frame was "
    "resampled onto the stack, which mixes the colour sites). For colour, choose the Bayer pattern when "
    "opening the capture (SERViewer's Save pattern... remembers it) and stack again.";

namespace {

using Clock = std::chrono::steady_clock;

double Since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

std::string Num(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

std::string Count(int64_t v) { return std::to_string(v); }

std::string Plural(int64_t v, const std::string& one, const std::string& many) {
    return Count(v) + " " + (v == 1 ? one : many);
}

double Median(std::vector<double> v) {
    if (v.empty()) return 0;
    const size_t m = (v.size() - 1) / 2;  // lower median, as AlignmentCut takes it
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(m), v.end());
    return v[m];
}

// Counts the frame payload every reader delivers (StackSummary::bytesRead).
class MeteredSource : public ser::FrameSource {
public:
    MeteredSource(std::unique_ptr<ser::FrameSource> inner, std::atomic<int64_t>& bytes)
        : inner_(std::move(inner)), bytes_(bytes) {}
    const ser::FileInfo& Info() const override { return inner_->Info(); }
    ser::Frame ReadFrame(int64_t index) const override {
        ser::Frame f = inner_->ReadFrame(index);
        const int64_t payload = inner_->Info().frameBytes;
        bytes_ += payload > 0 ? payload
                              : static_cast<int64_t>(f.samples.size()) * std::max(1, inner_->Info().bytesPerSample);
        return f;
    }
    std::optional<int64_t> FrameTimestamp(int64_t index) const override { return inner_->FrameTimestamp(index); }

private:
    std::unique_ptr<ser::FrameSource> inner_;
    std::atomic<int64_t>& bytes_;
};

// One progress phase: a fixed total, a done count that never goes back, reported only when it moves.
class Phase {
public:
    Phase(const Progress& progress, const char* name, int64_t total)
        : progress_(progress), name_(name), total_(std::max<int64_t>(1, total)) {}
    void Report(int64_t done) {
        done = std::clamp<int64_t>(done, last_ < 0 ? 0 : last_, total_);
        if (done == last_) return;
        last_ = done;
        if (progress_.report) progress_.report(name_, done, total_);
    }
    void Finish() { Report(total_); }
    bool Cancelled() const { return progress_.cancelled && progress_.cancelled(); }
    void ThrowIfCancelled() const {
        if (Cancelled()) throw StackCancelled(std::string("cancelled while ") + name_);
    }
    const char* Name() const { return name_; }
    // A Progress for a stage that reports its own (done, total): mapped onto [offset, offset + span)
    // of this phase. cancelled() is the caller's own.
    Progress Mapped(int64_t offset, int64_t span) {
        Progress p;
        p.report = [this, offset, span](const std::string&, int64_t done, int64_t total) {
            if (total <= 0) return;
            Report(offset + static_cast<int64_t>(static_cast<double>(span) * static_cast<double>(std::min(done, total)) /
                                                 static_cast<double>(total)));
        };
        p.cancelled = progress_.cancelled;
        return p;
    }

private:
    const Progress& progress_;
    const char* name_;
    int64_t total_;
    int64_t last_ = -1;
};

void CheckReader(const ser::FrameSource* src, const ser::FileInfo& info) {
    if (!src) throw ser::SerError("RunStack: the reader factory returned no reader");
    const ser::FileInfo& si = src->Info();
    if (si.frameCount != info.frameCount || si.header.width != info.header.width ||
        si.header.height != info.header.height || si.colorId != info.colorId || si.normDepth != info.normDepth)
        throw ser::SerError(std::string("RunStack: a reader opened ") + std::to_string(si.header.width) + "x" +
                            std::to_string(si.header.height) + ", " + std::to_string(si.frameCount) + " frames, " +
                            ser::ColorIdName(si.colorId) + ", depth " + std::to_string(si.normDepth) + "; expected " +
                            std::to_string(info.header.width) + "x" + std::to_string(info.header.height) + ", " +
                            std::to_string(info.frameCount) + " frames, " + ser::ColorIdName(info.colorId) + ", depth " +
                            std::to_string(info.normDepth));
}

// Runs body(worker, reader, pos) for every pos in [0, n) on `workers` threads (fewer when there are
// fewer runs), each with its own reader from `factory`, claiming runs of kRun consecutive positions so
// each reader reads forward (callers order positions by frame index). The calling thread reports
// offset + done on `phase` and polls cancel every 10 ms -- MeasureAll's pattern. Every thread is joined
// on every way out; a cancel throws StackCancelled, else the first worker error is rethrown.
template <class Body>
void ForFrames(int64_t n, int workers, const SourceFactory& factory, const ser::FileInfo& info, Phase& phase,
               int64_t offset, Body body) {
    phase.ThrowIfCancelled();
    if (n <= 0) return;
    constexpr int64_t kRun = 8;
    const int count = static_cast<int>(std::min<int64_t>(std::max(1, workers), (n + kRun - 1) / kRun));
    std::atomic<int64_t> next{0}, done{0};
    std::atomic<bool> stop{false};
    std::mutex m;
    std::condition_variable cv;
    std::exception_ptr error;
    int finished = 0;

    auto work = [&](int w) {
        try {
            const std::unique_ptr<ser::FrameSource> src = factory();
            CheckReader(src.get(), info);
            while (!stop.load(std::memory_order_relaxed)) {
                const int64_t first = next.fetch_add(kRun);
                if (first >= n) break;
                const int64_t last = std::min(n, first + kRun);
                for (int64_t i = first; i < last && !stop.load(std::memory_order_relaxed); ++i) {
                    body(w, *src, i);
                    done.fetch_add(1);
                }
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock(m);
            if (!error) error = std::current_exception();
            stop = true;
        }
        std::lock_guard<std::mutex> lock(m);
        ++finished;
        cv.notify_all();
    };

    std::vector<std::thread> pool;
    struct Joiner {
        std::vector<std::thread>& pool;
        std::atomic<bool>& stop;
        ~Joiner() {
            stop = true;
            for (std::thread& t : pool)
                if (t.joinable()) t.join();
        }
    } joiner{pool, stop};
    pool.reserve(static_cast<size_t>(count));
    // A thread that cannot be started (std::system_error): the exception leaves through Joiner, which
    // stops and joins the threads already running.
    for (int k = 0; k < count; ++k) pool.emplace_back(work, k);

    bool cancelled = false;
    std::unique_lock<std::mutex> lock(m);
    while (finished < count) {
        cv.wait_for(lock, std::chrono::milliseconds(10), [&] { return finished == count; });
        lock.unlock();
        phase.Report(offset + done.load());
        if (!cancelled && phase.Cancelled()) {
            cancelled = true;
            stop = true;
        }
        lock.lock();
    }
    lock.unlock();
    for (std::thread& t : pool) t.join();
    if (cancelled) throw StackCancelled(std::string("cancelled while ") + phase.Name());
    if (error) std::rethrow_exception(error);
    phase.Report(offset + n);
    phase.ThrowIfCancelled();
}

// ResidualScale instances shared by the alignment pass's workers: a worker checks one out per Observe.
class ObserverPool {
public:
    ObserverPool(int count, const RegionLayout& layout, const ser::FloatImage& reference) {
        for (int i = 0; i < count; ++i) {
            all_.push_back(std::make_unique<ResidualScale>(layout, reference));
            free_.push_back(all_.back().get());
        }
    }
    void Observe(const ser::FloatImage& luma, const FrameRegions& m) {
        ResidualScale* r;
        {
            std::unique_lock<std::mutex> lock(m_);
            cv_.wait(lock, [&] { return !free_.empty(); });
            r = free_.back();
            free_.pop_back();
        }
        struct Back {
            ObserverPool& pool;
            ResidualScale* r;
            ~Back() {
                {
                    std::lock_guard<std::mutex> lock(pool.m_);
                    pool.free_.push_back(r);
                }
                pool.cv_.notify_one();
            }
        } back{*this, r};
        r->Observe(luma, m);
    }
    // Merges every instance into the first; its Sigma.
    ser::FloatImage Sigma(int normDepth) {
        for (size_t i = 1; i < all_.size(); ++i) all_[0]->Merge(*all_[i]);
        all_.resize(1);
        return all_[0]->Sigma(normDepth);
    }

private:
    std::vector<std::unique_ptr<ResidualScale>> all_;
    std::vector<ResidualScale*> free_;
    std::mutex m_;
    std::condition_variable cv_;
};

StackOutcome Diagnosed(StackOutcome o, DiagnosisKind kind, std::string message,
                       std::vector<std::pair<std::string, double>> numbers) {
    o.diagnosis.kind = kind;
    o.diagnosis.message = std::move(message);
    o.diagnosis.numbers = std::move(numbers);
    o.image.reset();
    o.summary.used = 0;
    o.frames.stacked.clear();
    o.frames.regionsKeeping.clear();
    return o;
}

}  // namespace

std::vector<FocusWindow> FocusWindows(const RegionLayout& layout, double lumaScale) {
    std::vector<FocusWindow> out;
    const int side = std::min({static_cast<int>(kFocusWindow / lumaScale), layout.width, layout.height});
    if (side < kMinFocusWindow) return out;
    // A window of `side` centred on (cx, cy), moved inside the image.
    auto add = [&](int cx, int cy) {
        FocusWindow f;
        f.side = side;
        f.x0 = std::clamp(cx - side / 2, 0, layout.width - side);
        f.y0 = std::clamp(cy - side / 2, 0, layout.height - side);
        out.push_back(f);
    };
    if (!layout.regions.empty()) {
        for (const Region& r : layout.regions) add((r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2);
    } else {
        for (int j = 0; j < layout.ny; ++j)
            for (int i = 0; i < layout.nx; ++i)
                add(layout.originX + i * layout.step + layout.size / 2, layout.originY + j * layout.step + layout.size / 2);
    }
    return out;
}

std::vector<FocusProfile> MeasureFocus(const std::vector<ser::FloatImage>& lumas, const std::vector<Shift>& shifts,
                                       const std::vector<FocusWindow>& windows, int threads,
                                       const std::function<void()>& poll) {
    if (lumas.size() != shifts.size()) throw std::invalid_argument("MeasureFocus: one shift per frame");
    for (const ser::FloatImage& l : lumas)
        if (l.channels != 1 || l.width != lumas[0].width || l.height != lumas[0].height ||
            l.data.size() != static_cast<size_t>(l.width) * l.height)
            throw std::invalid_argument("MeasureFocus: frames must be 1-channel images of one size");
    std::vector<FocusProfile> out(windows.size());
    if (lumas.size() < 2) return out;
    const int W = lumas[0].width, H = lumas[0].height;
    for (const FocusWindow& f : windows)
        if (f.side < kMinFocusWindow || f.x0 < 0 || f.y0 < 0 || f.x0 + f.side > W || f.y0 + f.side > H)
            throw std::invalid_argument("MeasureFocus: a window is not inside the frames");
    auto measure = [&](size_t wi, const std::function<void()>* framePoll) {
            const FocusWindow& win = windows[wi];
            const int s = win.side, n = NextFastSize(s);
            const Fft2D fft(n, n);
            std::vector<double> taper(static_cast<size_t>(s));
            for (int i = 0; i < s; ++i) {
                // Hann for texture windows; flat for a disc window (the disc sits inside it on dark sky).
                const double v = win.taper ? std::sin(M_PI * (i + 0.5) / s) : 1.0;
                taper[static_cast<size_t>(i)] = v * v;
            }
            std::vector<Complex> a(static_cast<size_t>(n) * n, Complex(0, 0)), b = a, data;
            int na = 0, nb = 0;
            for (size_t k = 0; k < lumas.size(); ++k) {
                if (framePoll) (*framePoll)();
                // The window's content in frame k sits at the reference window + the global shift: read it
                // at the integer part, then move it by the fraction with the Fourier shift theorem (exact
                // for the windowed data; interpolation would colour the noise and soften the detail).
                const double gx = win.x0 + shifts[k].dx, gy = win.y0 + shifts[k].dy;
                // Kept inside the frame: near an edge the read position is clamped and the phase shift takes
                // the whole remaining offset (the content entering at the window edge is tapered away).
                const int ix = std::clamp(static_cast<int>(std::floor(gx)), 0, W - s);
                const int iy = std::clamp(static_cast<int>(std::floor(gy)), 0, H - s);
                const double fx = gx - ix, fy = gy - iy;
                const ser::FloatImage& l = lumas[k];
                double mean = 0;
                for (int y = 0; y < s; ++y)
                    for (int x = 0; x < s; ++x) mean += l.data[static_cast<size_t>(iy + y) * W + ix + x];
                mean /= static_cast<double>(s) * s;
                data.assign(static_cast<size_t>(n) * n, Complex(0, 0));
                for (int y = 0; y < s; ++y)
                    for (int x = 0; x < s; ++x)
                        data[static_cast<size_t>(y) * n + x] =
                            (l.data[static_cast<size_t>(iy + y) * W + ix + x] - mean) * taper[static_cast<size_t>(x)] *
                            taper[static_cast<size_t>(y)];
                fft.Forward(data);
                std::vector<Complex>& acc = (k % 2 == 0) ? a : b;
                ((k % 2 == 0) ? na : nb) += 1;
                for (int ky = 0; ky < n; ++ky) {
                    const double vy = (ky <= n / 2 ? ky : ky - n) / static_cast<double>(n);
                    for (int kx = 0; kx < n; ++kx) {
                        const double vx = (kx <= n / 2 ? kx : kx - n) / static_cast<double>(n);
                        const size_t i = static_cast<size_t>(ky) * n + kx;
                        acc[i] += data[i] * std::polar(1.0, 2 * M_PI * (vx * fx + vy * fy));
                    }
                }
            }
            FocusProfile& p = out[static_cast<size_t>(wi)];
            p.side = n;
            p.frames = na + nb;
            if (na == 0 || nb == 0) return;
            // The taper's own power spectrum: its second moment adds to the signal's (see FocusProfile).
            data.assign(static_cast<size_t>(n) * n, Complex(0, 0));
            for (int y = 0; y < s; ++y)
                for (int x = 0; x < s; ++x)
                    data[static_cast<size_t>(y) * n + x] = taper[static_cast<size_t>(x)] * taper[static_cast<size_t>(y)];
            fft.Forward(data);
            const int bins = n / 2 + 1;
            p.cross.assign(static_cast<size_t>(bins), 0.0);
            p.power.assign(static_cast<size_t>(bins), 0.0);
            for (int ky = 0; ky < n; ++ky) {
                const int vy = ky <= n / 2 ? ky : ky - n;
                for (int kx = 0; kx < n; ++kx) {
                    const int vx = kx <= n / 2 ? kx : kx - n;
                    const size_t i = static_cast<size_t>(ky) * n + kx;
                    const double f2 = static_cast<double>(vx * vx + vy * vy) / (static_cast<double>(n) * n);
                    const double c = (a[i] * std::conj(b[i])).real() / (static_cast<double>(na) * nb);
                    const double pw = 0.5 * (std::norm(a[i]) / (static_cast<double>(na) * na) +
                                             std::norm(b[i]) / (static_cast<double>(nb) * nb));
                    const double t = std::norm(data[i]);
                    // A disc window drops DC: its window mean is the disc's flux, not detail (and the sky level
                    // only moves DC). A texture window keeps it: the taper moments are additive over every bin.
                    if (!win.taper && vx == 0 && vy == 0) continue;
                    p.c0 += c;
                    p.c2 += f2 * c;
                    p.p0 += pw;
                    p.w0 += t;
                    p.w2 += f2 * t;
                    const int r = static_cast<int>(std::lround(std::sqrt(static_cast<double>(vx * vx + vy * vy))));
                    if (r >= bins) continue;  // the profile stops at Nyquist; the corners count in the sums above
                    p.cross[static_cast<size_t>(r)] += c;
                    p.power[static_cast<size_t>(r)] += pw;
                }
            }
            if (!win.taper) {  // no taper to correct for
                p.w0 = 1;
                p.w2 = 0;
            }
    };
    if (windows.size() == 1) {  // one window (a disc): on the calling thread, polled between frames
        measure(0, poll ? &poll : nullptr);
        return out;
    }
    // Windows over ParallelBands, in groups (polled between them): window w is "row" w x kMinBandRows, so
    // every band of whole rows holds whole windows and each window is in one band.
    const size_t group = static_cast<size_t>(std::max(1, threads)) * 4;
    for (size_t g0 = 0; g0 < windows.size(); g0 += group) {
        if (poll) poll();
        const size_t count = std::min(group, windows.size() - g0);
        ParallelBands(static_cast<int>(count) * kMinBandRows, threads, [&](int r0, int r1) {
            for (int i = (r0 + kMinBandRows - 1) / kMinBandRows; i < (r1 + kMinBandRows - 1) / kMinBandRows; ++i)
                measure(g0 + static_cast<size_t>(i), nullptr);
        });
    }
    if (poll) poll();
    return out;
}

FocusWindow DiscFocusWindow(double cx, double cy, double radius, int width, int height) {
    FocusWindow f;
    f.taper = false;
    f.cx = cx;
    f.cy = cy;
    const int fit = PrevFastSize(std::max(1, std::min(width, height)));
    if (fit < kMinFocusWindow || fit < 2 * radius + 16) return f;  // no room for the disc and 8 px of sky
    const int want = NextFastSize(static_cast<int>(std::ceil(4 * radius + 16)));
    f.side = std::min(want, fit);
    f.x0 = std::clamp(static_cast<int>(std::lround(cx)) - f.side / 2, 0, width - f.side);
    f.y0 = std::clamp(static_cast<int>(std::lround(cy)) - f.side / 2, 0, height - f.side);
    return f;
}

double DiscHalfRadius(const ser::FloatImage& luma, double cx, double cy, double r0, const FocusWindow& window) {
    if (luma.channels != 1 || window.side < kMinFocusWindow || window.x0 < 0 || window.y0 < 0 ||
        window.x0 + window.side > luma.width || window.y0 + window.side > luma.height)
        throw std::invalid_argument("DiscHalfRadius: the window is not inside the image");
    const int W = luma.width, s = window.side;
    auto at = [&](int x, int y) { return static_cast<double>(luma.data[static_cast<size_t>(y) * W + x]); };
    std::vector<double> border;
    for (int i = 0; i < s; ++i) {
        border.push_back(at(window.x0 + i, window.y0));
        border.push_back(at(window.x0 + i, window.y0 + s - 1));
        border.push_back(at(window.x0, window.y0 + i));
        border.push_back(at(window.x0 + s - 1, window.y0 + i));
    }
    const double sky = Median(border);
    constexpr double kRing = 0.25;
    const double reach = std::max(cx - window.x0, std::max(window.x0 + s - 1 - cx, std::max(cy - window.y0, window.y0 + s - 1 - cy)));
    const size_t rings = static_cast<size_t>(reach / kRing) + 1;
    std::vector<double> sum(rings, 0.0), count(rings, 0.0);
    double inner = 0, innerCount = 0;
    const double innerR = std::max(1.0, 0.5 * r0);
    for (int y = window.y0; y < window.y0 + s; ++y)
        for (int x = window.x0; x < window.x0 + s; ++x) {
            const double r = std::hypot(x - cx, y - cy);
            const size_t k = static_cast<size_t>(r / kRing);
            if (k < rings) {
                sum[k] += at(x, y);
                count[k] += 1;
            }
            if (r < innerR) {
                inner += at(x, y);
                innerCount += 1;
            }
        }
    if (innerCount == 0) return 0;
    const double half = sky + 0.5 * (inner / innerCount - sky);
    double prevR = 0, prevV = inner / innerCount;
    for (size_t k = 0; k < rings; ++k) {
        if (count[k] == 0) continue;
        const double r = (k + 0.5) * kRing, v = sum[k] / count[k];
        if (v <= half) return prevV > v ? prevR + (r - prevR) * (prevV - half) / (prevV - v) : r;
        prevR = r;
        prevV = v;
    }
    return 0;
}

double DiscRadius(const ser::FloatImage& luma, double cx, double cy, double r0) {
    double r = r0;
    for (int step = 0; step < 6; ++step) {
        FocusWindow win = DiscFocusWindow(cx, cy, r, luma.width, luma.height);
        if (win.side == 0) {  // an inflated first guess: the largest window the frame allows
            win.side = PrevFastSize(std::max(1, std::min(luma.width, luma.height)));
            if (win.side < kMinFocusWindow) break;
            win.x0 = std::clamp(static_cast<int>(std::lround(cx)) - win.side / 2, 0, luma.width - win.side);
            win.y0 = std::clamp(static_cast<int>(std::lround(cy)) - win.side / 2, 0, luma.height - win.side);
        }
        const double next = DiscHalfRadius(luma, cx, cy, r, win);
        if (!(next > 0)) break;
        const bool settled = std::fabs(next - r) < 0.05;
        r = next;
        if (settled) break;
    }
    return r;
}

namespace {

// The disc model reduced to its distinct radial frequencies: the power is (2 J1(x) / x)^2 exp(-4 pi^2
// sigma^2 f^2) sinc^2(fx) sinc^2(fy); every factor but the separable pixel sinc^2 depends on f^2 =
// (kx^2 + ky^2) / n^2 alone, so the grid's bins are grouped by the integer key kx^2 + ky^2 with their
// sinc^2 summed, and the Bessel term evaluated once per key. Exact (the same sums, regrouped); a model
// evaluation is then one exp per distinct key -- ~0.4 M for n = 2048 instead of 4 M bins with a Bessel
// each. poll() is called every 64 rows.
struct DiscModel {
    std::vector<double> f2, weight;  // per distinct key: f^2 and (2 J1 / x)^2 x the summed sinc^2
};

DiscModel BuildDiscModel(double radius, int n, const std::function<void()>& poll) {
    auto sinc = [](double u) { return u == 0 ? 1.0 : std::sin(M_PI * u) / (M_PI * u); };
    std::vector<double> s2(static_cast<size_t>(n));
    for (int k = 0; k < n; ++k) {
        const double v = sinc((k <= n / 2 ? k : k - n) / static_cast<double>(n));
        s2[static_cast<size_t>(k)] = v * v;
    }
    const int half = n / 2 + 1;
    const size_t maxKey = 2 * static_cast<size_t>(half) * static_cast<size_t>(half);
    std::vector<double> sum(maxKey + 1, 0.0);
    for (int ky = 0; ky < n; ++ky) {
        if (poll && ky % 64 == 0) poll();
        const int vy = ky <= n / 2 ? ky : ky - n;
        for (int kx = 0; kx < n; ++kx) {
            if (kx == 0 && ky == 0) continue;
            const int vx = kx <= n / 2 ? kx : kx - n;
            sum[static_cast<size_t>(vx * vx + vy * vy)] += s2[static_cast<size_t>(kx)] * s2[static_cast<size_t>(ky)];
        }
    }
    DiscModel m;
    for (size_t key = 1; key <= maxKey; ++key) {
        if (sum[key] == 0) continue;
        const double f2 = static_cast<double>(key) / (static_cast<double>(n) * n);
        const double x = 2 * M_PI * std::sqrt(f2) * radius;
        const double airy = x > 1e-9 ? 2 * std::cyl_bessel_j(1.0, x) / x : 1.0;
        m.f2.push_back(f2);
        m.weight.push_back(airy * airy * sum[key]);
    }
    return m;
}

double ModelDetail(const DiscModel& m, double sigma) {
    const double c = 4 * M_PI * M_PI * sigma * sigma;
    double num = 0, den = 0;
    for (size_t i = 0; i < m.f2.size(); ++i) {
        const double p = m.weight[i] * std::exp(-c * m.f2[i]);
        num += m.f2[i] * p;
        den += p;
    }
    return num > 0 && den > 0 ? 1 / (2 * M_PI * std::sqrt(num / den)) : std::numeric_limits<double>::infinity();
}

}  // namespace

double DiscDetailScale(double radius, double sigma, int n) { return ModelDetail(BuildDiscModel(radius, n, {}), sigma); }

double LimbBlurFromDetail(double detail, double radius, int n, const std::function<void()>& poll) {
    const double maxSigma = 4 * radius + 8;
    const DiscModel m = BuildDiscModel(radius, n, poll);
    if (!(detail > ModelDetail(m, 0))) return 0;
    if (!(detail < ModelDetail(m, maxSigma))) return maxSigma;
    double lo = 0, hi = maxSigma;
    while (hi - lo > 1e-3) {
        if (poll) poll();
        const double mid = 0.5 * (lo + hi);
        (ModelDetail(m, mid) < detail ? lo : hi) = mid;
    }
    return 0.5 * (lo + hi);
}

double FocusDetailScale(const FocusProfile& p, double* snr) {
    // Signal over noise power; no noise at all (identical halves: power == cross-power) is infinite, not 0.
    if (snr) *snr = !(p.c0 > 0) ? 0 : p.p0 > p.c0 ? p.c0 / (p.p0 - p.c0) : std::numeric_limits<double>::infinity();
    if (!(p.c0 > 0) || !(p.w0 > 0)) return std::numeric_limits<double>::infinity();
    const double m = p.c2 / p.c0 - p.w2 / p.w0;
    return m > 0 ? 1 / (2 * M_PI * std::sqrt(m)) : std::numeric_limits<double>::infinity();
}

FocusVerdict JudgeFocus(const std::vector<FocusProfile>& profiles, double lumaScale) {
    FocusVerdict v;
    double c0 = 0, c2 = 0, taper = 0;
    for (const FocusProfile& p : profiles) {
        double snr = 0;
        FocusDetailScale(p, &snr);
        if (!(snr >= kMinFocusSnr) || !(p.c0 > 0) || !(p.w0 > 0)) continue;
        ++v.judged;
        c0 += p.c0;
        c2 += p.c2;
        taper += p.c0 * p.w2 / p.w0;  // the taper's moment, weighted like the signal's
    }
    const double m = c0 > 0 ? (c2 - taper) / c0 : 0;
    v.detailScale = m > 0 ? lumaScale / (2 * M_PI * std::sqrt(m)) : std::numeric_limits<double>::infinity();
    return v;
}

FocusCheck JudgeDiscFocus(const FocusProfile& profile, double radius, double lumaScale, const FocusWindow& window,
                          const std::function<void()>& poll) {
    FocusCheck c;
    c.disc = true;
    c.radius = radius;
    c.windows = 1;
    c.warnAbove = kDiscWarnAbove;
    c.refuseFrom = kDiscRefuseFrom;
    double snr = 0;
    const double detail = FocusDetailScale(profile, &snr);
    if (!(radius > 0) || !(snr >= kMinFocusSnr) || !std::isfinite(detail)) {
        c.note = "the disc shows no signal above the noise";
        return c;
    }
    c.judged = 1;
    c.detailScale = detail * lumaScale;
    const double limb = LimbBlurFromDetail(detail, radius, profile.side, poll);
    c.limbBlur = limb * lumaScale;
    c.measure = limb / radius;
    // Never refused (Ruling P20): refuseFrom is +inf; a gross defocus is Select's to refuse.
    c.outcome = c.measure > c.warnAbove ? FocusOutcome::Warned : FocusOutcome::InFocus;
    // Two ways the number means nothing, on every path: the inversion ran into its limit (the detail lies
    // beyond the disc model -- noise or a blob far wider than the disc it was fitted with), or the blurred
    // halo leaves the window around the disc's centre (the window's edge cuts it).
    if (!(limb < 4 * radius + 8 - 1e-6)) {
        c.note = "the disc's detail lies beyond the blurred-disc model (the limb blur reached its " +
                 Num((4 * radius + 8) * lumaScale, 0) + "-px limit)";
        c.outcome = FocusOutcome::NotJudged;
        return c;
    }
    const double room = std::min({window.cx - window.x0, window.x0 + window.side - 1 - window.cx, window.cy - window.y0,
                                  window.y0 + window.side - 1 - window.cy});
    if (radius + 3 * limb > room) {
        c.note = "the disc's blurred halo (radius " + Num((radius + 3 * limb) * lumaScale, 0) + " px) does not fit the " +
                 Num(window.side * lumaScale, 0) + "-px window the frame allows";
        c.outcome = FocusOutcome::NotJudged;
    }
    return c;
}

FocusCheck MeasureDiscFocus(const std::vector<ser::FloatImage>& lumas, const std::vector<Shift>& shifts, double cx, double cy,
                            double radius, double lumaScale, int threads, const std::function<void()>& poll,
                            FocusProfile* profile, FocusWindow* window) {
    FocusCheck c;
    c.disc = true;
    c.radius = radius;
    c.warnAbove = kDiscWarnAbove;
    c.refuseFrom = kDiscRefuseFrom;
    if (lumas.empty()) return c;
    const int W = lumas[0].width, H = lumas[0].height;
    FocusWindow win = DiscFocusWindow(cx, cy, radius, W, H);
    if (window) *window = win;
    if (win.side == 0) {
        c.note = "the disc and 8 px of sky around it do not fit the frame";
        return c;
    }
    const int fit = PrevFastSize(std::min(W, H));
    for (int attempt = 0; attempt < 3; ++attempt) {
        const FocusProfile p = MeasureFocus(lumas, shifts, {win}, threads, poll).front();
        if (profile) *profile = p;
        if (window) *window = win;
        c = JudgeDiscFocus(p, radius, lumaScale, win, poll);
        const double limb = c.limbBlur / lumaScale;
        if (c.outcome != FocusOutcome::NotJudged || c.judged == 0 || win.side >= fit) return c;
        const int need = std::min(fit, NextFastSize(static_cast<int>(std::ceil(2 * (radius + 3 * limb) + 8))));
        if (need <= win.side) return c;
        win.side = need;
        win.x0 = std::clamp(static_cast<int>(std::lround(cx)) - need / 2, 0, W - need);
        win.y0 = std::clamp(static_cast<int>(std::lround(cy)) - need / 2, 0, H - need);
    }
    return c;
}

FocusCheck JudgeSurfaceFocus(const std::vector<FocusProfile>& profiles, double lumaScale) {
    FocusCheck c;
    c.windows = static_cast<int>(profiles.size());
    c.warnAbove = kSurfaceWarnAbove;
    c.refuseFrom = std::numeric_limits<double>::infinity();  // never refused: see kSurfaceWarnAbove
    const FocusVerdict v = JudgeFocus(profiles, lumaScale);
    c.judged = v.judged;
    if (v.judged == 0) return c;  // nothing above the noise: stacked, no verdict
    c.detailScale = std::isfinite(v.detailScale) ? v.detailScale : 0.0;
    c.measure = v.detailScale;
    c.outcome = c.measure > c.warnAbove ? FocusOutcome::Warned : FocusOutcome::InFocus;
    return c;
}

std::string FocusWarning(const FocusCheck& c) {
    if (c.outcome != FocusOutcome::Warned) return {};
    if (c.disc)
        return "The stack may be out of focus: the sharpest frames blur the limb by " + Num(c.limbBlur, 1) + " px, " +
               Num(c.measure, 2) + " of the disc's radius (in-focus captures: up to " + Num(c.warnAbove, 2) + ") -- check the focus.";
    return "The stack may be out of focus (or the capture is sampled far finer than the detail it holds): the "
           "sharpest frames show no detail finer than " +
           Num(c.detailScale, 1) + " px (in-focus captures: up to " + Num(c.warnAbove, 1) + " px).";
}

std::string RegionPhrase(const std::vector<RegionDetail>& detail, int64_t alignedFrames, int64_t stackedFrames) {
    if (detail.empty()) return "no region holds detail to rank on, so the " + Count(alignedFrames) + " aligned frames are stacked whole";
    std::vector<int64_t> k;
    int64_t none = 0;
    for (const RegionDetail& d : detail)
        if (d.kept > 0) k.push_back(d.kept);
        else ++none;
    const int64_t regions = static_cast<int64_t>(detail.size());
    std::string out = Plural(regions, "region", "regions") + " placed on the target's detail";
    if (!k.empty()) {
        std::sort(k.begin(), k.end());
        const std::string range = Count(k.front()) + (k.front() == k.back() ? "" : "-" + Count(k.back())) + " of " +
                                  Count(alignedFrames) + " frames";
        const std::string median = k.size() > 1 ? " (median " + Count(k[(k.size() - 1) / 2]) + ")" : "";
        if (none == 0)
            out += (regions == 1 ? ", stacking its own sharpest " : ", each stacking its own sharpest ") + range + median;
        else
            out += ": " + Count(static_cast<int64_t>(k.size())) + " stacking their own sharpest " + range + median + ",";
    }
    if (none > 0)
        out += (k.empty() ? ": " : " ") + Count(none) + " with no detail above the measurement noise taking the mean of the " +
               Count(stackedFrames) + " stacked frames";
    return out;
}

MemoryPlan PlanMemory(const ser::FileInfo& info, int threads) {
    threads = std::max(1, threads);
    const int scale = static_cast<int>(LumaScale(info));
    const size_t w = static_cast<size_t>(std::max(0, info.header.width));
    const size_t h = static_cast<size_t>(std::max(0, info.header.height));
    const size_t lumaPx = (w / static_cast<size_t>(scale)) * (h / static_cast<size_t>(scale));
    const bool rgb = info.colorId == ser::ColorId::RGB || info.colorId == ser::ColorId::BGR;
    const size_t px = w * h, samples = px * (rgb ? 3 : 1);
    const size_t outSamples = px * (rgb || ser::IsBayer(info.colorId) ? 3 : 1);

    MemoryPlan p;
    p.observerBytes = 20 * lumaPx;  // ResidualScale: 16 per luma pixel + its copy of the reference
    // Hot-pixel pass: the raw frame and the worker's own HotPixelMap (2 + 2 bytes per sample); measuring:
    // the raw frame, its luma and MeasureFrame's opened-image scratch (2 per sample, 4 + 8 per luma px).
    p.measureWorkerBytes = 4 * samples + 12 * lumaPx;
    p.alignWorkerBytes = 2 * samples + 8 * lumaPx + kWorkerScratchBytes;  // raw, luma, regions; correlation
    p.stackWorkerBytes = 2 * samples + 8 * px + 8 * outSamples;           // raw, demosaic planes, frame + prefilter
    const size_t lumaImages = 24 * lumaPx;  // reference, residual scale, the Accumulator's copies of both
    p.fixedBytes = 8 * (outSamples + px) + lumaImages;

    auto fit = [&](size_t avail, size_t each) {
        if (each == 0) return threads;
        const size_t n = avail / each;
        return static_cast<int>(std::clamp<size_t>(n, 1, static_cast<size_t>(threads)));
    };
    auto rest = [](size_t total, size_t used) { return total > used ? total - used : 0; };
    p.measureWorkers = fit(kMemoryBudget, p.measureWorkerBytes);
    p.alignWorkers = fit(rest(kMemoryBudget, kResidualBudget + lumaImages), p.alignWorkerBytes);
    p.observers = static_cast<int>(
        std::clamp<size_t>(p.observerBytes ? kResidualBudget / p.observerBytes : 1, 1, static_cast<size_t>(p.alignWorkers)));
    p.stackWorkers = fit(rest(kMemoryBudget, p.fixedBytes), p.stackWorkerBytes);
    const size_t measurePeak = p.measureWorkers * p.measureWorkerBytes;
    const size_t alignPeak = lumaImages + p.observers * p.observerBytes + p.alignWorkers * p.alignWorkerBytes;
    const size_t stackPeak = p.fixedBytes + p.stackWorkers * p.stackWorkerBytes;
    // The focus measure holds its frames after the residual scale's observers are gone (RunStack).
    p.focusBytes = static_cast<size_t>(kFocusFrames) * 4 * lumaPx;
    const size_t focusPeak = lumaImages + p.focusBytes;
    p.peakBytes = std::max({measurePeak, alignPeak, stackPeak, focusPeak});
    return p;
}

RepairingSource::RepairingSource(std::unique_ptr<ser::FrameSource> inner, std::shared_ptr<const HotPixelMap> map)
    : inner_(std::move(inner)), map_(std::move(map)) {
    if (!inner_) throw std::invalid_argument("RepairingSource: no reader");
}

ser::Frame RepairingSource::ReadFrame(int64_t index) const {
    ser::Frame f = inner_->ReadFrame(index);
    if (map_) map_->Repair(f);
    return f;
}

SourceFactory RepairingFactory(SourceFactory base, std::shared_ptr<const HotPixelMap> map) {
    return [base = std::move(base), map = std::move(map)]() -> std::unique_ptr<ser::FrameSource> {
        std::unique_ptr<ser::FrameSource> inner = base();
        if (!inner) return nullptr;
        return std::make_unique<RepairingSource>(std::move(inner), map);
    };
}

StackOutcome RunStack(const std::string& path, ser::BayerChoice bayer, int threads, const Progress& progress) {
    return RunStack([path, bayer] { return ser::OpenVideo(path, bayer); }, threads, progress);
}

StackOutcome RunStack(const SourceFactory& rawFactory, int threads, const Progress& progress) {
    const Clock::time_point start = Clock::now();
    if (!rawFactory) throw std::invalid_argument("RunStack: no reader factory");
    threads = std::max(1, threads);
    std::atomic<int64_t> bytesRead{0};
    const SourceFactory metered = [&rawFactory, &bytesRead]() -> std::unique_ptr<ser::FrameSource> {
        std::unique_ptr<ser::FrameSource> inner = rawFactory();
        if (!inner) return nullptr;
        return std::make_unique<MeteredSource>(std::move(inner), bytesRead);
    };

    StackOutcome out;
    {
        const std::unique_ptr<ser::FrameSource> first = rawFactory();
        if (!first) throw ser::SerError("RunStack: the reader factory returned no reader");
        out.info = first->Info();
    }
    const ser::FileInfo& info = out.info;
    const double scale = LumaScale(info);
    const int64_t n = std::max<int64_t>(0, info.frameCount);
    StackSummary& sum = out.summary;
    sum.measured = n;
    sum.warnings = info.warnings;
    sum.memory = PlanMemory(info, threads);
    const MemoryPlan& mem = sum.memory;
    auto step = [&](const char* name, Clock::time_point t0) { sum.phaseSeconds.emplace_back(name, Since(t0)); };
    auto finish = [&](StackOutcome o) {
        o.summary.seconds = Since(start);
        o.summary.bytesRead = bytesRead.load();
        o.summary.readMBps = o.summary.seconds > 0 ? o.summary.bytesRead / 1e6 / o.summary.seconds : 0;
        return o;
    };

    // ---- Measuring: hot pixels, then every frame -----------------------------------------------------
    const int64_t nHot = std::min<int64_t>(n, kHotPixelFrames);
    Phase measuring(progress, kPhaseMeasuring, nHot + n);
    measuring.Report(0);
    Clock::time_point t0 = Clock::now();
    auto hot = std::make_shared<HotPixelMap>(info);
    {
        std::vector<std::unique_ptr<HotPixelMap>> maps(static_cast<size_t>(mem.measureWorkers));
        ForFrames(nHot, mem.measureWorkers, metered, info, measuring, 0, [&](int w, ser::FrameSource& src, int64_t pos) {
            std::unique_ptr<HotPixelMap>& map = maps[static_cast<size_t>(w)];
            if (!map) map = std::make_unique<HotPixelMap>(info);
            map->Observe(src.ReadFrame(pos * n / nHot));
        });
        for (const auto& map : maps)
            if (map) hot->Merge(*map);
        hot->Finish();
    }
    sum.hotPixels = hot->Count();
    step("Hot pixels", t0);
    const SourceFactory factory = RepairingFactory(metered, hot);

    t0 = Clock::now();
    const std::vector<FrameStats> stats = MeasureAll(factory, info, mem.measureWorkers, measuring.Mapped(nHot, n));
    measuring.Finish();
    measuring.ThrowIfCancelled();
    step("Measuring frames", t0);

    t0 = Clock::now();
    const Selection sel = SelectFrames(stats, info);
    step("Selecting", t0);
    sum.selection = sel.explanation;
    if (sel.diagnosis.kind != DiagnosisKind::None) {
        StackOutcome o = std::move(out);
        o.diagnosis = sel.diagnosis;
        return finish(std::move(o));
    }

    // ---- Aligning: the reference, then every selected frame and suspect ------------------------------
    std::vector<int64_t> alignList = sel.frames;
    for (const Suspect& s : sel.suspects) alignList.push_back(s.index);
    std::sort(alignList.begin(), alignList.end());
    const int64_t refUnits = 3 * std::min<int64_t>(kReferenceFrames, static_cast<int64_t>(sel.frames.size()));
    const int64_t nAlign = static_cast<int64_t>(alignList.size());
    const int64_t focusUnits = std::min<int64_t>(kFocusFrames, nAlign);
    Phase aligning(progress, kPhaseAligning, refUnits + nAlign + focusUnits);
    aligning.Report(0);
    t0 = Clock::now();
    const ser::FloatImage reference =
        BuildReference(factory, info, sel, stats, mem.alignWorkers, aligning.Mapped(0, refUnits));
    aligning.Report(refUnits);
    aligning.ThrowIfCancelled();
    step("Building reference", t0);

    t0 = Clock::now();
    const GlobalAligner aligner(reference, sel.fullFrameTarget, scale);
    const FrameStats refStats = MeasureFrame(reference, 0, scale);
    const RegionLayout layout = PlaceRegions(reference, refStats, sel.fullFrameTarget);
    const RegionMeasurer measurer(reference, layout);
    std::vector<Shift> shifts(static_cast<size_t>(nAlign));
    std::vector<FrameRegions> measured(static_cast<size_t>(nAlign));
    std::vector<uint8_t> hasRegions(static_cast<size_t>(nAlign), 0);
    std::optional<ObserverPool> observers;
    observers.emplace(mem.observers, layout, reference);
    ForFrames(nAlign, mem.alignWorkers, factory, info, aligning, refUnits, [&](int, ser::FrameSource& src, int64_t pos) {
        const size_t p = static_cast<size_t>(pos);
        const int64_t f = alignList[p];
        const ser::FloatImage luma = MeasurementLuma(src.ReadFrame(f), info);
        shifts[p] = aligner.Align(luma, stats[static_cast<size_t>(f)]);
        // Under the absolute floor a frame is excluded whatever the cut (AlignmentCut never goes lower):
        // its regions are not measured and it does not widen the residual scale.
        if (!(shifts[p].confidence >= kMinAlignConfidence)) return;
        measured[p] = measurer.Measure(luma, f, shifts[p]);
        hasRegions[p] = 1;
        observers->Observe(luma, measured[p]);
    });

    // The clean frames' cut; the suspects decided against it (Ruling P6). Each suspect is found in the
    // sorted alignList by index (Select's order of suspects is not relied on); suspectShifts follows
    // sel.suspects' order, as AdmitSuspects takes it.
    std::vector<Shift> clean, suspectShifts;
    std::vector<double> cleanConfidence;
    std::vector<uint8_t> isSuspect(static_cast<size_t>(nAlign), 0);
    if (std::adjacent_find(alignList.begin(), alignList.end()) != alignList.end())
        throw std::logic_error("RunStack: a frame is both selected and suspect");
    for (const Suspect& s : sel.suspects) {
        const auto it = std::lower_bound(alignList.begin(), alignList.end(), s.index);
        if (it == alignList.end() || *it != s.index) throw std::logic_error("RunStack: a suspect is missing from the alignment pass");
        const size_t p = static_cast<size_t>(it - alignList.begin());
        isSuspect[p] = 1;
        suspectShifts.push_back(shifts[p]);
    }
    for (size_t p = 0; p < alignList.size(); ++p)
        if (!isSuspect[p]) {
            clean.push_back(shifts[p]);
            cleanConfidence.push_back(shifts[p].confidence);
        }
    const double cut = AlignmentCut(clean);
    const Admission adm = AdmitSuspects(sel, suspectShifts, cut);
    sum.selection = adm.explanation;
    out.frames.admittedSuspects = adm.admitted;
    out.frames.rejectedSuspects = adm.rejected;
    std::vector<FrameRegions> kept;
    int64_t cleanAligned = 0;
    for (size_t p = 0; p < alignList.size(); ++p) {
        const bool pass = shifts[p].confidence >= cut;
        if (!isSuspect[p] && !pass) out.frames.alignmentExcluded.push_back(alignList[p]);
        if (!isSuspect[p] && pass) ++cleanAligned;
        const bool use = isSuspect[p] ? std::binary_search(adm.admitted.begin(), adm.admitted.end(), alignList[p]) : pass;
        if (!use) continue;
        // Passing the cut implies passing the floor, where the regions were measured.
        if (!hasRegions[p]) throw std::logic_error("RunStack: a frame over the alignment cut has no region measurement");
        kept.push_back(std::move(measured[p]));
    }
    measured = {};
    step("Aligning frames", t0);

    const int64_t need = std::max<int64_t>(1, std::min<int64_t>(kMinStackFrames, (n + 1) / 2));
    const int64_t nSelected = static_cast<int64_t>(sel.frames.size());
    const double medianMatch = Median(cleanConfidence);
    if (static_cast<int64_t>(kept.size()) < need) {
        return finish(Diagnosed(
            std::move(out), DiagnosisKind::Unaligned,
            "The frames do not line up: only " + Count(static_cast<int64_t>(kept.size())) + " of " +
                Count(nSelected + static_cast<int64_t>(sel.suspects.size())) +
                " selected frames match the reference well enough to be aligned (median match " + Num(medianMatch, 2) +
                ", needed " + Num(cut, 2) + ") -- the view shook or changed from frame to frame (wind, a knock, or "
                "not one steady target).",
            {{"frames selected", static_cast<double>(nSelected + static_cast<int64_t>(sel.suspects.size()))},
             {"frames aligned", static_cast<double>(kept.size())},
             {"median match", medianMatch},
             {"alignment cut", cut},
             {"frames needed", static_cast<double>(need)}}));
    }

    t0 = Clock::now();
    const RegionPlan plan = PlanRegions(layout, kept, mem.alignWorkers);
    const int64_t nKept = static_cast<int64_t>(kept.size());
    kept = {};
    step("Planning regions", t0);

    const int regions = static_cast<int>(plan.RegionCount());
    out.frames.regions = regions;
    out.frames.regionDetail = plan.detail;
    // The residual scale now, so its observers are gone before the focus measure holds its frames.
    const ser::FloatImage residualSigma = observers->Sigma(info.normDepth);
    observers.reset();

    // Region-stage focus (Rulings P4, P15): the detail the sharpest frames the stack keeps show on the target.
    {
        t0 = Clock::now();
        std::vector<size_t> ranked;  // positions in alignList of the frames kept, sharpest first
        for (size_t p = 0; p < alignList.size(); ++p) {
            const bool use = isSuspect[p] ? std::binary_search(adm.admitted.begin(), adm.admitted.end(), alignList[p])
                                          : shifts[p].confidence >= cut;
            if (use) ranked.push_back(p);
        }
        std::sort(ranked.begin(), ranked.end(), [&](size_t a, size_t b) {
            const double qa = stats[static_cast<size_t>(alignList[a])].quality, qb = stats[static_cast<size_t>(alignList[b])].quality;
            return qa != qb ? qa > qb : alignList[a] < alignList[b];
        });
        ranked.resize(std::min<size_t>(ranked.size(), kFocusFrames));
        std::vector<size_t> byIndex(ranked.size());
        for (size_t i = 0; i < ranked.size(); ++i) byIndex[i] = i;
        std::sort(byIndex.begin(), byIndex.end(), [&](size_t a, size_t b) { return alignList[ranked[a]] < alignList[ranked[b]]; });
        std::vector<ser::FloatImage> lumas(ranked.size());
        std::vector<Shift> focusShifts(ranked.size());
        {
            const std::unique_ptr<ser::FrameSource> src = factory();
            CheckReader(src.get(), info);
            int64_t done = refUnits + nAlign;
            for (size_t i : byIndex) {  // read forward
                lumas[i] = MeasurementLuma(src->ReadFrame(alignList[ranked[i]]), info);
                focusShifts[i] = shifts[ranked[i]];
                aligning.Report(++done);
                aligning.ThrowIfCancelled();
            }
        }
        // A disc's radius from its half-level crossing on the reference (blur-proof), not Select's box.
        const double discRadius = sel.fullFrameTarget ? 0.0 : DiscRadius(reference, refStats.cx, refStats.cy, sel.targetRadius);
        const std::function<void()> poll = [&aligning] { aligning.ThrowIfCancelled(); };
        FocusCheck& fc = sum.focus;
        if (sel.fullFrameTarget) {
            out.frames.focusWindows = FocusWindows(layout, scale);
            if (!out.frames.focusWindows.empty()) {
                out.frames.focus = MeasureFocus(lumas, focusShifts, out.frames.focusWindows, mem.alignWorkers, poll);
                fc = JudgeSurfaceFocus(out.frames.focus, scale);
            }
        } else {
            FocusProfile profile;
            FocusWindow window;
            fc = MeasureDiscFocus(lumas, focusShifts, refStats.cx, refStats.cy, discRadius, scale, mem.alignWorkers, poll,
                                  &profile, &window);
            out.frames.focusWindows = {window};
            if (window.side > 0) out.frames.focus = {profile};
        }
        lumas = {};
        aligning.Finish();
        aligning.ThrowIfCancelled();
        step("Judging focus", t0);
        // No region-stage verdict refuses (Ruling P20): a Warned check adds a summary warning when the
        // image is made; a gross defocus is refused only by Select (its disc rule) or found as NoTarget.
    }

    // ---- Stacking ------------------------------------------------------------------------------------
    t0 = Clock::now();
    Accumulator acc(plan, reference, residualSigma, info);
    std::vector<int64_t> stackList;
    for (size_t p = 0; p < plan.frames.size(); ++p)
        if (plan.stacked[p]) {
            stackList.push_back(plan.frames[p]);
            int keeping = 0;
            for (int r = 0; r < regions; ++r)
                keeping += (plan.flags[p * static_cast<size_t>(regions) + static_cast<size_t>(r)] & RegionPlan::kKept) != 0;
            out.frames.regionsKeeping.push_back(keeping);
        }
    Phase stacking(progress, kPhaseStacking, static_cast<int64_t>(stackList.size()));
    stacking.Report(0);
    ForFrames(static_cast<int64_t>(stackList.size()), mem.stackWorkers, factory, info, stacking, 0,
              [&](int, ser::FrameSource& src, int64_t pos) {
                  const int64_t f = stackList[static_cast<size_t>(pos)];
                  acc.Add(f, DemosaicHQ(src.ReadFrame(f), info.colorId, info.normDepth));
              });
    ser::FloatImage image = acc.Finish();
    stacking.ThrowIfCancelled();
    step("Stacking", t0);

    out.frames.stacked = stackList;
    sum.used = static_cast<int64_t>(stackList.size());
    sum.rejectedCells = acc.RejectedCells();

    // The alignment line.
    std::string line = "aligned " + Count(cleanAligned) + " of " + Count(nSelected) +
                       " selected frames to the reference (match median " + Num(medianMatch, 3) + ", cut " + Num(cut, 3) + ")";
    const int64_t excluded = static_cast<int64_t>(out.frames.alignmentExcluded.size());
    if (excluded > 0)
        line += "; " + Plural(excluded, "frame", "frames") + " could not be aligned (torn, shaken or not the target) and " +
                (excluded == 1 ? "was" : "were") + " left out";
    line += "; " + RegionPhrase(plan.detail, nKept, sum.used);
    const double cells = static_cast<double>(sum.used) * static_cast<double>(info.header.width / static_cast<int>(scale)) *
                         static_cast<double>(info.header.height / static_cast<int>(scale));
    line += "; " + Num(cells > 0 ? 100.0 * static_cast<double>(sum.rejectedCells) / cells : 0.0, 3) +
            "% of samples rejected as outliers; " + Plural(static_cast<int64_t>(sum.hotPixels), "hot pixel", "hot pixels") +
            " repaired";
    sum.alignment = line;

    // Only with an image: a diagnosed capture stacked nothing (Ruling P9's warning is about the result).
    if (UndeclaredMosaic(info)) sum.warnings.push_back(kUndeclaredMosaicWarning);
    if (sum.focus.outcome == FocusOutcome::Warned) sum.warnings.push_back(FocusWarning(sum.focus));
    out.image = std::move(image);
    return finish(std::move(out));
}

}  // namespace planetary::stack
