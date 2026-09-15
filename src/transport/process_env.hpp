#pragma once

#include <string>
#include <utility>
#include <vector>

namespace proxima::detail {

/// A name/value pair layered over the inherited environment.
using EnvOverride = std::pair<std::string, std::string>;

/// Merges `overrides` over the current process environment and returns the
/// result as "NAME=VALUE" entries.
///
/// Name matching follows the platform, and the difference is not cosmetic:
/// Windows compares variable names case-insensitively, so an override spelled
/// "path" must *replace* an inherited "Path" rather than sit beside it — two
/// entries differing only in case leave the child reading whichever it finds
/// first. POSIX treats PATH and Path as genuinely different variables, so the
/// same merge there must be case-sensitive.
std::vector<std::string>
mergeEnvironment(const std::vector<EnvOverride> &overrides);

} // namespace proxima::detail
