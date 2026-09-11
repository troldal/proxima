#include "transport/process_env.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
extern char **environ;
#endif

namespace mx::detail {
namespace {

bool namesMatch(std::string_view a, std::string_view b) {
#ifdef _WIN32
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const auto lhs = static_cast<unsigned char>(a[i]);
        const auto rhs = static_cast<unsigned char>(b[i]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }
    return true;
#else
    return a == b;
#endif
}

} // namespace

std::vector<std::string>
mergeEnvironment(const std::vector<EnvOverride> &overrides) {
    std::vector<std::string> entries;
    std::vector<bool> applied(overrides.size(), false);

    const auto consider = [&](std::string_view text) {
        // Entries beginning with '=' are Windows' per-drive working directories
        // ("=C:=C:\work"). They are not user variables and must be passed
        // through untouched.
        const size_t equals
            = text.empty() ? std::string_view::npos : text.find('=', 1);
        if (text.empty() || text.front() == '=' || equals == std::string_view::npos) {
            entries.emplace_back(text);
            return;
        }

        const std::string_view name = text.substr(0, equals);
        const auto match = std::find_if(
            overrides.begin(), overrides.end(),
            [&name](const EnvOverride &o) { return namesMatch(o.first, name); });

        if (match == overrides.end()) {
            entries.emplace_back(text);
        } else {
            applied[static_cast<size_t>(match - overrides.begin())] = true;
            entries.push_back(match->first + "=" + match->second);
        }
    };

#ifdef _WIN32
    if (const char *environment = GetEnvironmentStringsA()) {
        for (const char *entry = environment; *entry != '\0';
             entry += std::strlen(entry) + 1) {
            consider(entry);
        }
        FreeEnvironmentStringsA(const_cast<LPCH>(environment));
    }
#else
    if (environ != nullptr) {
        for (char **entry = environ; *entry != nullptr; ++entry) {
            consider(*entry);
        }
    }
#endif

    // Overrides that did not replace anything inherited.
    for (size_t i = 0; i < overrides.size(); ++i) {
        if (!applied[i]) {
            entries.push_back(overrides[i].first + "=" + overrides[i].second);
        }
    }

    return entries;
}

} // namespace mx::detail
