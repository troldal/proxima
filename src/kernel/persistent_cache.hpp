#pragma once

#include "kernel/reply.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace proxima::detail {

/// A cache of replies that outlives the process, stored as one file per entry.
///
/// ## What has to be in the key
///
/// An in-memory cache can be blunt about invalidation: throw it all away
/// whenever anything might have changed (see ReplyCache). A cache shared
/// between processes and across time cannot, because the thing that changed may
/// have happened in another process entirely. Everything an answer depends on
/// has to be *in the key*:
///
/// - **The Maxima version.** A later Maxima may simplify differently, and an
///   answer it never gave should not be attributed to it.
/// - **This library's version.** The stored value is Maxima's internal
///   s-expression, read back by a mapping that could change; and the key is
///   source text rendered by a printer that could change too.
/// - **The assumption state.** This is the one that is easy to miss and
///   unsound to omit. `sqrt(x^2)` is `abs(x)` normally and `x` under
///   `assume(x > 0)`. A process that cached the second would otherwise hand it
///   to a process that never made the assumption.
///
/// The key is hashed to name the file, and stored *inside* it as well, so a
/// hash collision is detected rather than silently answered wrongly.
///
/// ## Size
///
/// With a byte limit, the directory is kept under it by deleting the least
/// recently used entries: recency is a file's modification time, which a read
/// refreshes. Over the limit, entries go oldest first until the directory is at
/// three quarters of it, so a full cache is not swept again on the very next
/// write. The size is learned by scanning the directory on the first write,
/// and tracked from there. Each process enforces the limit on what it sees, so
/// several sharing a directory can overshoot it between their sweeps.
///
/// A sweep also removes temporaries older than an hour, left by a writer that
/// died mid-write. Younger ones may belong to a writer still at work.
///
/// ## Concurrency
///
/// One file per entry, written to a temporary and renamed into place. The
/// temporary's name is unique to the writer (see temporary_path_for), so two
/// processes writing the same entry never share a half-written file; they race
/// only over which of two identical entries is renamed into place last. No
/// locking, no index to corrupt, and nothing to flush at exit — an entry is
/// durable as soon as it is written. A sweep deleting an entry another process
/// is reading costs that process a miss, nothing worse.
///
/// Within a process, the tracked size is unsynchronised state: calls on one
/// PersistentCache must not overlap. MaximaSession makes them under its state
/// lock.
class PersistentCache {
public:
    /// `directory` is created if needed. `stamp` is the version and state
    /// material every key is qualified by. `byte_limit` caps the directory's
    /// entries; zero means no limit, and no sweeping at all.
    PersistentCache(std::filesystem::path directory, std::string stamp,
                    std::uintmax_t byte_limit = 0);

    std::optional<Reply> find(std::string_view source) const;
    void insert(std::string_view source, const Reply &reply) const;

    /// Qualifies later keys by a new stamp — the assumption state changed —
    /// keeping what is known of the directory's size, so that a change of
    /// assumptions does not cost a rescan.
    void restamp(std::string stamp);

    /// False if the directory could not be created, in which case find and
    /// insert do nothing rather than throwing on every call.
    bool usable() const { return usable_; }

    const std::filesystem::path &directory() const { return directory_; }

    /// The file an entry for `source` lives in under the current stamp.
    /// Exposed for testing.
    std::filesystem::path entry_path(std::string_view source) const;

private:
    std::string key_for(std::string_view source) const;
    std::filesystem::path path_for(const std::string &key) const;

    /// Deletes orphaned temporaries and, over the limit, the least recently
    /// used entries; records the entries' total size.
    void sweep() const;

    std::filesystem::path directory_;
    std::string stamp_;
    std::uintmax_t byte_limit_ = 0;
    bool usable_ = false;

    /// Bytes of entries in the directory, as far as this object knows. Unknown
    /// until the first write scans for it.
    mutable std::optional<std::uintmax_t> bytes_;
};

/// 64-bit FNV-1a, rendered as 16 hex digits.
///
/// Not std::hash: that varies between standard libraries, and a cache on disk
/// outlives the build that wrote it.
std::string stable_hash(std::string_view text);

/// Where PersistentCache::insert writes an entry before renaming it to
/// `target`: beside it, as `<target>.tmp-<token>-<n>`.
///
/// The token is random per process, mixed with the process id and the clock,
/// and `n` counts writes within the process, so no other writer — another
/// thread, another process, another machine sharing the directory — picks the
/// same name. Exposed for testing.
std::filesystem::path temporary_path_for(const std::filesystem::path &target);

} // namespace proxima::detail
