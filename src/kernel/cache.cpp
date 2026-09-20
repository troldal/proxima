#include "kernel/cache.hpp"

namespace proxima::detail {

const Reply *ReplyCache::find(const std::string &key) {
    const auto found = index_.find(key);
    if (found == index_.end()) {
        ++misses_;
        return nullptr;
    }
    ++hits_;
    entries_.splice(entries_.begin(), entries_, found->second);
    return &entries_.front().second;
}

std::size_t ReplyCache::footprint(const std::string &key, const Reply &reply) {
    constexpr std::size_t kOverhead = 256;
    return 2 * key.size() + reply.value().size() + reply.reason().size() + kOverhead;
}

void ReplyCache::insert(std::string key, Reply reply) {
    if (capacity_ == 0) {
        return;
    }

    const std::size_t cost = footprint(key, reply);
    const auto found = index_.find(key);

    // Too large to hold at all. Remembering it would only evict everything
    // else and then itself. Any older answer under the same key goes too,
    // rather than being left behind a newer one that was not kept.
    if (cost > byte_limit_) {
        if (found != index_.end()) {
            bytes_ -= footprint(key, found->second->second);
            entries_.erase(found->second);
            index_.erase(found);
        }
        return;
    }

    if (found != index_.end()) {
        bytes_ -= footprint(key, found->second->second);
        found->second->second = std::move(reply);
        entries_.splice(entries_.begin(), entries_, found->second);
    } else {
        entries_.emplace_front(key, std::move(reply));
        index_.emplace(std::move(key), entries_.begin());
    }
    bytes_ += cost;
    evict();
}

void ReplyCache::evict() {
    while (entries_.size() > capacity_ || bytes_ > byte_limit_) {
        const Entry &oldest = entries_.back();
        bytes_ -= footprint(oldest.first, oldest.second);
        index_.erase(oldest.first);
        entries_.pop_back();
    }
}

void ReplyCache::clear() {
    entries_.clear();
    index_.clear();
    bytes_ = 0;
}

} // namespace proxima::detail
