#pragma once

#include <filesystem>

namespace mx {

/// Settings for a Kernel.
struct Config {
    /// Root of the Maxima installation.
    ///
    /// Hard-coded for now, exactly as the prototype had it. PLAN.md step 5
    /// replaces the default with real discovery (explicit path -> MAXIMA_ROOT
    /// -> known install locations -> PATH), at which point an empty value
    /// means "find it yourself".
    std::filesystem::path maximaRoot = "C:\\maxima-5.50.0";
};

} // namespace mx
