// Discovery and launch-recipe tests. The precedence chain runs against an
// injected environment rather than the real one, so these need no Maxima and
// cannot be perturbed by whatever the developer happens to have installed.

#include <doctest/doctest.h>

#include "kernel/discovery.hpp"
#include "kernel/session.hpp"
#include "util/utf8.hpp"

#include <proxima/config.hpp>
#include <proxima/errors.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

using proxima::detail::candidate_roots;
using proxima::detail::EnvLookup;
using proxima::detail::inspect_root;
using proxima::detail::MaximaInstall;
using proxima::detail::MaximaSession;

namespace {

#ifdef _WIN32
constexpr char kPathSep = ';';
constexpr const char *kPrefix = "C:/";
constexpr const char *kSbclName = "sbcl.exe";
constexpr const char *kNotAnInstall = "C:/Windows";
#else
constexpr char kPathSep = ':';
constexpr const char *kPrefix = "/";
constexpr const char *kSbclName = "sbcl";
constexpr const char *kNotAnInstall = "/etc";
#endif

/// An absolute path spelled the same way on both platforms: forward slashes
/// under a platform-appropriate prefix. std::filesystem::path accepts forward
/// slashes on Windows too, so the precedence tests need no per-platform
/// expectations — only the PATH separator genuinely differs.
std::filesystem::path abs(std::string_view relative) {
    return std::filesystem::path(std::string(kPrefix) + std::string(relative));
}

/// Joins entries into a PATH-style variable using the platform's separator.
std::string path_var(std::initializer_list<const char *> entries) {
    std::string joined;
    for (const char *entry : entries) {
        if (!joined.empty()) {
            joined.push_back(kPathSep);
        }
        joined += abs(entry).string();
    }
    return joined;
}

/// An environment built from a literal map, so precedence is tested without
/// touching the process's own variables.
EnvLookup fake_env(std::map<std::string, std::string, std::less<>> vars) {
    return [vars = std::move(vars)](
               std::string_view name) -> std::optional<std::string> {
        if (const auto it = vars.find(name); it != vars.end()) {
            return it->second;
        }
        return std::nullopt;
    };
}

/// An install description that touches no filesystem.
MaximaInstall fake_install(bool raise_dynamic_space_size = true) {
    MaximaInstall install;
    install.root = abs("maxima-5.50.0");
    install.sbcl_exe = abs("maxima-5.50.0") / "bin" / kSbclName;
    install.maxima_core = abs("maxima-5.50.0") / "lib" / "maxima" / "5.50.0"
                         / "binary-sbcl" / "maxima.core";
    install.version_tag = "5.50.0";
    install.raise_dynamic_space_size = raise_dynamic_space_size;
    return install;
}

std::string joined(const std::vector<std::string> &argv) {
    std::string all;
    for (const std::string &arg : argv) {
        all += arg;
        all += '\n';
    }
    return all;
}

} // namespace

TEST_CASE("candidate roots follow the documented precedence") {
    proxima::Config config;
    config.maxima_root = abs("explicit");

    const auto roots = candidate_roots(
        config,
        fake_env({{"MAXIMA_ROOT", abs("from_root_var").string()},
                 {"MAXIMA_PREFIX", abs("from_prefix_var").string()},
                 {"PATH", path_var({"somewhere", "maxima-x/bin", "other"})}}));

    REQUIRE(roots.size() == 4);
    CHECK(roots[0] == abs("explicit"));
    CHECK(roots[1] == abs("from_root_var"));
    CHECK(roots[2] == abs("from_prefix_var"));
    // Maxima's launcher lives at <root>/bin, so the PATH entry contributes its
    // parent.
    CHECK(roots[3] == abs("maxima-x"));
}

TEST_CASE("an unset Config falls through to the environment") {
    const auto roots = candidate_roots(
        proxima::Config{}, fake_env({{"MAXIMA_ROOT", abs("env").string()}}));
    REQUIRE(roots.size() == 1);
    CHECK(roots.front() == abs("env"));
}

TEST_CASE("only PATH entries named bin become candidates") {
    // Otherwise every directory on PATH would be probed.
    const auto roots = candidate_roots(
        proxima::Config{},
        fake_env({{"PATH", path_var({"somewhere", "tools", "apps/maxima/bin"})}}));
    REQUIRE(roots.size() == 1);
    CHECK(roots.front() == abs("apps/maxima"));
}

TEST_CASE("the PATH separator is the platform's own") {
    // ';' on Windows, ':' on POSIX. Getting this wrong silently produces one
    // nonsensical candidate instead of several real ones.
    const auto roots = candidate_roots(
        proxima::Config{}, fake_env({{"PATH", path_var({"a/bin", "b/bin"})}}));
    REQUIRE(roots.size() == 2);
    CHECK(roots[0] == abs("a"));
    CHECK(roots[1] == abs("b"));
}

TEST_CASE("duplicate candidates are collapsed") {
    proxima::Config config;
    config.maxima_root = abs("same");
    const auto roots
        = candidate_roots(config, fake_env({{"MAXIMA_ROOT", abs("same").string()},
                                          {"MAXIMA_PREFIX", abs("same").string()}}));
    CHECK(roots.size() == 1);
}

TEST_CASE("an empty environment yields no candidates") {
    CHECK(candidate_roots(proxima::Config{}, fake_env({})).empty());
}

TEST_CASE("a directory that is not a Maxima install is rejected") {
    CHECK_FALSE(inspect_root(kNotAnInstall).has_value());
    CHECK_FALSE(inspect_root(abs("definitely/not/here")).has_value());
}

TEST_CASE("an explicit but wrong root is an error, not a reason to search on") {
    // Falling through to the search here would silently run whatever Maxima
    // happened to be installed elsewhere, turning a mistyped path into
    // surprising results rather than a diagnosable failure.
    proxima::Config config;
    config.maxima_root = abs("nowhere/at/all");

    try {
        proxima::detail::discover_maxima(config, fake_env({}));
        FAIL("expected discovery to reject an unusable explicit root");
    } catch (const proxima::KernelError &e) {
        const std::string message = e.what();
        // An error that does not say what was wrong is not actionable.
        CHECK(message.find(abs("nowhere/at/all").string()) != std::string::npos);
        CHECK(message.find("maxima.core") != std::string::npos);
        // And it names this platform's executable, not always the Windows one.
        CHECK(message.find(std::string("/bin/") + kSbclName + " ") != std::string::npos);
#ifndef _WIN32
        CHECK(message.find("sbcl.exe") == std::string::npos);
#endif
    }
}

TEST_CASE("discovery with nothing configured names the locations it tried") {
    // No explicit root, and an environment pointing somewhere useless. This
    // still consults the conventional install locations, so on a machine with
    // Maxima installed the search legitimately succeeds.
    proxima::Config config;
    try {
        const auto install = proxima::detail::discover_maxima(
            config,
            fake_env({{"MAXIMA_ROOT", abs("nowhere/at/all").string()}}));
        // If it succeeded it must have produced something coherent.
        CHECK(std::filesystem::is_regular_file(install.sbcl_exe));
        CHECK(std::filesystem::is_regular_file(install.maxima_core));
        CHECK_FALSE(install.version_tag.empty());
    } catch (const proxima::KernelError &e) {
        const std::string message = e.what();
        CHECK(message.find("Tried:") != std::string::npos);
    }
}

TEST_CASE("launch command wires the core and installs the Lisp helper") {
    const std::vector<std::string> argv
        = MaximaSession::launch_command(fake_install());

    REQUIRE_FALSE(argv.empty());
    CHECK(argv.front()
          == proxima::detail::to_utf8(abs("maxima-5.50.0") / "bin" / kSbclName));

    const std::string text = joined(argv);
    CHECK(text.find("maxima.core") != std::string::npos);
    CHECK(text.find("--noinform") != std::string::npos);
    CHECK(text.find("--end-toplevel-options") != std::string::npos);

    // The Lisp helper that does the framing must actually be installed, and it
    // must run Maxima's toplevel afterwards.
    CHECK(text.find("$cppsend") != std::string::npos);
    CHECK(text.find("cl-user::run") != std::string::npos);
}

TEST_CASE("the Lisp helper's delimiters agree with the ones C++ looks for") {
    // The format string lives in Lisp and the matching lives in C++, so they
    // can drift apart silently — the symptom would be every reply timing out.
    // Both sides derive from the same literal shape, and this pins that down.
    const std::string lisp = joined(MaximaSession::launch_command(fake_install()));

    // The helper formats the tag — key and id — with ~a, so strip the tag
    // from each delimiter and look for the surrounding literal text.
    const auto strip_tag = [](std::string delimiter) {
        const std::string tag = "k3y-7";
        const size_t at = delimiter.find(tag);
        REQUIRE(at != std::string::npos);
        return std::pair{delimiter.substr(0, at), delimiter.substr(at + tag.size())};
    };

    for (const auto &[prefix, suffix] :
         {strip_tag(MaximaSession::frame_begin("k3y", 7)),
          strip_tag(MaximaSession::frame_separator("k3y", 7)),
          strip_tag(MaximaSession::frame_end("k3y", 7))}) {
        CHECK(lisp.find(prefix + "~a" + suffix) != std::string::npos);
    }
}

TEST_CASE("a request is wrapped so errors become values") {
    const std::string request = MaximaSession::request_for(
        "k3y", 42, proxima::detail::Payload::text("integrate(x, 5)"));

    // The tag as a string, which the helper prints with ~a and so without its
    // quotes: exactly the spelling frame_begin("k3y", 42) looks for.
    CHECK(request.find("cppsend(\"k3y-42\",") != std::string::npos);
    // errcatch is what stops a Maxima error leaving the stream in an error
    // prompt; ratdisrep keeps canonical rational (MRAT) forms from coming back.
    CHECK(request.find("errcatch(") != std::string::npos);
    CHECK(request.find("ratdisrep(") != std::string::npos);
    // The expression travels as a string literal for Maxima to parse inside
    // the trap, never as bare syntax.
    CHECK(request.find("eval_string(\"integrate(x, 5)\")") != std::string::npos);
    // '$' rather than ';': the wrapper prints the frame itself and Maxima
    // should print nothing of its own.
    CHECK(request.back() == '$');
}

TEST_CASE("the heap adjustment tracks the install rather than being hard-coded") {
    // Set on 64-bit Windows, matching maxima.bat; not set on Unix, matching
    // /usr/bin/maxima, which leaves it to MAXIMA_LISP_OPTIONS.
    const std::string wide
        = joined(MaximaSession::launch_command(fake_install(true)));
    CHECK(wide.find("--dynamic-space-size") != std::string::npos);
    CHECK(wide.find("2000") != std::string::npos);

    const std::string narrow
        = joined(MaximaSession::launch_command(fake_install(false)));
    CHECK(narrow.find("--dynamic-space-size") == std::string::npos);
}

TEST_CASE("launch environment isolates the user's maxima-init.mac by default") {
    proxima::Config config;
    config.user_dir = abs("controlled/userdir");
    const auto env = MaximaSession::launch_environment(fake_install(), config);

    const auto find = [&env](std::string_view name) -> std::string {
        for (const auto &[key, value] : env) {
            if (key == name) {
                return value;
            }
        }
        return "<absent>";
    };

    // Forward slashes, as upstream's launchers export them.
    CHECK(find("MAXIMA_PREFIX") == abs("maxima-5.50.0").generic_string());
    CHECK(find("MAXIMA_USERDIR") == abs("controlled/userdir").generic_string());

#ifdef _WIN32
    // The Windows bundle keeps sbcl.core beside sbcl.exe, so maxima.bat points
    // SBCL_HOME at <root>/bin.
    CHECK(find("SBCL_HOME")
          == (abs("maxima-5.50.0") / "bin").generic_string());
#else
    // A distribution SBCL has its home compiled in (/usr/lib/sbcl on openSUSE),
    // which is not <root>/bin. Overriding it would break contrib loading, so
    // the Unix launcher leaves it alone and so do we.
    CHECK(find("SBCL_HOME") == "<absent>");
#endif
}

TEST_CASE("opting into the user's configuration leaves MAXIMA_USERDIR alone") {
    proxima::Config config;
    config.load_user_init = true;
    const auto env = MaximaSession::launch_environment(fake_install(), config);

    for (const auto &[key, value] : env) {
        CHECK(key != "MAXIMA_USERDIR");
    }
}

TEST_CASE("the default user directory is private to the user") {
    // Maxima runs the maxima-init.mac it finds in MAXIMA_USERDIR. The default
    // used to be /tmp/proxima/userdir on Unix: one directory for every user
    // of the machine, so whoever made it first could run code in everyone
    // else's Proxima.
    const auto env
        = MaximaSession::launch_environment(fake_install(), proxima::Config{});
    std::string user_dir;
    for (const auto &[key, value] : env) {
        if (key == "MAXIMA_USERDIR") {
            user_dir = value;
        }
    }
    REQUIRE_FALSE(user_dir.empty());
    CHECK(std::filesystem::is_directory(proxima::detail::path_from_utf8(user_dir)));

#ifndef _WIN32
    CHECK(user_dir.find("/proxima-" + std::to_string(::geteuid()) + "/")
          != std::string::npos);
    for (const std::string &dir :
         {user_dir, std::filesystem::path(user_dir).parent_path().string()}) {
        CAPTURE(dir);
        struct stat info {};
        REQUIRE(::lstat(dir.c_str(), &info) == 0);
        CHECK(info.st_uid == ::geteuid());
        CHECK((info.st_mode & static_cast<mode_t>(0777)) == static_cast<mode_t>(0700));
    }
#endif
}

#ifndef _WIN32
TEST_CASE("a user directory that is not private to the user is refused") {
    namespace fs = std::filesystem;
    using proxima::detail::ensure_private_directory;

    const fs::path base = fs::temp_directory_path()
                          / ("proxima_private_dir_tests_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    REQUIRE_FALSE(ec);

    SUBCASE("created private, and accepted again") {
        const fs::path fresh = base / "fresh";
        CHECK_NOTHROW(ensure_private_directory(fresh));
        struct stat info {};
        REQUIRE(::lstat(fresh.c_str(), &info) == 0);
        CHECK((info.st_mode & static_cast<mode_t>(0777)) == static_cast<mode_t>(0700));
        CHECK_NOTHROW(ensure_private_directory(fresh));
    }
    SUBCASE("open to others") {
        const fs::path open = base / "open";
        REQUIRE(::mkdir(open.c_str(), 0700) == 0);
        REQUIRE(::chmod(open.c_str(), 0777) == 0);
        CHECK_THROWS_AS(ensure_private_directory(open), proxima::KernelError);
    }
    SUBCASE("a symbolic link, even to a private directory") {
        const fs::path target = base / "target";
        REQUIRE(::mkdir(target.c_str(), 0700) == 0);
        const fs::path link = base / "link";
        fs::create_directory_symlink(target, link, ec);
        REQUIRE_FALSE(ec);
        CHECK_THROWS_AS(ensure_private_directory(link), proxima::KernelError);
    }
    SUBCASE("not a directory") {
        const fs::path file = base / "file";
        std::ofstream(file) << "proxima";
        CHECK_THROWS_AS(ensure_private_directory(file), proxima::KernelError);
    }

    fs::remove_all(base, ec);
}
#endif

// --- paths outside ASCII -------------------------------------------------------
//
// One name with a Latin letter and two CJK characters, U+00E6, U+4E2D and
// U+6587: no single ANSI code page holds all three, so on Windows anything
// still going through one fails here.
//
// None of those characters appears raw in this file, and none is written as a
// universal character name either. MSVC reads a source file that has no
// byte-order mark in the ANSI code page unless it is given /utf-8, so a raw
// character in a literal compiles to the UTF-8 of different characters. The
// first version of these tests spelled the name raw and failed under MSVC for
// exactly that reason, while GCC and clang-cl, which assume UTF-8, passed. The
// build now passes /utf-8, but the tests should not depend on it.

namespace {

/// The name's UTF-8 bytes, written out so the expectation is not computed by
/// the code under test.
constexpr const char *kUnicodeNameBytes
    = "m" "\xC3\xA6" "xima_" "\xE4\xB8\xAD" "\xE6\x96\x87";

#ifdef _WIN32
/// The same name as UTF-16 code units, which is what a Windows path holds.
const std::wstring kUnicodeNameWide = {L'm', wchar_t{0x00E6}, L'x', L'i', L'm',
                                       L'a', L'_', wchar_t{0x4E2D},
                                       wchar_t{0x6587}};
#endif

/// The name as a path, built without the conversions under test: from code
/// units on Windows, where a path is wide, and from the bytes elsewhere, where
/// a path is bytes.
std::filesystem::path unicode_name() {
#ifdef _WIN32
    return std::filesystem::path(kUnicodeNameWide);
#else
    return std::filesystem::path(std::string(kUnicodeNameBytes));
#endif
}

std::filesystem::path unicode_path(std::string_view relative = {}) {
    std::filesystem::path path = abs("apps") / unicode_name();
    if (!relative.empty()) {
        path /= relative;
    }
    return path;
}

std::string utf8(const std::filesystem::path &path) {
    return proxima::detail::to_utf8(path);
}

} // namespace

TEST_CASE("paths convert to and from UTF-8 exactly") {
    const std::filesystem::path name = unicode_name();
    CHECK(proxima::detail::to_utf8(name) == kUnicodeNameBytes);
    CHECK(proxima::detail::path_from_utf8(kUnicodeNameBytes) == name);
#ifdef _WIN32
    // And the native, wide, spelling it produces is the right one — not bytes
    // widened one at a time, which would round-trip just as well.
    CHECK(proxima::detail::path_from_utf8(kUnicodeNameBytes).native() == kUnicodeNameWide);
#endif

    // Text that is not UTF-8 is refused, not guessed at. Only Windows has to
    // transcode, so only Windows can notice.
#ifdef _WIN32
    CHECK_FALSE(proxima::detail::try_path_from_utf8("bad\xFF" "byte").has_value());
#endif
    CHECK(proxima::detail::describe_path(name) == kUnicodeNameBytes);
}

TEST_CASE("the real environment is read without loss") {
    // What std::getenv could not do on Windows: its ANSI copy of the
    // environment replaces the CJK characters with '?'.
#ifdef _WIN32
    REQUIRE(_wputenv_s(L"PROXIMA_UTF8_TEST", kUnicodeNameWide.c_str()) == 0);
#else
    REQUIRE(setenv("PROXIMA_UTF8_TEST", kUnicodeNameBytes, 1) == 0);
#endif
    const auto value = proxima::detail::system_env()("PROXIMA_UTF8_TEST");
#ifdef _WIN32
    _wputenv_s(L"PROXIMA_UTF8_TEST", L"");
#else
    unsetenv("PROXIMA_UTF8_TEST");
#endif

    REQUIRE(value.has_value());
    CHECK(*value == kUnicodeNameBytes);
    CHECK_FALSE(proxima::detail::system_env()("PROXIMA_UTF8_TEST").has_value());
}

TEST_CASE("non-ASCII environment values become the right candidate roots") {
    const auto roots = candidate_roots(
        proxima::Config{},
        fake_env({{"MAXIMA_ROOT", utf8(unicode_path("root"))},
                 {"PATH", utf8(unicode_path("tools/bin"))}}));

    REQUIRE(roots.size() == 2);
    CHECK(roots[0] == unicode_path("root"));
    CHECK(roots[1] == unicode_path("tools"));
}

TEST_CASE("an environment value that is not UTF-8 is skipped, not thrown") {
    // Only reachable through an injected environment — the real one is
    // converted to UTF-8 on the way in — but discovery must not turn it into
    // an exception that is not a KernelError.
    // Built as a string: a path cannot hold it on Windows, which is the point.
    const std::string bad = std::string(kPrefix) + "bad\xFF" "root";
    std::vector<std::filesystem::path> roots;
    CHECK_NOTHROW(roots = candidate_roots(proxima::Config{},
                                         fake_env({{"MAXIMA_ROOT", bad},
                                                  {"MAXIMA_PREFIX",
                                                   abs("good").generic_string()}})));
    CHECK(std::find(roots.begin(), roots.end(), abs("good")) != roots.end());
}

TEST_CASE("the launch recipe carries non-ASCII paths as UTF-8") {
    MaximaInstall install = fake_install();
    install.root = unicode_path("maxima-5.50.0");
    install.sbcl_exe = install.root / "bin" / kSbclName;
    install.maxima_core = install.root / "lib" / "maxima" / "5.50.0"
                         / "binary-sbcl" / "maxima.core";

    const std::vector<std::string> argv = MaximaSession::launch_command(install);
    REQUIRE(argv.size() > 2);
    CHECK(argv[0] == utf8(install.sbcl_exe));
    CHECK(argv[0].find(kUnicodeNameBytes) != std::string::npos);
    CHECK(argv[2] == utf8(install.maxima_core));

    proxima::Config config;
    config.user_dir = unicode_path("userdir");
    const auto env = MaximaSession::launch_environment(install, config);
    for (const auto &[key, value] : env) {
        // Every path in the environment names the install or the user
        // directory, and each must be the UTF-8 bytes.
        CAPTURE(key);
        CHECK(value.find(kUnicodeNameBytes) != std::string::npos);
    }
}

TEST_CASE("an ASCII path is handed to SBCL unchanged") {
    // Not shortened just because it could be: messages and SBCL's own
    // *CORE-PATHNAME* should keep the names a person recognises.
    const std::filesystem::path path = abs("Program Files/maxima-5.50.0/bin");
    CHECK(proxima::detail::sbcl_readable_path(path) == path);
}

TEST_CASE("a non-ASCII path is handed to SBCL in a form it can open") {
    namespace fs = std::filesystem;
    fs::path leaf("mx_sbcl_");
    leaf += unicode_name();
    const fs::path dir = fs::temp_directory_path() / leaf;
    std::error_code ec;
    fs::create_directories(dir, ec);
    REQUIRE_FALSE(ec);

    const fs::path readable = proxima::detail::sbcl_readable_path(dir);
    // Whatever comes back names the same directory.
    CHECK(fs::equivalent(readable, dir, ec));

#ifdef _WIN32
    const std::string spelled = proxima::detail::to_utf8(readable);
    const bool ascii = std::all_of(spelled.begin(), spelled.end(), [](char c) {
        return static_cast<unsigned char>(c) < 0x80;
    });
    if (readable == dir) {
        // Short-name generation is off on this volume; nothing to do but
        // pass the long name and let SBCL try.
        MESSAGE("No 8.3 short names on the temp volume; the path is unchanged.");
    } else {
        CHECK(ascii);
    }
#else
    CHECK(readable == dir);
#endif

    fs::remove_all(dir, ec);
}
