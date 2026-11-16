#pragma once

#include "nukex/core/frame_metadata.hpp"
#include "nukex/core/progress_observer.hpp"
#include "nukex/fitting/huber_estimator.hpp"
#include "nukex/fitting/model_selector.hpp"
#include "nukex/io/image.hpp"

#include <string>
#include <vector>

namespace nukex {

/// What a calibration master is.
///
/// DARK_FLAT is a dark taken at the flats' exposure: it removes the flats'
/// pedestal and dark signal the way a dark does a light's.
enum class MasterKind { BIAS, DARK, DARK_FLAT, FLAT };

/// "bias", "dark", "dark-flat", "flat".
const char* master_kind_name(MasterKind kind);

/// The header values that decide whether a calibration frame fits a light.
///
/// Built from FrameMetadata's raw calibration fields, never from its
/// interpreted ones (`gain` is e-/ADU, and 0.0 means "absent" in
/// `temperature` and `exposure`, which is also a real reading).
struct CalibrationHeader {
    std::string image_type;      ///< IMAGETYP as written
    std::string instrument;      ///< INSTRUME as written
    std::string bayer_pattern;   ///< BAYERPAT; empty for mono
    int    width = 0, height = 0, channels = 1;
    int    xbin = 1, ybin = 1;
    bool   has_exposure = false;    double exposure = 0.0;      ///< seconds
    bool   has_temperature = false; double temperature = 0.0;   ///< degrees C
    bool   temperature_is_setpoint = false;  ///< SET-TEMP stood in for a missing CCD-TEMP
    bool   has_gain = false;        double gain = 0.0;          ///< GAIN as written
    bool   has_offset = false;      double offset = 0.0;        ///< OFFSET as written
    /// The divisor that put the data on [0, 1] (FITSReader::read_fixed_scale):
    /// 65535 for unsigned 16-bit camera data. 0 when only the header has been
    /// read and the type is float, whose scale the pixels decide.
    double full_scale = 0.0;

    static CalibrationHeader from_metadata(const FrameMetadata& m, int channels,
                                           double full_scale);

    /// "ZWO ASI2400MC Pro 6072x4042 bin 1x1 RGGB, 300 s, gain 0, offset 30, -10.0 C"
    std::string describe() const;
};

/// A master bias, dark, dark-flat or flat, in memory.
///
/// `data` is on the fixed [0, 1] scale of the frames it came from
/// (`header.full_scale`), raw CFA layout for a colour camera -- calibration
/// happens before debayer. A master flat is additionally divided by its own
/// fitted level, so it is about 1.
struct MasterFrame {
    MasterKind        kind = MasterKind::BIAS;
    CalibrationHeader header;      ///< exposure/temperature: the set's own (fitted) values
    Image             data;
    int               n_frames = 0;
    double            temperature_min = 0.0, temperature_max = 0.0;
    std::string       estimator;   ///< "Huber" or "model race"
    /// Provenance hash of the inputs (paths, sizes, modification times,
    /// estimator, builder format and -- for a flat -- the pedestal's own
    /// signature). A cached master is reused only when this matches.
    std::string       signature;
    std::string       pedestal;    ///< flat: what was subtracted from each flat (description)
    /// Flat: the pedestal plan's description, exactly as the engine reports it
    /// in ExecuteResult::flat_pedestal when it builds the flat itself.
    std::string       pedestal_plan;
    /// Flat: the pedestal's provenance (LightCalibrator::pedestal_signature):
    /// which masters it came from, checked against the set it is used with.
    std::string       pedestal_signature;
    std::vector<std::string> sources;   ///< input file names
    std::string       path;        ///< the file it was read from or written to, if any

    bool empty() const { return data.empty(); }
};

/// Builds master calibration frames by fitting, per pixel, the distribution
/// of the input frames' values -- the stacker's own estimator, on all N
/// values, with no frame and no pixel rejected.
///
/// Why not mean or median: the master is an estimate of each pixel's
/// expected value, and the estimator that does that for the stack does it
/// here. A cosmic-ray hit in one dark is an outlier of that pixel's
/// distribution, and Huber gives it a weight of delta/|r| instead of either
/// averaging it in or cutting it out. A hot pixel is not an outlier -- it is
/// high in every frame -- so it stays in the master and is subtracted.
///
/// Inputs are read on the fixed full scale of their type
/// (FITSReader::read_fixed_scale), so 16-bit data goes through the frame
/// cache losslessly. Every input must agree on camera, geometry, CFA
/// pattern, binning, gain, offset and exposure; the set's temperatures must
/// lie within `temperature_spread_c`. A frame that does not is an error that
/// names it -- never a frame quietly dropped.
class MasterBuilder {
public:
    struct Config {
        enum class Estimator { HUBER, MODEL_RACE };
        Estimator              estimator = Estimator::HUBER;
        HuberEstimator::Config huber;
        ModelSelector::Config  race;
        std::string            cache_dir = "/tmp";   ///< frame cache (unlinked temp file)
        double                 exposure_rel_tolerance = 0.001;
        double                 temperature_spread_c   = 2.0;
        /// Frames read at once. Each 24 MP frame holds ~200 MB while it is
        /// read and checked; <= 0 picks min(hardware threads, 8).
        int                    max_read_workers = 0;
        int                    min_frames = 3;   ///< fewer cannot be fitted
    };

    struct Result {
        bool        ok = false;
        std::string error;
        MasterFrame master;
        bool        reused = false;   ///< build_or_load found a matching cached file
    };

    /// Build a master from frame files.
    ///
    /// For MasterKind::FLAT, `pedestal` is what each flat contains besides
    /// light -- a master dark-flat, or a master bias, already matched to the
    /// flats by the caller -- and is subtracted from every flat before the
    /// flat is scaled to unit level and fitted. `pedestal_label` describes it
    /// for the provenance record, `pedestal_signature` identifies it in the
    /// signature. Ignored for the other kinds.
    static Result build(MasterKind kind, const std::vector<std::string>& paths,
                        const Config& config,
                        const Image* pedestal = nullptr,
                        const std::string& pedestal_label = {},
                        const std::string& pedestal_signature = {},
                        ProgressObserver* progress = nullptr);

    /// The cached master at `cache_path` if it was built from exactly these
    /// inputs (signature match); otherwise build, write it there, and return
    /// it. A cached file that cannot be read or whose signature differs is
    /// rebuilt and replaced, and the reason is reported to `progress`.
    static Result build_or_load(MasterKind kind, const std::vector<std::string>& paths,
                                const std::string& cache_path, const Config& config,
                                const Image* pedestal = nullptr,
                                const std::string& pedestal_label = {},
                                const std::string& pedestal_signature = {},
                                ProgressObserver* progress = nullptr);

    /// The provenance signature build() would give these inputs.
    static std::string signature(MasterKind kind, const std::vector<std::string>& paths,
                                 const Config& config,
                                 const std::string& pedestal_signature = {});

    /// Write a master as float32 FITS with keywords recording what it is and
    /// what it was made from. Atomic (temp file + rename).
    static bool write(const MasterFrame& master, const std::string& path, std::string& error);

    /// Read a master written by write(). Refuses a file that is not one.
    static Result read(const std::string& path);
};

} // namespace nukex
