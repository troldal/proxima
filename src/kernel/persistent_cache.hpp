#pragma once

#include <mx/reply.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace mx::detail {

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
/// ## Concurrency
///
/// One file per entry, written to a temporary and renamed into place, so two
/// processes writing the same entry race only to produce identical content. No
/// locking, no index to corrupt, and nothing to flush at exit — an entry is
/// durable as soon as it is written.
class PersistentCache {
public:
    /// `directory` is created if needed. `stamp` is the version and state
    /// material every key is qualified by.
    PersistentCache(std::filesystem::path directory, std::string stamp);

    std::optional<Reply> find(std::string_view source) const;
    void insert(std::string_view source, const Reply &reply) const;

    /// False if the directory could not be created, in which case find and
    /// insert do nothing rather than throwing on every call.
    bool usable() const { return usable_; }

    const std::filesystem::path &directory() const { return directory_; }

private:
    std::string keyFor(std::string_view source) const;
    std::filesystem::path pathFor(const std::string &key) const;

    std::filesystem::path directory_;
    std::string stamp_;
    bool usable_ = false;
};

/// 64-bit FNV-1a, rendered as 16 hex digits.
///
/// Not std::hash: that varies between standard libraries, and a cache on disk
/// outlives the build that wrote it.
std::string stableHash(std::string_view text);

} // namespace mx::detail
