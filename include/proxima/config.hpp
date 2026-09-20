#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace proxima {

/// @addtogroup kernel
/// @{

/// How far a Kernel may go looking for Maxima, beyond what Config names.
///
/// Each step includes the ones before it. Config::sbcl_exe, Config::maxima_core
/// and Config::maxima_root are consulted whichever this is: they are what the
/// program itself says, not searching.
enum class Search {
    /// What Config names, and nothing else. A program that ships its own
    /// Maxima, or one that must never run another, wants this: if what it
    /// named is not there, that is an error rather than a reason to run some
    /// other installation.
    Configured,
    /// And what the environment says: $MAXIMA_ROOT, $MAXIMA_PREFIX, and the
    /// parent of any $PATH entry named "bin".
    Environment,
    /// And the conventional install locations — on Windows, a directory named
    /// like Maxima under C:\ or Program Files; elsewhere /usr/local, /usr and
    /// /opt. What makes a plain installation work with no configuration.
    Automatic,
};

/// Settings for a Kernel.
struct Config {
    /// Root of the Maxima installation, e.g. `C:\maxima-5.50.0`.
    ///
    /// Empty means "discover it": Config::maxima_root, then $MAXIMA_ROOT, then
    /// $MAXIMA_PREFIX, then the parent of any $PATH entry named "bin", then the
    /// conventional install locations. A root given here is still validated;
    /// discovery throws KernelError naming everything it tried.
    ///
    /// Where the core lies under a root is asked of the installation, by
    /// running `<root>/bin/maxima -d`, rather than inferred from the layout;
    /// a copy with no launcher falls back to
    /// `<root>/lib[64]/maxima/<version>/binary-sbcl/maxima.core`. To leave
    /// nothing to discovery at all, see Config::sbcl_exe.
    std::filesystem::path maxima_root;

    /// The SBCL executable and the Maxima core to launch, named exactly.
    ///
    /// Set both and nothing is inferred: no layout is assumed, no directory
    /// is searched, and no launcher is consulted — Proxima runs the two files
    /// named here. That is what an application shipping its own copy of
    /// Maxima wants, an installer knowing where it put things, and anyone who
    /// would rather say than have the library guess.
    ///
    /// Both or neither: one without the other is a KernelError, as is either
    /// one not naming a file. maxima_root may be set alongside them, and is
    /// then used only as the prefix Maxima is told about; left empty, the
    /// prefix is taken to be the directory above SBCL's.
    std::filesystem::path sbcl_exe;
    std::filesystem::path maxima_core; ///< See Config::sbcl_exe.

    /// How far to look beyond what this Config names, when it names nothing
    /// usable. Automatic by default, which is what makes an ordinary
    /// installation work without configuration; Search::Configured refuses to
    /// look at all.
    Search search = Search::Automatic;

    /// How long to wait for a single statement to produce its result before
    /// giving up on it.
    ///
    /// Generous by default, since some integrals legitimately take a long time.
    /// Exceeding it costs the call but not the session: the Maxima process is
    /// ended — nothing is left running — a fresh one is started, and its
    /// assumptions are replayed. Treat it as a backstop against a wedged child
    /// rather than as cancellation: Maxima cannot be interrupted short of ending
    /// the process, so every timeout also pays for a Maxima startup, bounded by
    /// startup_timeout.
    std::chrono::milliseconds timeout{std::chrono::minutes{2}};

    /// How long to allow for starting Maxima and restoring session state.
    ///
    /// Separate from `timeout`, and deliberately so. Launching a Lisp image is
    /// not a computation, and a caller who wants a one-second deadline on their
    /// integrals should not thereby make the kernel unstartable — nor should a
    /// timeout leave recovery unable to run because the very deadline that was
    /// just exceeded also governs the restart.
    std::chrono::milliseconds startup_timeout{std::chrono::seconds{30}};

    /// How many replies to remember.
    ///
    /// A round trip costs milliseconds and a cache hit costs nanoseconds, and
    /// symbolic work asks the same questions repeatedly — the same derivative
    /// while searching, the same subexpression from two callers. Zero disables
    /// caching entirely.
    std::size_t cache_entries = 4096;

    /// The most memory the remembered replies may occupy, counting their text.
    ///
    /// A limit on entries alone does not bound memory: one reply can be close
    /// to a megabyte (`expand((x+y+z)^120)`), and 4096 of those is gigabytes.
    /// The least recently used replies go first once either limit is reached,
    /// and a reply larger than this on its own is not remembered at all.
    std::size_t cache_bytes = std::size_t{64} * 1024 * 1024;

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
    /// that change is not part of the key; Kernel::persistence_active says
    /// whether that has happened, and Kernel::restart undoes it.
    ///
    /// Entries are trusted as they are read: nothing signs or checks them. So
    /// the directory must be writable only by people whose answers you would
    /// accept — anyone who can write a file there can make `integrate` return
    /// whatever they like.
    std::filesystem::path cache_directory;

    /// The most cache_directory may hold, in bytes, before the least recently
    /// used answers are deleted. Zero means no limit.
    ///
    /// Recency is each entry's modification time, which reading it back
    /// refreshes when the recorded use is an hour old or more. Over the limit,
    /// entries are deleted oldest first until the directory is at three quarters of
    /// it. Each process enforces the limit on what it sees, so processes sharing a
    /// directory can overshoot it briefly.
    std::uintmax_t cache_directory_limit = std::uintmax_t{256} * 1024 * 1024;

    /// Whether to let Maxima load the user's maxima-init.mac at startup.
    ///
    /// Off by default, and deliberately so. Maxima reads that file from
    /// $MAXIMA_USERDIR (by default ~/maxima), where wxMaxima creates one as a
    /// matter of course. Anything in it — a redefined function, a changed
    /// simplification flag — would silently alter this library's results on one
    /// machine and not another. A library should compute the same answer
    /// everywhere, so the user directory is pointed at user_dir instead.
    ///
    /// Set true to opt back into the user's own Maxima configuration.
    bool load_user_init = false;

    /// Where Maxima keeps user state when load_user_init is false.
    ///
    /// Empty means a directory private to the current user under the system
    /// temporary directory, created on demand. On Unix that is
    /// `<tmp>/proxima-<uid>/userdir`, created with mode 0700; if it already
    /// exists but is a link, belongs to another user or is open to others, the
    /// kernel refuses to start rather than use it. Ignored entirely when
    /// load_user_init is true, in which case Maxima falls back to its own
    /// default.
    ///
    /// A directory given here is used as it is. Maxima executes the
    /// maxima-init.mac it finds in it, so it must not be writable by anyone
    /// you would not let run code as you.
    std::filesystem::path user_dir;
};

/// @}

} // namespace proxima
