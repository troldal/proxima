#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace proxima {

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
    /// Exceeding it costs the call but not the session: the Maxima process is
    /// ended — nothing is left running — a fresh one is started, and its
    /// assumptions are replayed. Treat it as a backstop against a wedged child
    /// rather than as cancellation: Maxima cannot be interrupted short of ending
    /// the process, so every timeout also pays for a Maxima startup, bounded by
    /// startupTimeout.
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

    /// The most memory the remembered replies may occupy, counting their text.
    ///
    /// A limit on entries alone does not bound memory: one reply can be close
    /// to a megabyte (`expand((x+y+z)^120)`), and 4096 of those is gigabytes.
    /// The least recently used replies go first once either limit is reached,
    /// and a reply larger than this on its own is not remembered at all.
    std::size_t cacheBytes = std::size_t{64} * 1024 * 1024;

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

    /// The most cacheDirectory may hold, in bytes, before the least recently
    /// used answers are deleted. Zero means no limit.
    ///
    /// Recency is each entry's modification time, which reading it back
    /// refreshes. Over the limit, entries are deleted oldest first until the
    /// directory is at three quarters of it. Each process enforces the limit on
    /// what it sees, so processes sharing a directory can overshoot it briefly.
    std::uintmax_t cacheDirectoryLimit = std::uintmax_t{256} * 1024 * 1024;

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
    /// Empty means a directory private to the current user under the system
    /// temporary directory, created on demand. On Unix that is
    /// `<tmp>/proxima-<uid>/userdir`, created with mode 0700; if it already
    /// exists but is a link, belongs to another user or is open to others, the
    /// kernel refuses to start rather than use it. Ignored entirely when
    /// loadUserInit is true, in which case Maxima falls back to its own
    /// default.
    ///
    /// A directory given here is used as it is. Maxima executes the
    /// maxima-init.mac it finds in it, so it must not be writable by anyone
    /// you would not let run code as you.
    std::filesystem::path userDir;
};

} // namespace proxima
