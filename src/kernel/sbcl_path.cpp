#include "kernel/discovery.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#endif

// Kept apart from discovery.cpp so that <windows.h> reaches one translation
// unit and no further.

namespace mx::detail {

#ifdef _WIN32
namespace {

bool isAscii(const std::wstring &text) {
    for (const wchar_t c : text) {
        if (c >= 0x80) {
            return false;
        }
    }
    return true;
}

} // namespace
#endif

std::filesystem::path sbclReadablePath(const std::filesystem::path &path) {
#ifdef _WIN32
    // Measured against the SBCL 2.6.7 that ships with Maxima 5.50: launched
    // with a core under a directory named "mæxima_中文", the runtime reports
    //
    //     could not open file "...\mx_m\xe6xima_??\...\maxima.core"
    //
    // — the ANSI code page's rendering of the name, with the CJK characters
    // already lost as '?' — and exits. With the executable there instead, it
    // starts but warns that *RUNTIME-PATHNAME* could not be decoded. Nothing
    // else is affected: SBCL reads the environment in full Unicode, and
    // Maxima loads files from a non-ASCII MAXIMA_PREFIX without complaint.
    //
    // A short name is plain ASCII, so the runtime can open the file by it.
    // ASCII paths are left alone so that messages and SBCL's own
    // *CORE-PATHNAME* keep the names a person would recognise.
    if (isAscii(path.native())) {
        return path;
    }
    const DWORD needed = GetShortPathNameW(path.c_str(), nullptr, 0);
    if (needed == 0) {
        return path;
    }
    std::wstring buffer(needed, L'\0');
    const DWORD written = GetShortPathNameW(path.c_str(), buffer.data(), needed);
    if (written == 0 || written >= needed) {
        return path;
    }
    buffer.resize(written);
    // A volume with short-name generation turned off returns the long name for
    // every component that has no short one.
    if (!isAscii(buffer)) {
        return path;
    }
    return std::filesystem::path(std::move(buffer));
#else
    return path;
#endif
}

} // namespace mx::detail
