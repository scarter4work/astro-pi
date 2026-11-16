#pragma once

#include "nukex/core/cancel_poll.hpp"
#include "nukex/core/parallel_for.hpp"

namespace nukex {

/// body(i) for every i in [0, count), on worker threads, stopping early on a
/// cancel. False when a cancel stopped it (some indices then never ran).
///
/// With a CancelPoll the calling thread does no work itself: it asks the
/// observer every 20 ms until the workers are done (parallel_for_polled), and
/// every worker reads the flag before each index. A loop that polled only
/// from worker 0, between its own indices, went unwatched once worker 0 ran
/// out of work -- under memory pressure the other workers' last rows of a
/// file-backed record took seconds to finish.
///
/// Without one it is parallel_for_dynamic. Each index must touch only its own
/// outputs, so the result does not depend on scheduling.
template <typename Body>
bool cancellable_for(int count, int chunk, CancelPoll* cancel, Body&& body) {
    if (!cancel) {
        parallel_for_dynamic(count, chunk, [&](int i, int) { body(i); });
        return true;
    }
    parallel_for_polled(count, chunk, 0,
        [&](int i, int) {
            if (cancel->cancelled()) return;
            body(i);
        },
        [&] { cancel->check(); });
    return !cancel->cancelled();
}

} // namespace nukex
