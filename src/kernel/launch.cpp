#include "kernel/launch.hpp"

#include "kernel/protocol.hpp"
#include "transport/child_process.hpp"
#include "util/utf8.hpp"

#include <proxima/errors.hpp>

#include <algorithm>
#include <cerrno>
#include <string>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace proxima::detail {
namespace {

// Maxima expects Windows paths with forward slashes; upstream's maxima.bat
// performs the same substitution before exporting maxima_prefix. UTF-8, like
// every string handed to the transport; '\\' is ASCII, so replacing it in the
// encoded bytes cannot touch part of a longer character.
std::string to_maxima_path(const std::filesystem::path &p) {
    std::string text = to_utf8(p);
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

} // namespace

std::vector<std::string> launch_command(const MaximaInstall &install) {
    // UTF-8, as the transport takes every string. path::string() would be the
    // ANSI code page on Windows, which mangles a Maxima installed under a path
    // outside it before SBCL ever sees it.
    std::vector<std::string> argv{to_utf8(install.sbcl_exe), "--core",
                                  to_utf8(install.maxima_core), "--noinform"};

    if (install.raise_dynamic_space_size) {
        // What maxima.bat does on 64-bit builds, and for the same reason:
        // without the larger heap, load("lapack") runs out of dynamic space.
        argv.emplace_back("--dynamic-space-size");
        argv.emplace_back("2000");
    }

    // --disable-debugger is a *toplevel* option, not a runtime one, so it
    // belongs after --end-runtime-options. Without it, an unhandled Lisp error
    // drops SBCL into a debugger that reads standard input — over a pipe, a
    // deadlock. With it, the process exits instead, which recover() can undo.
    // errcatch is unaffected: it handles the error before the debugger would
    // ever see it.
    argv.insert(argv.end(), {"--end-runtime-options", "--disable-debugger", "--eval",
                             std::string(helper_lisp()), "--end-toplevel-options"});
    return argv;
}

std::vector<EnvOverride> launch_environment(const MaximaInstall &install,
                                            const Config &config) {
    std::vector<EnvOverride> env;

    // Correct even where the image already has a prefix compiled in, which
    // matters for a relocated or portable installation whose baked-in path no
    // longer exists.
    env.emplace_back("MAXIMA_PREFIX", to_maxima_path(install.root));

#ifdef _WIN32
    // Windows only, and deliberately so. The Windows bundle keeps sbcl.core
    // beside sbcl.exe and maxima.bat sets SBCL_HOME to that directory because
    // the crosscompiled installer does not. A distribution SBCL has its home
    // compiled in (/usr/lib/sbcl on openSUSE), which is *not* <root>/bin —
    // overriding it there would break contrib loading rather than fix it.
    env.emplace_back("SBCL_HOME", to_maxima_path(install.sbcl_exe.parent_path()));
#endif

    if (!config.load_user_init) {
        // Point Maxima's user directory somewhere we control so it does not
        // read the user's maxima-init.mac. See Config::load_user_init.
        std::filesystem::path user_dir = config.user_dir;
        if (user_dir.empty()) {
            user_dir = default_user_dir();
        } else {
            std::error_code ec;
            std::filesystem::create_directories(user_dir, ec);
        }
        env.emplace_back("MAXIMA_USERDIR", to_maxima_path(user_dir));
    }

    return env;
}

void ensure_private_directory(const std::filesystem::path &dir) {
#ifdef _WIN32
    // %TEMP% is under the user's own profile, which other users cannot write.
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
#else
    const auto refuse = [&dir](const std::string &why) {
        throw KernelError("refusing to use " + to_utf8(dir)
                          + " as Maxima's user directory: " + why);
    };

    // mkdir with the mode, rather than create_directories and a chmod after,
    // so there is no moment at which the directory exists and is open to
    // others. An existing one is accepted only if it is already private.
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        refuse(std::generic_category().message(errno));
    }

    // lstat, not stat: a symbolic link planted in its place is refused rather
    // than followed to wherever its owner chose.
    struct stat info{};
    if (::lstat(dir.c_str(), &info) != 0) {
        refuse(std::generic_category().message(errno));
    }
    if (!S_ISDIR(info.st_mode)) {
        refuse("it is not a directory");
    }
    if (info.st_uid != ::geteuid()) {
        refuse("it belongs to another user");
    }
    if ((info.st_mode & static_cast<mode_t>(0077)) != 0) {
        refuse("other users have access to it");
    }
#endif
}

std::filesystem::path default_user_dir() {
    std::error_code ec;
    const std::filesystem::path temp = std::filesystem::temp_directory_path(ec);

#ifdef _WIN32
    const std::filesystem::path user_dir = temp / "proxima" / "userdir";
    ensure_private_directory(user_dir);
#else
    // Maxima runs whatever maxima-init.mac it finds here. /tmp is shared by
    // every user of the machine, so a single /tmp/proxima/userdir — what this
    // used to be — let whoever created it first run code in every other
    // user's Proxima. One directory per user, and only if it is really theirs.
    const std::filesystem::path base
        = temp / ("proxima-" + std::to_string(::geteuid()));
    ensure_private_directory(base);
    const std::filesystem::path user_dir = base / "userdir";
    ensure_private_directory(user_dir);
#endif
    return user_dir;
}

Launched launch_maxima(const Config &config) {
    const MaximaInstall install
        = discover_maxima(config, system_env(), system_command());

    // SBCL's runtime opens its executable and core by the names on its
    // command line, which it reads through the ANSI API on Windows; those two
    // get a spelling it can open. The core may need the child's working
    // directory to be spelled for it: see core_spelling. The environment keeps
    // the real paths, which SBCL and Maxima read in full Unicode.
    MaximaInstall launchable = install;
    launchable.sbcl_exe = sbcl_readable_path(install.sbcl_exe);
    const CoreSpelling spelling = core_spelling(install.maxima_core);
    launchable.maxima_core = spelling.core;

    return {std::make_unique<ChildProcessTransport>(
                launch_command(launchable), launch_environment(install, config),
                spelling.start_dir),
            install.version_tag};
}

} // namespace proxima::detail
