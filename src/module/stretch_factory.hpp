#ifndef __NukeX_stretch_factory_h
#define __NukeX_stretch_factory_h

#include "fits_metadata.hpp"
#include "nukex/stretch/stretch_op.hpp"
#include "nukex/stretch/image_stats.hpp"
#include "nukex/stretch/layer_loader.hpp"

#include <memory>
#include <string>

namespace nukex {

// Library-layer enum that mirrors the PCL NXPrimaryStretch enum in
// NukeXParameters.h (Task A6). Values MUST stay synchronized:
// Auto=0, VeraLux=1, GHS=2, MTF=3, ArcSinh=4, Log=5, Lupton=6, CLAHE=7.
enum class PrimaryStretch {
    Auto = 0, VeraLux, GHS, MTF, ArcSinh, Log, Lupton, CLAHE,
};

enum class FinishingStretch {
    None = 0,
};

// NEW: optional Phase 8 context. When loader is null, behaviour is identical
// to pre-Phase-8 -- factory defaults only.
struct Phase8Context {
    const LayerLoader* loader      = nullptr;
    const ImageStats*  stats       = nullptr;  // for this stack, linear
};

std::unique_ptr<StretchOp> build_primary(PrimaryStretch e,
                                         const FITSMetadata& meta,
                                         std::string& out_log_line,
                                         const Phase8Context* p8 = nullptr);

std::unique_ptr<StretchOp> build_finishing(FinishingStretch e);

} // namespace nukex

#endif
