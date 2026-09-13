#pragma once

#include <chrono>
#include <cstddef>
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
    /// Generous by default, since some integrals legitimately take a long time.
    /// Exceeding it costs the call but not the session: the kernel is restarted
    /// and its assumptions replayed. Treat it as a backstop against a wedged
    /// child, not as a cancellation mechanism — Maxima keeps computing until it
    /// is killed.
    std::chrono::milliseconds timeout{std::chrono::minutes{2}};

    /// How long to allow for starting Maxima and restoring session state.
    ///
    /// Separate from `timeout`, and deliberately so. Launching a Lisp image is
    /// not a computation, and a caller who wants a one-second deadline on their
    /// integrals should not thereby make the kernel unstartable — nor should a
    /// timeout leave recovery unable to run because the very deadline that was
    /// just exceeded also governs the restart.
    std::chrono::milliseconds startupTimeout{std::chrono::seconds{30}};

    /// How many replies to remember.
    ///
    /// A round trip costs milliseconds and a cache hit costs nanoseconds, and
    /// symbolic work asks the same questions repeatedly — the same derivative
    /// while searching, the same subexpression from two callers. Zero disables
    /// caching entirely.
    std::size_t cacheEntries = 4096;

    /// Where to keep replies between runs. Empty means do not.
    ///
    /// Off by default: writing files somewhere is not a thing a library should
    /// start doing unasked. When set, answers survive process exit and are
    /// shared between processes using the same directory.
    ///
    /// Every key is qualified by the Maxima version, this library's version and
    /// the kernel's assumption state, so an entry can only ever be read back
    /// under the conditions that produced it. Persistence switches itself off
    /// for a kernel whose state has been changed by a raw Kernel::eval, since
    /// that change is not part of the key; Kernel::persistenceActive says
    /// whether that has happened, and Kernel::restart undoes it.
    std::filesystem::path cacheDirectory;

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
