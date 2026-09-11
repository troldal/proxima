#pragma once

#include <mx/config.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mx::detail {

/// A validated Maxima installation: everything needed to launch it.
struct MaximaInstall {
    /// Installation prefix, e.g. C:\maxima-5.50.0
    std::filesystem::path root;
    /// <root>/bin/sbcl.exe
    std::filesystem::path sbclExe;
    /// <root>/lib/maxima/<tag>/binary-sbcl/maxima.core
    std::filesystem::path maximaCore;
    /// The <tag> above, e.g. "branch_5_50_base_9_gf03405fbf_dirty".
    std::string versionTag;
    /// True when this looks like a 64-bit build, which upstream's maxima.bat
    /// detects by the presence of libgcc_s_seh-1.dll and responds to by raising
    /// SBCL's dynamic space size so that load("lapack") works.
    bool is64Bit = false;
};

/// Reads an environment variable; absent means unset.
using EnvLookup = std::function<std::optional<std::string>(std::string_view)>;

/// The real process environment.
EnvLookup systemEnv();

/// Candidate installation roots in precedence order:
///
///   1. config.maximaRoot, when set explicitly
///   2. $MAXIMA_ROOT
///   3. $MAXIMA_PREFIX          (the variable upstream's own launcher uses)
///   4. the parent of every $PATH entry named "bin", since Maxima's launcher
///      lives at <root>/bin/maxima.bat
///
/// Performs no filesystem access, so the precedence chain is testable against
/// an injected environment rather than the real one.
std::vector<std::filesystem::path> candidateRoots(const Config &config,
                                                  const EnvLookup &env);

/// Conventional install locations, discovered by listing rather than guessing
/// (on Windows, C:\maxima-* and the Program Files variants). Consulted only
/// after candidateRoots comes up empty.
std::vector<std::filesystem::path> knownInstallRoots();

/// Validates one candidate root against the expected layout, returning nullopt
/// if it is not a usable SBCL-based Maxima installation.
///
/// Targeted rather than exhaustive: the prototype's recursive walk descended
/// into gnuplot, vtk, clisp and doc — thousands of files — and could match an
/// unrelated sbcl.exe. Only <root>/bin and <root>/lib are consulted, and the
/// bounded fallback within them runs only if the standard layout is absent.
std::optional<MaximaInstall> inspectRoot(const std::filesystem::path &root);

/// Returns the first usable installation, or throws KernelError naming every
/// location tried.
///
/// A non-empty Config::maximaRoot is authoritative: if it does not hold a
/// usable installation this throws rather than searching on, so a mistyped
/// path surfaces as an error instead of silently running a different Maxima.
MaximaInstall discoverMaxima(const Config &config, const EnvLookup &env);

} // namespace mx::detail
