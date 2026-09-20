#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace proxima::detail {

/// Which Maxima context each set of assumptions has, least recently used
/// first to go.
///
/// A set of assumptions is named by a key — its canonical text — and given a
/// Maxima context of its own the first time it is asked under. This table
/// remembers which context that was, promotes a set each time it is used, and
/// says which contexts to kill once there are more than `capacity`. It knows
/// nothing of Maxima: making, switching to and killing the contexts is
/// MaximaSession's conversation, and this is the bookkeeping beside it.
///
/// Names are the table's own, so they can travel to Maxima as text without
/// escaping: `proxima_a1`, `proxima_a2`, and so on.
class ContextTable {
public:
    explicit ContextTable(std::size_t capacity) : capacity_(capacity) {}

    /// The context made for `key`, now the most recently used; nothing if
    /// none has been made, or it has since been evicted.
    std::optional<std::string> find(const std::string &key) {
        const auto found = index_.find(key);
        if (found == index_.end()) {
            return std::nullopt;
        }
        entries_.splice(entries_.begin(), entries_, found->second);
        return found->second->second;
    }

    /// A name for a context about to be made, never one handed out before.
    std::string next_name() { return "proxima_a" + std::to_string(++next_); }

    /// Records `name` as the context for `key`, most recently used, and hands
    /// back the names evicted to stay within capacity — never the one just
    /// recorded, which is at the front.
    std::vector<std::string> insert(std::string key, std::string name) {
        entries_.emplace_front(std::move(key), std::move(name));
        index_.emplace(entries_.front().first, entries_.begin());
        std::vector<std::string> evicted;
        while (entries_.size() > capacity_) {
            auto &[victim_key, victim] = entries_.back();
            evicted.push_back(std::move(victim));
            index_.erase(victim_key);
            entries_.pop_back();
        }
        return evicted;
    }

    /// Forgets every context: what a restart does, the contexts having died
    /// with the process. The names are not reused.
    void clear() {
        entries_.clear();
        index_.clear();
    }

    std::size_t size() const { return entries_.size(); }
    std::size_t capacity() const { return capacity_; }

private:
    using Entry = std::pair<std::string, std::string>; ///< (key, name)

    std::size_t capacity_;
    std::list<Entry> entries_; ///< Most recently used at the front.
    std::unordered_map<std::string, std::list<Entry>::iterator> index_;
    std::uint64_t next_ = 0;
};

} // namespace proxima::detail
