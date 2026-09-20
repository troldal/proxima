#pragma once

#include <proxima/config.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace proxima::detail {

/// A validated Maxima installation: everything needed to launch it.
struct MaximaInstall {
    /// Installation prefix: C:\maxima-5.50.0 on Windows, /usr on a Unix
    /// distribution package.
    std::filesystem::path root;
    /// <root>/bin/sbcl[.exe]
    std::filesystem::path sbcl_exe;
    /// <root>/lib[64]/maxima/<tag>/binary-sbcl/maxima.core
    std::filesystem::path maxima_core;
    /// The <tag> above: "branch_5_50_base_9_gf03405fbf_dirty" on the Windows
    /// installer, plain "5.50.0" on a distribution package.
    std::string version_tag;
    /// Whether to pass --dynamic-space-size, which upstream's maxima.bat does
    /// on 64-bit Windows builds so that load("lapack") works. The Unix launcher
    /// does not, leaving it to MAXIMA_LISP_OPTIONS.
    bool raise_dynamic_space_size = false;
};

/// Reads an environment variable; absent means unset. Values are UTF-8.
using EnvLookup = std::function<std::optional<std::string>(std::string_view)>;

/// The real process environment, read through the wide API on Windows so that
/// values outside the ANSI code page arrive intact.
EnvLookup system_env();

/// Runs a command and returns everything it wrote to standard output, or
/// nullopt if it could not be run or did not finish in time. Arguments are
/// UTF-8, as everywhere inside the library.
using CommandRunner
    = std::function<std::optional<std::string>(const std::vector<std::string> &)>;

/// Runs the command as a child process. A `.bat` launcher is run through the
/// command interpreter, which is the only way Windows starts one.
CommandRunner system_command();

/// What `maxima -d` says about an installation: the directory holding
/// maxima.core, and the version tag naming the directory above it.
///
/// The installation's own answer, so no layout is assumed — neither lib
/// against lib64 nor how the version directory is spelled. None if `root` has
/// no launcher, if it could not be run, or if what it printed does not name
/// an existing core.
std::optional<std::pair<std::filesystem::path, std::string>>
ask_launcher(const std::filesystem::path &root, const CommandRunner &run);

/// Candidate installation roots in precedence order:
///
///   1. config.maxima_root, when set explicitly
///   2. $MAXIMA_ROOT
///   3. $MAXIMA_PREFIX          (the variable upstream's own launcher uses)
///   4. the parent of every $PATH entry named "bin", since Maxima's launcher
///      lives at <root>/bin/maxima.bat
///
/// Performs no filesystem access, so the precedence chain is testable against
/// an injected environment rather than the real one.
std::vector<std::filesystem::path> candidate_roots(const Config &config,
                                                   const EnvLookup &env);

/// Conventional install locations, discovered by listing rather than guessing
/// (on Windows, C:\maxima-* and the Program Files variants). Consulted only
/// after candidate_roots comes up empty.
std::vector<std::filesystem::path> known_install_roots();

/// Validates one candidate root, returning nullopt if it is not a usable
/// SBCL-based Maxima installation.
///
/// The core is located by asking the installation — `maxima -d` — and only
/// where that is not possible by the expected layout. That fallback is
/// targeted rather than exhaustive: the prototype's recursive walk descended
/// into gnuplot, vtk, clisp and doc — thousands of files — and could match an
/// unrelated sbcl.exe. Only <root>/bin and <root>/lib are consulted, and the
/// bounded fallback within them runs only if the standard layout is absent.
///
/// With no `run`, the launcher is not consulted and the layout alone decides.
std::optional<MaximaInstall> inspect_root(const std::filesystem::path &root,
                                          const CommandRunner &run = {});

/// The spelling of `path` to put on SBCL's command line.
///
/// On Windows SBCL's C runtime reads its command line through the ANSI API, so
/// an executable or core under a path outside ASCII cannot be opened by the
/// name the transport passes, even though the launch itself is wide. For such
/// a path this returns its 8.3 short form, which is ASCII. Where the volume has
/// no short names it returns the path unchanged, and SBCL may still fail.
///
/// ASCII paths, and every path on other platforms, come back unchanged. Only
/// the two command-line paths need this: SBCL reads the environment, and Maxima
/// the files under its prefix, in full Unicode.
///
/// Consults the filesystem on Windows, so the path should exist.
std::filesystem::path sbcl_readable_path(const std::filesystem::path &path);

/// Returns the first usable installation, or throws KernelError naming every
/// location tried.
///
/// Config::sbcl_exe and Config::maxima_core, set together, end it before it
/// begins: those two files are the installation, and nothing is searched for
/// or inferred. Failing that, a non-empty Config::maxima_root is
/// authoritative: if it does not hold a usable installation this throws
/// rather than searching on, so a mistyped path surfaces as an error instead
/// of silently running a different Maxima.
MaximaInstall discover_maxima(const Config &config, const EnvLookup &env,
                              const CommandRunner &run = {});

} // namespace proxima::detail
