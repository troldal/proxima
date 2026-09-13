#include "kernel/persistent_cache.hpp"

#include <atomic>
#include <cstdint>
#include <fstream>
#include <system_error>

namespace mx::detail {
namespace {

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
    text.resize(length);
    return length == 0 || static_cast<bool>(in.read(text.data(),
                                                    static_cast<std::streamsize>(
                                                        length)));
}

constexpr const char *kFormat = "maxima_cpp-cache-1";

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
                                 std::string stamp)
    : directory_(std::move(directory)), stamp_(std::move(stamp)) {
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    usable_ = std::filesystem::is_directory(directory_, ec);
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
    std::ifstream in(pathFor(key), std::ios::binary);
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

    Reply reply;
    reply.ok = ok == "1";
    reply.value = std::move(value);
    reply.reason = std::move(reason);
    return reply;
}

void PersistentCache::insert(std::string_view source, const Reply &reply) const {
    if (!usable_) {
        return;
    }

    const std::string key = keyFor(source);
    const std::filesystem::path target = pathFor(key);

    // Written to a unique temporary and renamed into place, so a reader never
    // sees a half-written entry and two writers race only to produce identical
    // content.
    static std::atomic<std::uint64_t> counter{0};
    // Appended to the path itself, not to target.string(): that round trip
    // goes through the ANSI code page on Windows, and a cache directory with a
    // character outside it would come back as a different, or no, path.
    std::filesystem::path temporary = target;
    temporary += ".tmp"
                 + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));

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
    }
}

} // namespace mx::detail
