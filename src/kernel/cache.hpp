#pragma once

#include <proxima/reply.hpp>

#include <cstddef>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>

namespace proxima::detail {

/// A least-recently-used cache of replies, keyed on the Maxima source that
/// produced them.
///
/// Keying on the source text rather than on (operation, argument hashes) is
/// equivalent here and simpler: the source is rendered from canonical
/// expressions, so two calls that should share an answer produce identical
/// text, and two that should not cannot. It also catches repetition the
/// operation layer would not see, such as the same subexpression arriving from
/// two different callers.
///
/// Holds no opinion about *when* an entry stops being true — see
/// Kernel::evalPure and Kernel::invalidateCache for that, which is the part
/// that has to be right.
class ReplyCache {
public:
    explicit ReplyCache(std::size_t capacity) : capacity_(capacity) {}

    /// The cached reply for `key`, or nullptr. A hit is promoted to most
    /// recently used, and the pointer is valid until the next insert.
    const Reply *find(const std::string &key);

    void insert(std::string key, Reply reply);

    /// Forgets everything. Cheaper and far easier to reason about than trying
    /// to work out which entries a change in Maxima's state invalidated.
    void clear();

    std::size_t size() const { return entries_.size(); }
    std::size_t capacity() const { return capacity_; }
    std::size_t hits() const { return hits_; }
    std::size_t misses() const { return misses_; }

private:
    using Entry = std::pair<std::string, Reply>;
    using Position = std::list<Entry>::iterator;

    std::size_t capacity_;
    /// Most recently used at the front.
    std::list<Entry> entries_;
    std::unordered_map<std::string, Position> index_;

    std::size_t hits_ = 0;
    std::size_t misses_ = 0;
};

} // namespace proxima::detail
