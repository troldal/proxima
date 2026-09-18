#include "kernel/discovery.hpp"

#include "util/utf8.hpp"

#include <proxima/errors.hpp>

#include <boost/process/v2/environment.hpp>

#include <algorithm>
#include <system_error>

namespace proxima::detail {
namespace {

namespace fs = std::filesystem;

#ifdef _WIN32
constexpr const char *kSbclName = "sbcl.exe";
constexpr char kSearchPathSeparator = ';';
#else
constexpr const char *kSbclName = "sbcl";
constexpr char kSearchPathSeparator = ':';
#endif

// Where a distribution may place the Lisp images. Debian and Windows use
// "lib"; openSUSE, Fedora and other multilib layouts use "lib64". Both are
// tried everywhere: the one that does not apply simply will not exist.
constexpr const char *kLibDirNames[] = {"lib64", "lib"};

bool regular_file_exists(const fs::path &p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

bool directory_exists(const fs::path &p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

/// Directory entries of `dir`, sorted by name. Empty if `dir` is unreadable.
std::vector<fs::path> sorted_children(const fs::path &dir) {
    std::vector<fs::path> children;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        children.push_back(entry.path());
    }
    std::sort(children.begin(), children.end());
    return children;
}

/// Whether a directory's name starts "maxima" or "Maxima".
///
/// Compared in the path's native encoding rather than converted first, so a
/// neighbouring directory whose name will not convert — anything can sit in
/// C:\Program Files — is simply not a match instead of an exception.
bool named_like_maxima(const fs::path &p) {
    const fs::path::string_type name = p.filename().native();
    for (const fs::path &prefix : {fs::path("maxima"), fs::path("Maxima")}) {
        if (name.rfind(prefix.native(), 0) == 0) {
            return true;
        }
    }
    return false;
}

/// Splits a PATH-style variable, given as UTF-8, on the platform's separator.
/// The separator is ASCII, so splitting the encoded bytes is safe.
std::vector<fs::path> split_search_path(const std::string &value) {
    std::vector<fs::path> entries;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t end = value.find(kSearchPathSeparator, start);
        const std::string piece = value.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!piece.empty()) {
            if (auto entry = try_path_from_utf8(piece)) {
                entries.push_back(std::move(*entry));
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return entries;
}

/// Locates maxima.core under <root>/lib64 or <root>/lib, preferring the
/// documented layout.
///
/// Returns the core path and the version tag naming its directory.
std::optional<std::pair<fs::path, std::string>> find_core(const fs::path &root) {
    // Several version tags can coexist; take the last by name so the choice is
    // deterministic rather than dependent on directory order.
    const auto newest_core_in = [](const fs::path &package_dir)
        -> std::optional<std::pair<fs::path, std::string>> {
        const std::vector<fs::path> versions = sorted_children(package_dir);
        for (auto it = versions.rbegin(); it != versions.rend(); ++it) {
            const fs::path core = *it / "binary-sbcl" / "maxima.core";
            if (!regular_file_exists(core)) {
                continue;
            }
            if (auto tag = try_to_utf8(it->filename())) {
                return std::pair{core, std::move(*tag)};
            }
        }
        return std::nullopt;
    };

    for (const char *lib_name : kLibDirNames) {
        const fs::path lib_dir = root / lib_name;
        if (!directory_exists(lib_dir)) {
            continue;
        }

        // The documented layout:
        // <root>/lib[64]/maxima/<tag>/binary-sbcl/maxima.core
        const fs::path package_dir = lib_dir / "maxima";
        if (directory_exists(package_dir)) {
            if (auto core = newest_core_in(package_dir)) {
                return core;
            }
        }

        // Fallback for a non-standard package name, still bounded to the lib
        // directory rather than walking the whole installation.
        for (const fs::path &package : sorted_children(lib_dir)) {
            if (!directory_exists(package)) {
                continue;
            }
            if (auto core = newest_core_in(package)) {
                return core;
            }
        }
    }

    return std::nullopt;
}

} // namespace

EnvLookup system_env() {
    return [](std::string_view name) -> std::optional<std::string> {
        // Not std::getenv: on Windows that reads the environment through the
        // ANSI code page, so a MAXIMA_ROOT or PATH entry with a character
        // outside it arrives mangled. Boost.Process reads the wide
        // environment and hands the value back as UTF-8.
        namespace environment = boost::process::v2::environment;
        boost::system::error_code ec;
        const environment::value value
            = environment::get(environment::key(std::string(name)), ec);
        if (ec) {
            return std::nullopt;
        }
        std::string text = value.string();
        if (text.empty()) {
            return std::nullopt;
        }
        return text;
    };
}

std::vector<fs::path> candidate_roots(const Config &config, const EnvLookup &env) {
    std::vector<fs::path> roots;
    const auto add = [&roots](const fs::path &p) {
        if (!p.empty() && std::find(roots.begin(), roots.end(), p) == roots.end()) {
            roots.push_back(p);
        }
    };

    if (!config.maxima_root.empty()) {
        add(config.maxima_root);
    }
    // Environment values are UTF-8 (see EnvLookup). One that does not convert
    // cannot name a directory anyway, so it is skipped rather than thrown.
    for (const char *name : {"MAXIMA_ROOT", "MAXIMA_PREFIX"}) {
        if (const auto value = env(name)) {
            if (auto root = try_path_from_utf8(*value)) {
                add(*root);
            }
        }
    }
    if (const auto value = env("PATH")) {
        // Maxima's launcher lives at <root>/bin/maxima.bat, so a PATH entry
        // named "bin" is the only shape worth considering. Anything else would
        // turn every directory on PATH into a candidate.
        for (const fs::path &entry : split_search_path(*value)) {
            if (entry.filename() == "bin") {
                add(entry.parent_path());
            }
        }
    }
    return roots;
}

std::vector<fs::path> known_install_roots() {
    std::vector<fs::path> roots;

#ifdef _WIN32
    // Windows installs into a versioned directory of its own.
    const fs::path search_in[]
        = {"C:\\", "C:\\Program Files", "C:\\Program Files (x86)"};
    for (const fs::path &parent : search_in) {
        for (const fs::path &child : sorted_children(parent)) {
            if (named_like_maxima(child)) {
                roots.push_back(child);
            }
        }
    }
    // Newest-looking last-by-name first, so 5.50 beats 5.47.
    std::reverse(roots.begin(), roots.end());
#else
    // On Unix a distribution package puts Maxima under an existing prefix
    // rather than a directory of its own, so the prefixes themselves are the
    // candidates. /usr/local first: a hand-built Maxima there is a deliberate
    // choice and should win over the distribution's.
    roots.emplace_back("/usr/local");
    roots.emplace_back("/usr");

    // Self-contained installs under /opt still get their own directory.
    for (const fs::path &child : sorted_children("/opt")) {
        if (named_like_maxima(child)) {
            roots.push_back(child);
        }
    }
#endif

    return roots;
}

std::optional<MaximaInstall> inspect_root(const fs::path &root) {
    if (!directory_exists(root)) {
        return std::nullopt;
    }

    MaximaInstall install;
    install.root = root;

    install.sbcl_exe = root / "bin" / kSbclName;
    if (!regular_file_exists(install.sbcl_exe)) {
        return std::nullopt;
    }

    auto core = find_core(root);
    if (!core) {
        return std::nullopt;
    }
    install.maxima_core = std::move(core->first);
    install.version_tag = std::move(core->second);

#ifdef _WIN32
    // Upstream's maxima.bat raises SBCL's dynamic space on 64-bit builds so
    // that load("lapack") works, detecting them by this DLL. Reproduced so the
    // adjustment is applied under exactly the same conditions.
    install.raise_dynamic_space_size
        = regular_file_exists(root / "bin" / "libgcc_s_seh-1.dll");
#else
    // The Unix launcher leaves the heap alone, exposing it through
    // MAXIMA_LISP_OPTIONS instead. Match that rather than invent a default.
    install.raise_dynamic_space_size = false;
#endif

    return install;
}

MaximaInstall discover_maxima(const Config &config, const EnvLookup &env) {
    // An explicitly configured root is authoritative. Falling through to the
    // search when it turns out to be wrong would silently run a different
    // installation than the caller asked for, turning a configuration mistake
    // into results that are merely surprising instead of an error.
    if (!config.maxima_root.empty()) {
        if (auto install = inspect_root(config.maxima_root)) {
            return *install;
        }
        throw KernelError(
            "Config::maxima_root does not point at a usable Maxima "
            "installation: "
            + describe_path(config.maxima_root) + "\nExpected <root>/bin/"
            + kSbclName
            + " and <root>/lib/maxima/<version>/binary-sbcl/maxima.core");
    }

    std::vector<fs::path> tried;

    const auto search
        = [&](const std::vector<fs::path> &roots) -> std::optional<MaximaInstall> {
        for (const fs::path &root : roots) {
            if (auto install = inspect_root(root)) {
                return install;
            }
            tried.push_back(root);
        }
        return std::nullopt;
    };

    if (auto install = search(candidate_roots(config, env))) {
        return *install;
    }
    // Only worth listing the conventional locations once nothing was named
    // explicitly and nothing on PATH panned out.
    if (auto install = search(known_install_roots())) {
        return *install;
    }

    std::string message
        = std::string("No usable Maxima installation found. Expected <root>/bin/")
          + kSbclName + " and <root>/lib/maxima/<version>/binary-sbcl/maxima.core. ";
    if (tried.empty()) {
        message += "No candidate locations: set Config::maxima_root or the "
                   "MAXIMA_ROOT environment variable.";
    } else {
        message += "Tried:";
        for (const fs::path &root : tried) {
            message += "\n  " + describe_path(root);
        }
    }
    throw KernelError(message);
}

} // namespace proxima::detail
