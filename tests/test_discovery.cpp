// Discovery and launch-recipe tests. The precedence chain runs against an
// injected environment rather than the real one, so these need no Maxima and
// cannot be perturbed by whatever the developer happens to have installed.

#include <doctest/doctest.h>

#include "kernel/discovery.hpp"
#include "kernel/session.hpp"
#include "transport/child_process_win32.hpp"

#include <mx/config.hpp>
#include <mx/errors.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using mx::detail::buildEnvironmentBlock;
using mx::detail::candidateRoots;
using mx::detail::EnvLookup;
using mx::detail::inspectRoot;
using mx::detail::MaximaInstall;
using mx::detail::MaximaSession;

namespace {

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

/// Splits an environment block back into "NAME=VALUE" entries.
std::vector<std::string> parseBlock(const std::vector<char> &block) {
    std::vector<std::string> entries;
    size_t start = 0;
    while (start < block.size() && block[start] != '\0') {
        const std::string entry(block.data() + start);
        entries.push_back(entry);
        start += entry.size() + 1;
    }
    return entries;
}

bool containsEntry(const std::vector<std::string> &entries,
                   const std::string &wanted) {
    return std::find(entries.begin(), entries.end(), wanted) != entries.end();
}

/// An install description that touches no filesystem.
MaximaInstall fakeInstall(bool is64Bit = true) {
    MaximaInstall install;
    install.root = "C:\\maxima-5.50.0";
    install.sbclExe = "C:\\maxima-5.50.0\\bin\\sbcl.exe";
    install.maximaCore
        = "C:\\maxima-5.50.0\\lib\\maxima\\branch_5_50\\binary-sbcl\\maxima.core";
    install.versionTag = "branch_5_50";
    install.is64Bit = is64Bit;
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
    config.maximaRoot = "C:\\explicit";

    const auto roots = candidateRoots(
        config, fakeEnv({{"MAXIMA_ROOT", "C:\\from_root_var"},
                         {"MAXIMA_PREFIX", "C:\\from_prefix_var"},
                         {"PATH", "C:\\windows;C:\\maxima-x\\bin;C:\\other"}}));

    REQUIRE(roots.size() == 4);
    CHECK(roots[0] == std::filesystem::path("C:\\explicit"));
    CHECK(roots[1] == std::filesystem::path("C:\\from_root_var"));
    CHECK(roots[2] == std::filesystem::path("C:\\from_prefix_var"));
    // Maxima's launcher lives at <root>/bin, so the PATH entry contributes its
    // parent.
    CHECK(roots[3] == std::filesystem::path("C:\\maxima-x"));
}

TEST_CASE("an unset Config falls through to the environment") {
    const auto roots
        = candidateRoots(mx::Config{}, fakeEnv({{"MAXIMA_ROOT", "C:\\env"}}));
    REQUIRE(roots.size() == 1);
    CHECK(roots.front() == std::filesystem::path("C:\\env"));
}

TEST_CASE("only PATH entries named bin become candidates") {
    // Otherwise every directory on PATH would be probed.
    const auto roots = candidateRoots(
        mx::Config{},
        fakeEnv({{"PATH", "C:\\windows;C:\\tools;D:\\apps\\maxima\\bin"}}));
    REQUIRE(roots.size() == 1);
    CHECK(roots.front() == std::filesystem::path("D:\\apps\\maxima"));
}

TEST_CASE("duplicate candidates are collapsed") {
    mx::Config config;
    config.maximaRoot = "C:\\same";
    const auto roots = candidateRoots(
        config, fakeEnv({{"MAXIMA_ROOT", "C:\\same"},
                         {"MAXIMA_PREFIX", "C:\\same"}}));
    CHECK(roots.size() == 1);
}

TEST_CASE("an empty environment yields no candidates") {
    CHECK(candidateRoots(mx::Config{}, fakeEnv({})).empty());
}

TEST_CASE("a directory that is not a Maxima install is rejected") {
    CHECK_FALSE(inspectRoot("C:\\Windows").has_value());
    CHECK_FALSE(inspectRoot("C:\\definitely\\not\\here").has_value());
}

TEST_CASE("an explicit but wrong root is an error, not a reason to search on") {
    // Falling through to the search here would silently run whatever Maxima
    // happened to be installed elsewhere, turning a mistyped path into
    // surprising results rather than a diagnosable failure.
    mx::Config config;
    config.maximaRoot = "C:\\nowhere\\at\\all";

    try {
        mx::detail::discoverMaxima(config, fakeEnv({}));
        FAIL("expected discovery to reject an unusable explicit root");
    } catch (const mx::KernelError &e) {
        const std::string message = e.what();
        // An error that does not say what was wrong is not actionable.
        CHECK(message.find("C:\\nowhere\\at\\all") != std::string::npos);
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
            config, fakeEnv({{"MAXIMA_ROOT", "C:\\nowhere\\at\\all"}}));
        // If it succeeded it must have produced something coherent.
        CHECK(std::filesystem::is_regular_file(install.sbclExe));
        CHECK(std::filesystem::is_regular_file(install.maximaCore));
        CHECK_FALSE(install.versionTag.empty());
    } catch (const mx::KernelError &e) {
        const std::string message = e.what();
        CHECK(message.find("Tried:") != std::string::npos);
        CHECK(message.find("C:\\nowhere\\at\\all") != std::string::npos);
    }
}

TEST_CASE("launch command wires the core and the prompt markers") {
    const std::vector<std::string> argv
        = MaximaSession::launchCommand(fakeInstall());

    REQUIRE_FALSE(argv.empty());
    CHECK(argv.front() == "C:\\maxima-5.50.0\\bin\\sbcl.exe");

    const std::string text = joined(argv);
    CHECK(text.find("maxima.core") != std::string::npos);
    CHECK(text.find("--noinform") != std::string::npos);
    CHECK(text.find("--end-toplevel-options") != std::string::npos);

    // The markers the protocol layer later splits on must actually be installed.
    CHECK(text.find(MaximaSession::kPromptPrefix) != std::string::npos);
    CHECK(text.find(MaximaSession::kPromptSuffix) != std::string::npos);
}

TEST_CASE("the 64-bit heap adjustment tracks the install, matching maxima.bat") {
    const std::string wide = joined(MaximaSession::launchCommand(fakeInstall(true)));
    CHECK(wide.find("--dynamic-space-size") != std::string::npos);
    CHECK(wide.find("2000") != std::string::npos);

    const std::string narrow
        = joined(MaximaSession::launchCommand(fakeInstall(false)));
    CHECK(narrow.find("--dynamic-space-size") == std::string::npos);
}

TEST_CASE("launch environment isolates the user's maxima-init.mac by default") {
    mx::Config config;
    config.userDir = "C:\\controlled\\userdir";
    const auto env = MaximaSession::launchEnvironment(fakeInstall(), config);

    const auto find = [&env](std::string_view name) -> std::string {
        for (const auto &[key, value] : env) {
            if (key == name) {
                return value;
            }
        }
        return "<absent>";
    };

    // Forward slashes, as upstream's launcher exports them.
    CHECK(find("MAXIMA_PREFIX") == "C:/maxima-5.50.0");
    CHECK(find("SBCL_HOME") == "C:/maxima-5.50.0/bin");
    CHECK(find("MAXIMA_USERDIR") == "C:/controlled/userdir");
}

TEST_CASE("opting into the user's configuration leaves MAXIMA_USERDIR alone") {
    mx::Config config;
    config.loadUserInit = true;
    const auto env = MaximaSession::launchEnvironment(fakeInstall(), config);

    for (const auto &[key, value] : env) {
        CHECK(key != "MAXIMA_USERDIR");
    }
}

TEST_CASE("environment block merges over the inherited environment") {
    const std::vector<std::string> entries
        = parseBlock(buildEnvironmentBlock({{"MX_TEST_NEW_VAR", "hello"}}));

    CHECK(containsEntry(entries, "MX_TEST_NEW_VAR=hello"));
    // The child must keep everything else, or it loses PATH and cannot even
    // resolve its own DLLs.
    const bool keptPath
        = std::any_of(entries.begin(), entries.end(), [](const std::string &e) {
              return e.size() > 5 && (e.rfind("PATH=", 0) == 0
                                      || e.rfind("Path=", 0) == 0);
          });
    CHECK(keptPath);
}

TEST_CASE("environment overrides match names case-insensitively") {
    // Windows compares variable names without regard to case, so an override
    // spelled differently from the inherited entry must replace it rather than
    // produce two entries the child would read ambiguously.
    const auto entries = parseBlock(buildEnvironmentBlock({{"path", "C:\\only"}}));

    const auto isPath = [](const std::string &entry) {
        return entry.size() >= 5
               && (entry.rfind("path=", 0) == 0 || entry.rfind("PATH=", 0) == 0
                   || entry.rfind("Path=", 0) == 0);
    };
    CHECK(std::count_if(entries.begin(), entries.end(), isPath) == 1);
    CHECK(containsEntry(entries, "path=C:\\only"));
}

TEST_CASE("environment block is double-NUL terminated") {
    const auto block = buildEnvironmentBlock({{"MX_TEST_X", "1"}});
    REQUIRE(block.size() >= 2);
    CHECK(block.back() == '\0');
    CHECK(block[block.size() - 2] == '\0');
}
