#pragma once

#include <filesystem>
#include <new>
#include <optional>
#include <string>
#include <string_view>

// Inside the library, every std::string that holds a path, a command-line
// argument or an environment value is UTF-8, on every platform.
//
// UTF-8 is what Boost.Process assumes when it hands narrow strings to the wide
// Windows API, so it is the only encoding that survives a launch intact. It is
// not what std::filesystem::path::string() produces on Windows: that is the
// ANSI code page, which holds a few hundred characters at most, so a Maxima
// installed under a path outside it was mangled on the way to SBCL, and with
// MSVC's standard library string() throws instead.
//
// The conversions below go through path's UTF-8 members, so the transcoding is
// the standard library's; these functions only move bytes between char8_t and
// char. On POSIX a path is bytes already and they amount to copies.

namespace proxima::detail {

/// A path as UTF-8.
///
/// Throws for a Windows path that is not valid Unicode — NTFS allows a name
/// holding a lone UTF-16 surrogate. Use tryToUtf8 where such a path can turn
/// up, for instance while listing a directory someone else owns.
inline std::string toUtf8(const std::filesystem::path &path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

/// The path named by UTF-8 text. Throws if the text is not valid UTF-8 and the
/// platform has to transcode it.
inline std::filesystem::path pathFromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

/// toUtf8, or nothing when the path cannot be represented.
///
/// What the standard library throws for that differs between implementations —
/// std::system_error from some, std::range_error from others — so anything but
/// running out of memory counts.
inline std::optional<std::string>
tryToUtf8(const std::filesystem::path &path) {
    try {
        return toUtf8(path);
    } catch (const std::bad_alloc &) {
        throw;
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

/// pathFromUtf8, or nothing when the text is not valid UTF-8.
inline std::optional<std::filesystem::path>
tryPathFromUtf8(std::string_view text) {
    try {
        return pathFromUtf8(text);
    } catch (const std::bad_alloc &) {
        throw;
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

/// A path for an error message: UTF-8, and never an exception of its own in
/// place of the error being reported.
inline std::string describePath(const std::filesystem::path &path) {
    return tryToUtf8(path).value_or("<a path that is not valid Unicode>");
}

} // namespace proxima::detail
