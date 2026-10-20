#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace planetary::ser {

// SER timestamps: 100-ns ticks since 0001-01-01 UTC, top two bits are flags.
// Returns "YYYY-MM-DDThh:mm:ss.fff", or nullopt when the masked value is <= 0.
std::optional<std::string> TicksToIsoUtc(int64_t ticks);

}  // namespace planetary::ser
