#pragma once

// Windows-only helpers, split out so they can be unit-tested. Included by
// child_process_win32.cpp and by the Windows half of the test suite.

#include "transport/process_env.hpp"

#include <string>
#include <vector>

namespace mx::detail {

/// Quotes a single argument for a Win32 CreateProcess command line, following
/// the escaping rules understood by the standard MSVCRT argv parser (doubling
/// backslashes that precede a quote, escaping embedded quotes).
///
/// Unlike passing arguments through cmd.exe/_popen, CreateProcess takes one
/// command-line string and hands it straight to the child's argv parser, so
/// this is well-defined and avoids the batch-file quoting bugs previously hit
/// with `;` and nested quotes.
std::string quoteArg(const std::string &arg);

/// Joins argv into a single CreateProcess command line, quoting each argument.
std::string buildCommandLine(const std::vector<std::string> &argv);

/// Renders a merged environment as a CreateProcess environment block:
/// "NAME=VALUE\0...\0\0".
std::vector<char>
buildEnvironmentBlock(const std::vector<EnvOverride> &overrides);

} // namespace mx::detail
