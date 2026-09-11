#pragma once

#include <chrono>
#include <filesystem>

namespace mx {

/// Settings for a Kernel.
struct Config {
    /// Root of the Maxima installation, e.g. C:\maxima-5.50.0
    ///
    /// Empty means "discover it": Config::maximaRoot, then $MAXIMA_ROOT, then
    /// $MAXIMA_PREFIX, then the parent of any $PATH entry named "bin", then the
    /// conventional install locations. A root given here is still validated;
    /// discovery throws KernelError naming everything it tried.
    std::filesystem::path maximaRoot;

    /// How long to wait for a single statement to produce its result before
    /// giving up on it.
    ///
    /// Generous by default: some integrals legitimately take a long time, and
    /// until PLAN.md step 13 adds restart-and-replay, exceeding this leaves the
    /// session unusable rather than recovering it. Treat it as a backstop
    /// against a wedged child, not as a cancellation mechanism.
    std::chrono::milliseconds timeout{std::chrono::minutes{2}};

    /// Whether to let Maxima load the user's maxima-init.mac at startup.
    ///
    /// Off by default, and deliberately so. Maxima reads that file from
    /// $MAXIMA_USERDIR (by default ~/maxima), where wxMaxima creates one as a
    /// matter of course. Anything in it — a redefined function, a changed
    /// simplification flag — would silently alter this library's results on one
    /// machine and not another. A library should compute the same answer
    /// everywhere, so the user directory is pointed at userDir instead.
    ///
    /// Set true to opt back into the user's own Maxima configuration.
    bool loadUserInit = false;

    /// Where Maxima keeps user state when loadUserInit is false.
    ///
    /// Empty means a directory under the system temporary directory, created on
    /// demand. Ignored entirely when loadUserInit is true, in which case Maxima
    /// falls back to its own default.
    std::filesystem::path userDir;
};

} // namespace mx
