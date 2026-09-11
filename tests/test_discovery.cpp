// Discovery and launch-recipe tests. The precedence chain runs against an
// injected environment rather than the real one, so these need no Maxima and
// cannot be perturbed by whatever the developer happens to have installed.

#include <doctest/doctest.h>

#include "kernel/discovery.hpp"
#include "kernel/session.hpp"

#include <mx/config.hpp>
#include <mx/errors.hpp>

#include <filesystem>
#include <initializer_list>
#include <map>
#include <string>
#include <string_view>
#include <vector>

using mx::detail::candidateRoots;
using mx::detail::EnvLookup;
using mx::detail::inspectRoot;
using mx::detail::MaximaInstall;
using mx::detail::MaximaSession;

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
std::string pathVar(std::initializer_list<const char *> entries) {
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
EnvLookup fakeEnv(std::map<std::string, std::string, std::less<>> vars) {
    return [vars = std::move(vars)](
               std::string_view name) -> std::optional<std::string> {
        if (const auto it = vars.find(name); it != vars.end()) {
            return it->second;
        }
        return std::nullopt;
    };
}

/// An install description that touches no filesystem.
MaximaInstall fakeInstall(bool raiseDynamicSpaceSize = true) {
    MaximaInstall install;
    install.root = abs("maxima-5.50.0");
    install.sbclExe = abs("maxima-5.50.0") / "bin" / kSbclName;
    install.maximaCore = abs("maxima-5.50.0") / "lib" / "maxima" / "5.50.0"
                         / "binary-sbcl" / "maxima.core";
    install.versionTag = "5.50.0";
    install.raiseDynamicSpaceSize = raiseDynamicSpaceSize;
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
    mx::Config config;
    config.maximaRoot = abs("explicit");

    const auto roots = candidateRoots(
        config,
        fakeEnv({{"MAXIMA_ROOT", abs("from_root_var").string()},
                 {"MAXIMA_PREFIX", abs("from_prefix_var").string()},
                 {"PATH", pathVar({"somewhere", "maxima-x/bin", "other"})}}));

    REQUIRE(roots.size() == 4);
    CHECK(roots[0] == abs("explicit"));
    CHECK(roots[1] == abs("from_root_var"));
    CHECK(roots[2] == abs("from_prefix_var"));
    // Maxima's launcher lives at <root>/bin, so the PATH entry contributes its
    // parent.
    CHECK(roots[3] == abs("maxima-x"));
}

TEST_CASE("an unset Config falls through to the environment") {
    const auto roots = candidateRoots(
        mx::Config{}, fakeEnv({{"MAXIMA_ROOT", abs("env").string()}}));
    REQUIRE(roots.size() == 1);
    CHECK(roots.front() == abs("env"));
}

TEST_CASE("only PATH entries named bin become candidates") {
    // Otherwise every directory on PATH would be probed.
    const auto roots = candidateRoots(
        mx::Config{},
        fakeEnv({{"PATH", pathVar({"somewhere", "tools", "apps/maxima/bin"})}}));
    REQUIRE(roots.size() == 1);
    CHECK(roots.front() == abs("apps/maxima"));
}

TEST_CASE("the PATH separator is the platform's own") {
    // ';' on Windows, ':' on POSIX. Getting this wrong silently produces one
    // nonsensical candidate instead of several real ones.
    const auto roots = candidateRoots(
        mx::Config{}, fakeEnv({{"PATH", pathVar({"a/bin", "b/bin"})}}));
    REQUIRE(roots.size() == 2);
    CHECK(roots[0] == abs("a"));
    CHECK(roots[1] == abs("b"));
}

TEST_CASE("duplicate candidates are collapsed") {
    mx::Config config;
    config.maximaRoot = abs("same");
    const auto roots
        = candidateRoots(config, fakeEnv({{"MAXIMA_ROOT", abs("same").string()},
                                          {"MAXIMA_PREFIX", abs("same").string()}}));
    CHECK(roots.size() == 1);
}

TEST_CASE("an empty environment yields no candidates") {
    CHECK(candidateRoots(mx::Config{}, fakeEnv({})).empty());
}

TEST_CASE("a directory that is not a Maxima install is rejected") {
    CHECK_FALSE(inspectRoot(kNotAnInstall).has_value());
    CHECK_FALSE(inspectRoot(abs("definitely/not/here")).has_value());
}

TEST_CASE("an explicit but wrong root is an error, not a reason to search on") {
    // Falling through to the search here would silently run whatever Maxima
    // happened to be installed elsewhere, turning a mistyped path into
    // surprising results rather than a diagnosable failure.
    mx::Config config;
    config.maximaRoot = abs("nowhere/at/all");

    try {
        mx::detail::discoverMaxima(config, fakeEnv({}));
        FAIL("expected discovery to reject an unusable explicit root");
    } catch (const mx::KernelError &e) {
        const std::string message = e.what();
        // An error that does not say what was wrong is not actionable.
        CHECK(message.find(abs("nowhere/at/all").string()) != std::string::npos);
        CHECK(message.find("maxima.core") != std::string::npos);
    }
}

TEST_CASE("discovery with nothing configured names the locations it tried") {
    // No explicit root, and an environment pointing somewhere useless. This
    // still consults the conventional install locations, so on a machine with
    // Maxima installed the search legitimately succeeds.
    mx::Config config;
    try {
        const auto install = mx::detail::discoverMaxima(
            config,
            fakeEnv({{"MAXIMA_ROOT", abs("nowhere/at/all").string()}}));
        // If it succeeded it must have produced something coherent.
        CHECK(std::filesystem::is_regular_file(install.sbclExe));
        CHECK(std::filesystem::is_regular_file(install.maximaCore));
        CHECK_FALSE(install.versionTag.empty());
    } catch (const mx::KernelError &e) {
        const std::string message = e.what();
        CHECK(message.find("Tried:") != std::string::npos);
    }
}

TEST_CASE("launch command wires the core and the prompt markers") {
    const std::vector<std::string> argv
        = MaximaSession::launchCommand(fakeInstall());

    REQUIRE_FALSE(argv.empty());
    CHECK(argv.front() == (abs("maxima-5.50.0") / "bin" / kSbclName).string());

    const std::string text = joined(argv);
    CHECK(text.find("maxima.core") != std::string::npos);
    CHECK(text.find("--noinform") != std::string::npos);
    CHECK(text.find("--end-toplevel-options") != std::string::npos);

    // The markers the protocol layer later splits on must actually be installed.
    CHECK(text.find(MaximaSession::kPromptPrefix) != std::string::npos);
    CHECK(text.find(MaximaSession::kPromptSuffix) != std::string::npos);
}

TEST_CASE("the heap adjustment tracks the install rather than being hard-coded") {
    // Set on 64-bit Windows, matching maxima.bat; not set on Unix, matching
    // /usr/bin/maxima, which leaves it to MAXIMA_LISP_OPTIONS.
    const std::string wide
        = joined(MaximaSession::launchCommand(fakeInstall(true)));
    CHECK(wide.find("--dynamic-space-size") != std::string::npos);
    CHECK(wide.find("2000") != std::string::npos);

    const std::string narrow
        = joined(MaximaSession::launchCommand(fakeInstall(false)));
    CHECK(narrow.find("--dynamic-space-size") == std::string::npos);
}

TEST_CASE("launch environment isolates the user's maxima-init.mac by default") {
    mx::Config config;
    config.userDir = abs("controlled/userdir");
    const auto env = MaximaSession::launchEnvironment(fakeInstall(), config);

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
    mx::Config config;
    config.loadUserInit = true;
    const auto env = MaximaSession::launchEnvironment(fakeInstall(), config);

    for (const auto &[key, value] : env) {
        CHECK(key != "MAXIMA_USERDIR");
    }
}
