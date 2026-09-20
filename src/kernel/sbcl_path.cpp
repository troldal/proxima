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

namespace proxima::detail {

#ifdef _WIN32
namespace {

bool is_ascii(const std::wstring &text) {
    for (const wchar_t c : text) {
        if (c >= 0x80) {
            return false;
        }
    }
    return true;
}

} // namespace
#endif

CoreSpelling core_spelling(const std::filesystem::path &core) {
    // A short name, where the volume has them, keeps the command line
    // self-contained and the child's working directory the caller's.
    const std::filesystem::path readable = sbcl_readable_path(core);
#ifdef _WIN32
    if (!is_ascii(readable.native())) {
        // No short name to be had — short-name generation is off for this
        // volume, as it is for most volumes that are not the system one. The
        // runtime reads its command line through the ANSI code page, but the
        // *working directory* reaches the child as wide text through
        // CreateProcessW, and a name resolved against it never passes through
        // that conversion. So run in the core's own directory and name the
        // file alone: "maxima.core" is ASCII whatever the path above it is.
        //
        // Measured against SBCL 2.6.7: with the core under a directory named
        // "proxima-探索-test", `--core <full path>` fails with "could not open
        // file ... open: Invalid argument", and `--core maxima.core` with that
        // directory as the working directory loads it. The executable itself
        // may sit under such a path either way: it is launched wide.
        std::filesystem::path name = core.filename();
        if (is_ascii(name.native())) {
            return {std::move(name), core.parent_path()};
        }
        // The file's own name is not ASCII either. Nothing here can spell it
        // for the runtime; the caller gets the path as it is, and SBCL will
        // report what it could not open.
    }
#endif
    return {readable, {}};
}

std::filesystem::path sbcl_readable_path(const std::filesystem::path &path) {
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
    if (is_ascii(path.native())) {
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
    if (!is_ascii(buffer)) {
        return path;
    }
    return std::filesystem::path(std::move(buffer));
#else
    return path;
#endif
}

} // namespace proxima::detail
