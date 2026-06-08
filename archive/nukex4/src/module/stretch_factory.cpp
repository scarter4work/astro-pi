#include "stretch_factory.hpp"
#include "stretch_auto_selector.hpp"

#include "nukex/stretch/veralux_stretch.hpp"
#include "nukex/stretch/ghs_stretch.hpp"
#include "nukex/stretch/mtf_stretch.hpp"
#include "nukex/stretch/arcsinh_stretch.hpp"
#include "nukex/stretch/log_stretch.hpp"
#include "nukex/stretch/lupton_stretch.hpp"
#include "nukex/stretch/clahe_stretch.hpp"

#include <sstream>

namespace nukex {

static void phase8_apply(StretchOp& op,
                         const Phase8Context& ctx,
                         std::string& log_line) {
    if (ctx.loader == nullptr || ctx.stats == nullptr) {
        log_line += " | Phase 8: inactive (no context)";
        return;
    }
    auto active = ctx.loader->active_for_stretch(op.name);
    if (active.layer == ActiveLayer::None || active.model == nullptr) {
        log_line += " | Phase 8: " + active.description + " -> factory defaults";
        return;
    }
    const bool applied = active.model->predict_and_apply(*ctx.stats, op);
    std::ostringstream oss;
    oss << " | Phase 8: " << active.description << (applied ? " applied" : " no-op");
    log_line += oss.str();
}

std::unique_ptr<StretchOp> build_primary(PrimaryStretch e,
                                         const FITSMetadata& meta,
                                         std::string& out_log_line,
                                         const Phase8Context* p8) {
    out_log_line.clear();
    std::unique_ptr<StretchOp> op;

    switch (e) {
        case PrimaryStretch::Auto: {
            // Pass the full metadata to select_auto so the log line can
            // include the FITS values (FILTER/BAYERPAT/NAXIS3) that drove
            // the classification — useful when a user is debugging an
            // unexpected auto-selection.
            auto sel = select_auto(meta);
            out_log_line = std::move(sel.log_line);
            op = std::move(sel.op);
            if (op && p8 != nullptr) {
                phase8_apply(*op, *p8, out_log_line);
            }
            return op;
        }
        case PrimaryStretch::VeraLux: return std::make_unique<VeraLuxStretch>();
        case PrimaryStretch::GHS:     return std::make_unique<GHSStretch>();
        case PrimaryStretch::MTF:     return std::make_unique<MTFStretch>();
        case PrimaryStretch::ArcSinh: return std::make_unique<ArcSinhStretch>();
        case PrimaryStretch::Log:     return std::make_unique<LogStretch>();
        case PrimaryStretch::Lupton:  return std::make_unique<LuptonStretch>();
        case PrimaryStretch::CLAHE:   return std::make_unique<CLAHEStretch>();
    }
    return nullptr;
}

std::unique_ptr<StretchOp> build_finishing(FinishingStretch /*e*/) {
    return nullptr;
}

} // namespace nukex
