#include "kernel/persistent_cache.hpp"

#include <boost/process/v2/pid.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <fstream>
#include <string_view>
#include <iterator>
#include <charconv>
#include <random>
#include <system_error>
#include <vector>

namespace proxima::detail {
namespace {

/// Sixteen hex digits that name this process among every writer that may
/// share a cache directory: other threads here, other processes, and other
/// machines if the directory is on a share.
///
/// Random bits, mixed with the process id and the clock. The process id alone
/// is not enough across machines, and std::random_device is allowed to be
/// deterministic, or to throw, where no entropy source is available.
const std::string &process_token() {
    static const std::string token = [] {
        std::uint64_t bits = 0;
        try {
            std::random_device device;
            bits = (static_cast<std::uint64_t>(device()) << 32) ^ device();
        } catch (const std::exception &) {
            // No entropy source. The id and the clock still separate writers.
        }
        bits ^= static_cast<std::uint64_t>(boost::process::v2::current_pid())
                * std::uint64_t{0x9e3779b97f4a7c15ULL};
        bits ^= static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        bits ^= static_cast<std::uint64_t>(
            std::chrono::system_clock::now().time_since_epoch().count());
        return stable_hash(std::to_string(bits));
    }();
    return token;
}

/// Entries are length-prefixed, so a value or reason containing newlines needs
/// no escaping and a truncated file fails to parse rather than reading short.
void write_field(std::ostream &out, std::string_view text) {
    out << text.size() << '\n' << text;
}

/// The next line of `rest`, without its newline, consumed. False if there is
/// no newline, which for a well-formed entry cannot happen.
bool take_line(std::string_view &rest, std::string_view &line) {
    const std::size_t end = rest.find('\n');
    if (end == std::string_view::npos) {
        return false;
    }
    line = rest.substr(0, end);
    rest.remove_prefix(end + 1);
    return true;
}

/// One length-prefixed field, consumed from `rest`: its decimal length, a
/// newline, and that many bytes.
///
/// Parsed from the entry in memory. It used to be read from the stream with
/// three seeks per field to find how much of the file was left — twelve per
/// entry — which is what bounding the length needs, and which a string
/// already knows.
bool take_field(std::string_view &rest, std::string_view &field) {
    std::string_view length_text;
    if (!take_line(rest, length_text) || length_text.empty()) {
        return false;
    }
    std::size_t length = 0;
    const char *const last = length_text.data() + length_text.size();
    const auto [stopped, error] = std::from_chars(length_text.data(), last, length);
    if (error != std::errc{} || stopped != last) {
        return false;
    }
    // A field cannot be longer than what is left of the entry. The length used
    // to be trusted, so a corrupt or hostile entry claiming
    // 18446744073709551615 bytes made a resize throw std::length_error, which
    // escaped a question instead of reading as a miss.
    if (length > rest.size()) {
        return false;
    }
    field = rest.substr(0, length);
    rest.remove_prefix(length);
    return true;
}

constexpr const char *kFormat = "proxima-cache-1";

/// How old a temporary must be before a sweep takes it for an orphan. No
/// writer takes anything like this long to write one entry.
constexpr auto kOrphanAge = std::chrono::hours{1};

bool is_entry(const std::filesystem::path &path) {
    return path.extension() == ".reply";
}

/// `<entry>.reply.tmp-<token>-<n>`, or the `<entry>.reply.tmp<n>` of older
/// writers. Compared in the path's native encoding, so that a file with a name
/// outside the ANSI code page, which nothing here wrote, cannot make the
/// conversion to a narrow string throw.
bool is_temporary(const std::filesystem::path &path) {
    static const std::filesystem::path marker(".reply.tmp");
    return path.filename().native().find(marker.native())
           != std::filesystem::path::string_type::npos;
}

} // namespace

std::string stable_hash(std::string_view text) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001b3ULL;
    }

    std::string hex(16, '0');
    for (int i = 15; i >= 0; --i) {
        hex[static_cast<std::size_t>(i)] = "0123456789abcdef"[hash & 0xf];
        hash >>= 4;
    }
    return hex;
}

PersistentCache::PersistentCache(std::filesystem::path directory,
                                 std::string stamp, std::uintmax_t byte_limit)
    : directory_(std::move(directory)), stamp_(std::move(stamp)),
      byte_limit_(byte_limit) {
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    usable_ = std::filesystem::is_directory(directory_, ec);
}

void PersistentCache::restamp(std::string stamp) {
    stamp_ = std::move(stamp);
}

std::filesystem::path PersistentCache::entry_path(std::string_view source) const {
    return path_for(key_for(source));
}

void PersistentCache::sweep() const {
    struct Entry {
        std::filesystem::path path;
        std::uintmax_t size;
        std::filesystem::file_time_type used;
    };
    std::vector<Entry> entries;
    std::uintmax_t total = 0;
    const auto now = std::filesystem::file_time_type::clock::now();

    // Every failure below is skipped rather than reported: another process may
    // be renaming or deleting the very file being looked at, and a sweep that
    // misses a file this time will see it the next.
    std::error_code ec;
    for (std::filesystem::directory_iterator it(directory_, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code file_ec;
        if (!it->is_regular_file(file_ec)) {
            continue;
        }
        const auto modified = it->last_write_time(file_ec);
        if (file_ec) {
            continue;
        }
        if (is_temporary(it->path())) {
            if (now - modified > kOrphanAge) {
                std::filesystem::remove(it->path(), file_ec);
            }
            continue;
        }
        if (!is_entry(it->path())) {
            continue;
        }
        const std::uintmax_t size = it->file_size(file_ec);
        if (file_ec) {
            continue;
        }
        entries.push_back({it->path(), size, modified});
        total += size;
    }

    if (byte_limit_ != 0 && total > byte_limit_) {
        const std::uintmax_t target = byte_limit_ / 4 * 3;
        // A full sort, although only the oldest are removed: how many is not
        // known until their sizes are summed in order, and the sort is
        // nothing beside the directory walk and the stat of every file above.
        std::sort(entries.begin(), entries.end(),
                  [](const Entry &a, const Entry &b) { return a.used < b.used; });
        for (const Entry &entry : entries) {
            if (total <= target) {
                break;
            }
            std::error_code remove_ec;
            if (std::filesystem::remove(entry.path, remove_ec)) {
                total -= entry.size;
            }
        }
    }
    bytes_ = total;
}

std::string PersistentCache::key_for(std::string_view source) const {
    // The separator cannot occur in either part, so two different (stamp,
    // source) pairs cannot produce the same key material.
    return stamp_ + "\n\n" + std::string(source);
}

std::filesystem::path PersistentCache::path_for(const std::string &key) const {
    return directory_ / (stable_hash(key) + ".reply");
}

std::optional<Reply> PersistentCache::find(std::string_view source) const {
    if (!usable_) {
        return std::nullopt;
    }

    const std::string key = key_for(source);
    const std::filesystem::path path = path_for(key);
    // The whole entry in one read, then parsed from memory.
    std::string contents;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return std::nullopt;
        }
        contents.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    std::string_view rest = contents;
    std::string_view format;
    if (!take_line(rest, format) || format != kFormat) {
        return std::nullopt;
    }

    std::string_view stored_key;
    if (!take_field(rest, stored_key) || stored_key != key) {
        // Either a hash collision or a file from an incompatible writer.
        // Either way this is not the answer to the question being asked.
        return std::nullopt;
    }

    std::string_view ok;
    std::string_view value;
    std::string_view reason;
    if (!take_field(rest, ok) || !take_field(rest, value) || !take_field(rest, reason)) {
        return std::nullopt; // Truncated, most likely a partial write.
    }

    Reply reply;
    reply.ok = ok == "1";
    reply.value = std::string(value);
    reply.reason = std::string(reason);

    // A read is a use: it keeps the entry from eviction. After the stream has
    // closed, since Windows may refuse to change the time of a file held open.
    //
    // Only when the recorded use is at least an hour old, though. Eviction
    // needs to know roughly which entries are cold, not the exact order of the
    // last minute's reads, and a metadata write on every hit is a write to
    // disk on what should be a read — for a directory shared by many
    // processes, many writes.
    if (byte_limit_ != 0) {
        constexpr auto kRefreshAge = std::chrono::hours{1};
        std::error_code ec;
        const auto now = std::filesystem::file_time_type::clock::now();
        const auto used = std::filesystem::last_write_time(path, ec);
        if (ec || now - used >= kRefreshAge) {
            std::filesystem::last_write_time(path, now, ec);
        }
    }
    return reply;
}

std::filesystem::path temporary_path_for(const std::filesystem::path &target) {
    // Named by this counter alone, as it used to be, the first write in every
    // process was <entry>.tmp0. Two processes writing one entry at once opened
    // the same file, one truncated the other's half-written content, and a
    // corrupt entry could be renamed into place. The reader rejected it, so it
    // came back as a miss — but only thanks to the length-prefixed format.
    static std::atomic<std::uint64_t> counter{0};

    // Appended to the path itself, not to target.string(): that round trip
    // goes through the ANSI code page on Windows, and a cache directory with a
    // character outside it would come back as a different, or no, path.
    std::filesystem::path temporary = target;
    temporary += ".tmp-" + process_token() + "-"
                 + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
    return temporary;
}

void PersistentCache::insert(std::string_view source, const Reply &reply) const {
    if (!usable_) {
        return;
    }

    const std::string key = key_for(source);
    const std::filesystem::path target = path_for(key);

    // Written to a temporary of this writer's own and renamed into place, so a
    // reader never sees a half-written entry, and two writers of the same entry
    // race only over which identical copy is renamed last.
    const std::filesystem::path temporary = temporary_path_for(target);

    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            return;
        }
        out << kFormat << '\n';
        write_field(out, key);
        write_field(out, reply.ok ? "1" : "0");
        write_field(out, reply.value);
        write_field(out, reply.reason);
        if (!out) {
            out.close();
            std::error_code ec;
            std::filesystem::remove(temporary, ec);
            return;
        }
    }

    std::error_code ec;
    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        // Losing a cache entry is not worth reporting; the next call simply
        // asks Maxima again.
        std::filesystem::remove(temporary, ec);
        return;
    }

    if (byte_limit_ == 0) {
        return;
    }
    if (!bytes_) {
        // The first write learns the directory's size, sweeping if a previous
        // run left it over the limit.
        sweep();
        return;
    }
    // Counted as an addition even when it replaced an entry of the same key:
    // an overestimate, which the sweep it may bring forward corrects.
    const std::uintmax_t written = std::filesystem::file_size(target, ec);
    *bytes_ += ec ? 0 : written;
    if (*bytes_ > byte_limit_) {
        sweep();
    }
}

} // namespace proxima::detail
