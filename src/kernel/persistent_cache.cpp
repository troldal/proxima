#include "kernel/persistent_cache.hpp"

#include <boost/process/v2/pid.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <fstream>
#include <random>
#include <system_error>
#include <vector>

namespace mx::detail {
namespace {

/// Sixteen hex digits that name this process among every writer that may
/// share a cache directory: other threads here, other processes, and other
/// machines if the directory is on a share.
///
/// Random bits, mixed with the process id and the clock. The process id alone
/// is not enough across machines, and std::random_device is allowed to be
/// deterministic, or to throw, where no entropy source is available.
const std::string &processToken() {
    static const std::string token = [] {
        std::uint64_t bits = 0;
        try {
            std::random_device device;
            bits = (static_cast<std::uint64_t>(device()) << 32) ^ device();
        } catch (const std::exception &) {
            // No entropy source. The id and the clock still separate writers.
        }
        bits ^= static_cast<std::uint64_t>(boost::process::v2::current_pid())
                * 0x9e3779b97f4a7c15ULL;
        bits ^= static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        bits ^= static_cast<std::uint64_t>(
            std::chrono::system_clock::now().time_since_epoch().count());
        return stableHash(std::to_string(bits));
    }();
    return token;
}

/// Entries are length-prefixed, so a value or reason containing newlines needs
/// no escaping and a truncated file fails to parse rather than reading short.
void writeField(std::ostream &out, std::string_view text) {
    out << text.size() << '\n' << text;
}

bool readField(std::istream &in, std::string &text) {
    std::size_t length = 0;
    if (!(in >> length)) {
        return false;
    }
    if (in.get() != '\n') {
        return false;
    }

    // A field cannot be longer than what is left of the file. The length used
    // to be trusted, so a corrupt or hostile entry claiming
    // 18446744073709551615 bytes made the resize below throw
    // std::length_error, which escaped evalPure instead of reading as a miss.
    const std::streampos here = in.tellg();
    in.seekg(0, std::ios::end);
    const std::streampos end = in.tellg();
    in.seekg(here);
    if (here < 0 || end < here
        || static_cast<std::uint64_t>(length)
               > static_cast<std::uint64_t>(end - here)) {
        return false;
    }

    text.resize(length);
    return length == 0 || static_cast<bool>(in.read(text.data(),
                                                    static_cast<std::streamsize>(
                                                        length)));
}

constexpr const char *kFormat = "maxima_cpp-cache-1";

/// How old a temporary must be before a sweep takes it for an orphan. No
/// writer takes anything like this long to write one entry.
constexpr auto kOrphanAge = std::chrono::hours{1};

bool isEntry(const std::filesystem::path &path) {
    return path.extension() == ".reply";
}

/// `<entry>.reply.tmp-<token>-<n>`, or the `<entry>.reply.tmp<n>` of older
/// writers. Compared in the path's native encoding, so that a file with a name
/// outside the ANSI code page, which nothing here wrote, cannot make the
/// conversion to a narrow string throw.
bool isTemporary(const std::filesystem::path &path) {
    static const std::filesystem::path marker(".reply.tmp");
    return path.filename().native().find(marker.native())
           != std::filesystem::path::string_type::npos;
}

} // namespace

std::string stableHash(std::string_view text) {
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
                                 std::string stamp, std::uintmax_t byteLimit)
    : directory_(std::move(directory)), stamp_(std::move(stamp)),
      byteLimit_(byteLimit) {
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    usable_ = std::filesystem::is_directory(directory_, ec);
}

void PersistentCache::restamp(std::string stamp) {
    stamp_ = std::move(stamp);
}

std::filesystem::path PersistentCache::entryPath(std::string_view source) const {
    return pathFor(keyFor(source));
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
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc)) {
            continue;
        }
        const auto modified = it->last_write_time(fileEc);
        if (fileEc) {
            continue;
        }
        if (isTemporary(it->path())) {
            if (now - modified > kOrphanAge) {
                std::filesystem::remove(it->path(), fileEc);
            }
            continue;
        }
        if (!isEntry(it->path())) {
            continue;
        }
        const std::uintmax_t size = it->file_size(fileEc);
        if (fileEc) {
            continue;
        }
        entries.push_back({it->path(), size, modified});
        total += size;
    }

    if (byteLimit_ != 0 && total > byteLimit_) {
        const std::uintmax_t target = byteLimit_ / 4 * 3;
        std::sort(entries.begin(), entries.end(),
                  [](const Entry &a, const Entry &b) { return a.used < b.used; });
        for (const Entry &entry : entries) {
            if (total <= target) {
                break;
            }
            std::error_code removeEc;
            if (std::filesystem::remove(entry.path, removeEc)) {
                total -= entry.size;
            }
        }
    }
    bytes_ = total;
}

std::string PersistentCache::keyFor(std::string_view source) const {
    // The separator cannot occur in either part, so two different (stamp,
    // source) pairs cannot produce the same key material.
    return stamp_ + "\n\n" + std::string(source);
}

std::filesystem::path PersistentCache::pathFor(const std::string &key) const {
    return directory_ / (stableHash(key) + ".reply");
}

std::optional<Reply> PersistentCache::find(std::string_view source) const {
    if (!usable_) {
        return std::nullopt;
    }

    const std::string key = keyFor(source);
    const std::filesystem::path path = pathFor(key);
    Reply reply;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return std::nullopt;
        }

        std::string format;
        if (!std::getline(in, format) || format != kFormat) {
            return std::nullopt;
        }

        std::string storedKey;
        if (!readField(in, storedKey) || storedKey != key) {
            // Either a hash collision or a file from an incompatible writer.
            // Either way this is not the answer to the question being asked.
            return std::nullopt;
        }

        std::string ok;
        std::string value;
        std::string reason;
        if (!readField(in, ok) || !readField(in, value) || !readField(in, reason)) {
            return std::nullopt; // Truncated, most likely a partial write.
        }

        reply.ok = ok == "1";
        reply.value = std::move(value);
        reply.reason = std::move(reason);
    }

    // A read is a use: it keeps the entry from eviction. After the stream has
    // closed, since Windows may refuse to change the time of a file held open.
    if (byteLimit_ != 0) {
        std::error_code ec;
        std::filesystem::last_write_time(
            path, std::filesystem::file_time_type::clock::now(), ec);
    }
    return reply;
}

std::filesystem::path temporaryPathFor(const std::filesystem::path &target) {
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
    temporary += ".tmp-" + processToken() + "-"
                 + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
    return temporary;
}

void PersistentCache::insert(std::string_view source, const Reply &reply) const {
    if (!usable_) {
        return;
    }

    const std::string key = keyFor(source);
    const std::filesystem::path target = pathFor(key);

    // Written to a temporary of this writer's own and renamed into place, so a
    // reader never sees a half-written entry, and two writers of the same entry
    // race only over which identical copy is renamed last.
    const std::filesystem::path temporary = temporaryPathFor(target);

    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            return;
        }
        out << kFormat << '\n';
        writeField(out, key);
        writeField(out, reply.ok ? "1" : "0");
        writeField(out, reply.value);
        writeField(out, reply.reason);
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

    if (byteLimit_ == 0) {
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
    if (*bytes_ > byteLimit_) {
        sweep();
    }
}

} // namespace mx::detail
