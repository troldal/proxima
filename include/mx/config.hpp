#pragma once

#include <chrono>
#include <filesystem>

namespace mx {

/// Settings for a Kernel.
struct Config {
    /// How long to wait for a single statement to produce its result before
    /// giving up on it.
    ///
    /// Generous by default: some integrals legitimately take a long time, and
    /// until PLAN.md step 13 adds restart-and-replay, exceeding this leaves the
    /// session unusable rather than recovering it. Treat it as a backstop
    /// against a wedged child, not as a cancellation mechanism.
    std::chrono::milliseconds timeout{std::chrono::minutes{2}};

    /// Root of the Maxima installation.
    ///
    /// Hard-coded for now, exactly as the prototype had it. PLAN.md step 5
    /// replaces the default with real discovery (explicit path -> MAXIMA_ROOT
    /// -> known install locations -> PATH), at which point an empty value
    /// means "find it yourself".
    std::filesystem::path maximaRoot = "C:\\maxima-5.50.0";
};

} // namespace mx
