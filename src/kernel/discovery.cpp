#include "kernel/discovery.hpp"

#include <mx/errors.hpp>

#include <algorithm>
#include <cstdlib>
#include <system_error>

namespace mx::detail {
namespace {

namespace fs = std::filesystem;

bool isRegularFile(const fs::path &p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

bool isDirectory(const fs::path &p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

/// Directory entries of `dir`, sorted by name. Empty if `dir` is unreadable.
std::vector<fs::path> sortedChildren(const fs::path &dir) {
    std::vector<fs::path> children;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        children.push_back(entry.path());
    }
    std::sort(children.begin(), children.end());
    return children;
}

/// Splits a PATH-style variable on ';'.
std::vector<fs::path> splitSearchPath(const std::string &value) {
    std::vector<fs::path> entries;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t end = value.find(';', start);
        const std::string piece
            = value.substr(start, end == std::string::npos ? std::string::npos
                                                           : end - start);
        if (!piece.empty()) {
            entries.emplace_back(piece);
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return entries;
}

/// Locates maxima.core under <root>/lib, preferring the documented layout.
///
/// Returns the core path and the version tag naming its directory.
std::optional<std::pair<fs::path, std::string>>
findCore(const fs::path &root) {
    const fs::path libDir = root / "lib";
    if (!isDirectory(libDir)) {
        return std::nullopt;
    }

    // The documented layout: <root>/lib/maxima/<tag>/binary-sbcl/maxima.core.
    // Several version tags can coexist; take the last by name so the choice is
    // deterministic rather than dependent on directory order.
    const fs::path packageDir = libDir / "maxima";
    if (isDirectory(packageDir)) {
        const std::vector<fs::path> versions = sortedChildren(packageDir);
        for (auto it = versions.rbegin(); it != versions.rend(); ++it) {
            const fs::path core = *it / "binary-sbcl" / "maxima.core";
            if (isRegularFile(core)) {
                return std::pair{core, it->filename().string()};
            }
        }
    }

    // Fallback for a non-standard package name, still bounded to <root>/lib
    // rather than walking the whole installation.
    for (const fs::path &package : sortedChildren(libDir)) {
        if (!isDirectory(package)) {
            continue;
        }
        const std::vector<fs::path> versions = sortedChildren(package);
        for (auto it = versions.rbegin(); it != versions.rend(); ++it) {
            const fs::path core = *it / "binary-sbcl" / "maxima.core";
            if (isRegularFile(core)) {
                return std::pair{core, it->filename().string()};
            }
        }
    }

    return std::nullopt;
}

} // namespace

EnvLookup systemEnv() {
    return [](std::string_view name) -> std::optional<std::string> {
        // std::getenv wants a NUL-terminated string.
        const std::string key(name);
        if (const char *value = std::getenv(key.c_str());
            value != nullptr && *value != '\0') {
            return std::string(value);
        }
        return std::nullopt;
    };
}

std::vector<fs::path> candidateRoots(const Config &config, const EnvLookup &env) {
    std::vector<fs::path> roots;
    const auto add = [&roots](const fs::path &p) {
        if (!p.empty()
            && std::find(roots.begin(), roots.end(), p) == roots.end()) {
            roots.push_back(p);
        }
    };

    if (!config.maximaRoot.empty()) {
        add(config.maximaRoot);
    }
    if (const auto value = env("MAXIMA_ROOT")) {
        add(*value);
    }
    if (const auto value = env("MAXIMA_PREFIX")) {
        add(*value);
    }
    if (const auto value = env("PATH")) {
        // Maxima's launcher lives at <root>/bin/maxima.bat, so a PATH entry
        // named "bin" is the only shape worth considering. Anything else would
        // turn every directory on PATH into a candidate.
        for (const fs::path &entry : splitSearchPath(*value)) {
            if (entry.filename() == "bin") {
                add(entry.parent_path());
            }
        }
    }
    return roots;
}

std::vector<fs::path> knownInstallRoots() {
    std::vector<fs::path> roots;
    const fs::path searchIn[] = {"C:\\", "C:\\Program Files",
                                 "C:\\Program Files (x86)"};
    for (const fs::path &parent : searchIn) {
        for (const fs::path &child : sortedChildren(parent)) {
            const std::string name = child.filename().string();
            if (name.rfind("maxima", 0) == 0 || name.rfind("Maxima", 0) == 0) {
                roots.push_back(child);
            }
        }
    }
    // Newest-looking last-by-name first, so 5.50 beats 5.47.
    std::reverse(roots.begin(), roots.end());
    return roots;
}

std::optional<MaximaInstall> inspectRoot(const fs::path &root) {
    if (!isDirectory(root)) {
        return std::nullopt;
    }

    MaximaInstall install;
    install.root = root;

    install.sbclExe = root / "bin" / "sbcl.exe";
    if (!isRegularFile(install.sbclExe)) {
        return std::nullopt;
    }

    auto core = findCore(root);
    if (!core) {
        return std::nullopt;
    }
    install.maximaCore = std::move(core->first);
    install.versionTag = std::move(core->second);

    // Upstream's own 64-bit test, reproduced so the dynamic-space-size
    // adjustment is applied under exactly the same conditions.
    install.is64Bit = isRegularFile(root / "bin" / "libgcc_s_seh-1.dll");

    return install;
}

MaximaInstall discoverMaxima(const Config &config, const EnvLookup &env) {
    // An explicitly configured root is authoritative. Falling through to the
    // search when it turns out to be wrong would silently run a different
    // installation than the caller asked for, turning a configuration mistake
    // into results that are merely surprising instead of an error.
    if (!config.maximaRoot.empty()) {
        if (auto install = inspectRoot(config.maximaRoot)) {
            return *install;
        }
        throw KernelError(
            "Config::maximaRoot does not point at a usable Maxima "
            "installation: "
            + config.maximaRoot.string()
            + "\nExpected <root>/bin/sbcl.exe and "
              "<root>/lib/maxima/<version>/binary-sbcl/maxima.core");
    }

    std::vector<fs::path> tried;

    const auto search = [&](const std::vector<fs::path> &roots)
        -> std::optional<MaximaInstall> {
        for (const fs::path &root : roots) {
            if (auto install = inspectRoot(root)) {
                return install;
            }
            tried.push_back(root);
        }
        return std::nullopt;
    };

    if (auto install = search(candidateRoots(config, env))) {
        return *install;
    }
    // Only worth listing the conventional locations once nothing was named
    // explicitly and nothing on PATH panned out.
    if (auto install = search(knownInstallRoots())) {
        return *install;
    }

    std::string message
        = "No usable Maxima installation found. Expected <root>/bin/sbcl.exe "
          "and <root>/lib/maxima/<version>/binary-sbcl/maxima.core. ";
    if (tried.empty()) {
        message += "No candidate locations: set Config::maximaRoot or the "
                   "MAXIMA_ROOT environment variable.";
    } else {
        message += "Tried:";
        for (const fs::path &root : tried) {
            message += "\n  " + root.string();
        }
    }
    throw KernelError(message);
}

} // namespace mx::detail
