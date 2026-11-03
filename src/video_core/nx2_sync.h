// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdlib>
#include <cstring>

namespace VideoCommon::Nx2Sync {

/// EDEN_NX2_SYNC=<letters>: opt-in serialization experiments (round 20). Empty: no change.
inline bool Has(char letter) {
    static const char* const flags = [] {
        const char* value = std::getenv("EDEN_NX2_SYNC");
        return value != nullptr ? value : "";
    }();
    return std::strchr(flags, letter) != nullptr;
}

/// EDEN_NX2_FILL_TRACE=1: log the first DMA constant fills.
inline bool FillTrace() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_FILL_TRACE");
        return value != nullptr && value[0] != '0';
    }();
    return enabled;
}

/// EDEN_NX2_HOST_BUFFERS=<dir>|1: host contents of the buffers the traced draws read (see buffer_cache.h).
inline const char* HostBuffers() {
    static const char* const value = [] {
        const char* env = std::getenv("EDEN_NX2_HOST_BUFFERS");
        return (env != nullptr && env[0] != '\0' && env[0] != '0') ? env : nullptr;
    }();
    return value;
}

} // namespace VideoCommon::Nx2Sync
