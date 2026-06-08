#pragma once

#include "nukex/stretch/stretch_op.hpp"
#include <vector>
#include <memory>
#include <string>

namespace nukex {

/// Ordered pipeline of stretch operations.
///
/// Sorts enabled ops by position, executes sequentially. Each op
/// receives the output of the previous op. All ops work on float32 [0,1].
class StretchPipeline {
public:
    std::vector<std::unique_ptr<StretchOp>> ops;

    /// Execute all enabled ops in position order.
    void execute(Image& img) const;

    /// Advisory ordering warnings. Non-blocking.
    /// Returns warning messages if the current ordering is suboptimal.
    std::vector<std::string> check_ordering() const;

    /// Quick preview stretch — arcsinh on a COPY, never modifies working data.
    /// The working data stays linear throughout. This is a hard rule.
    static Image quick_preview_stretch(const Image& linear_src, float alpha = 500.f);
};

} // namespace nukex
