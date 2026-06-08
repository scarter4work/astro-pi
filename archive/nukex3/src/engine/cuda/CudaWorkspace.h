// src/engine/cuda/CudaWorkspace.h
// Per-thread workspace layout for GPU pixel selection.
// Replaces fixed-size stack arrays (old MAX_SUBS=64) with dynamically-sized
// global memory workspaces, removing any limit on sub count.
#pragma once

#include <cstddef>
#include <cstdint>

namespace nukex {
namespace cuda {

// Workspace layout computed on the host, passed to the kernel.
// All offsets are byte offsets from the slot base pointer.
struct WorkspaceLayout {
    size_t bytesPerSlot;

    // Persistent double arrays
    size_t off_zValues;         // double[nSubs]
    size_t off_preFiltered;     // double[nSubs]
    size_t off_cleanData;       // double[nSubs]
    size_t off_sortedClean;     // double[nSubs]

    // Scratch double arrays (reused across non-concurrent phases)
    size_t off_scratch_d1;      // double[nSubs]
    size_t off_scratch_d2;      // double[nSubs]
    size_t off_scratch_d3;      // double[nSubs]

    // Persistent int arrays
    size_t off_preFilteredIdx;  // int[nSubs]
    size_t off_cleanIdx;        // int[nSubs]
    size_t off_sortedCleanIdx;  // int[nSubs]

    // Scratch int arrays
    size_t off_scratch_i1;      // int[nSubs]
    size_t off_scratch_i2;      // int[nSubs]

    // Mask pre-filter index
    size_t off_validIdx;        // int[nSubs] — maps compact index → original frame index

    // Bool arrays
    size_t off_madOutlier;      // bool[nSubs]
    size_t off_esdOutlier;      // bool[nSubs]
    size_t off_allOutlier;      // bool[nSubs]
};

// Compute workspace layout for a given sub count.
inline WorkspaceLayout computeWorkspaceLayout(int nSubs)
{
    WorkspaceLayout w{};
    size_t n = static_cast<size_t>(nSubs);
    size_t off = 0;

    // Double zone (7 arrays)
    w.off_zValues      = off; off += n * sizeof(double);
    w.off_preFiltered  = off; off += n * sizeof(double);
    w.off_cleanData    = off; off += n * sizeof(double);
    w.off_sortedClean  = off; off += n * sizeof(double);
    w.off_scratch_d1   = off; off += n * sizeof(double);
    w.off_scratch_d2   = off; off += n * sizeof(double);
    w.off_scratch_d3   = off; off += n * sizeof(double);

    // Int zone (5 arrays) — align to 4 bytes (already aligned after doubles)
    w.off_preFilteredIdx  = off; off += n * sizeof(int);
    w.off_cleanIdx        = off; off += n * sizeof(int);
    w.off_sortedCleanIdx  = off; off += n * sizeof(int);
    w.off_scratch_i1      = off; off += n * sizeof(int);
    w.off_scratch_i2      = off; off += n * sizeof(int);

    // Mask pre-filter index (1 array)
    w.off_validIdx        = off; off += n * sizeof(int);

    // Bool zone (3 arrays)
    w.off_madOutlier   = off; off += n * sizeof(bool);
    w.off_esdOutlier   = off; off += n * sizeof(bool);
    w.off_allOutlier   = off; off += n * sizeof(bool);

    // Align total to 8 bytes for clean slot boundaries
    w.bytesPerSlot = (off + 7) & ~size_t(7);
    return w;
}

// Device-side helper: get a typed pointer from workspace base + offset.
#ifdef __CUDACC__
template<typename T>
__device__ __forceinline__ T* wsPtr(char* base, size_t offset)
{
    return reinterpret_cast<T*>(base + offset);
}
#endif

} // namespace cuda
} // namespace nukex
