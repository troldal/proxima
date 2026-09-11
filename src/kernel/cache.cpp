#include "kernel/cache.hpp"

namespace mx::detail {

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

void ReplyCache::insert(std::string key, Reply reply) {
    if (capacity_ == 0) {
        return;
    }

    if (const auto found = index_.find(key); found != index_.end()) {
        found->second->second = std::move(reply);
        entries_.splice(entries_.begin(), entries_, found->second);
        return;
    }

    entries_.emplace_front(key, std::move(reply));
    index_.emplace(std::move(key), entries_.begin());

    while (entries_.size() > capacity_) {
        index_.erase(entries_.back().first);
        entries_.pop_back();
    }
}

void ReplyCache::clear() {
    entries_.clear();
    index_.clear();
}

} // namespace mx::detail
