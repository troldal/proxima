#pragma once

// How Maxima is started: the command line for its SBCL image, the environment
// it is given, the private directory it keeps user state in, and the one call
// that does all of that for a Config. Nothing here speaks the protocol or
// reads a reply; MaximaSession does, over the transport this hands back.

#include "kernel/discovery.hpp"
#include "transport/itransport.hpp"
#include "transport/process_env.hpp"

#include <proxima/config.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace proxima::detail {

/// Builds the argv used to launch Maxima's SBCL image for `install`, with its
/// paths in UTF-8. Touches no filesystem and starts nothing.
std::vector<std::string> launch_command(const MaximaInstall &install);

/// Builds the environment overrides layered over the parent's environment,
/// with its paths in UTF-8 and forward slashes. May create Config::user_dir,
/// or the default user directory, but starts nothing. Throws KernelError if
/// the default directory exists and is not private to this user (see
/// default_user_dir).
std::vector<EnvOverride> launch_environment(const MaximaInstall &install,
                                            const Config &config);

/// Creates `dir` accessible to its owner only, or accepts it if it already
/// exists as a directory — not a link — owned by this user and closed to
/// everyone else; otherwise throws KernelError. On Windows, only creates it.
void ensure_private_directory(const std::filesystem::path &dir);

/// The user directory Maxima is given when Config::user_dir is empty: a
/// per-user private directory under the system temporary directory, checked
/// with ensure_private_directory, since Maxima executes the maxima-init.mac it
/// finds there.
std::filesystem::path default_user_dir();

/// A Maxima started for `config`: the transport to talk to it over, and the
/// version tag of the installation it came from, which the persistent cache
/// keys on.
struct Launched {
    std::unique_ptr<ITransport> transport;
    std::string version_tag;
};

/// Discovers the Maxima `config` names or allows searching for, and starts
/// it. Throws KernelError if none is usable or the process could not start.
Launched launch_maxima(const Config &config);

} // namespace proxima::detail
