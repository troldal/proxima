#include "transport/process_env.hpp"

#include <boost/process/v2/environment.hpp>

#include <algorithm>
#include <cctype>
#include <string_view>

namespace proxima::detail {
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
    namespace environment = boost::process::v2::environment;

    std::vector<std::string> entries;
    std::vector<bool> applied(overrides.size(), false);

    // Boost.Process reads the environment through the wide-character API on
    // Windows and hands entries back as UTF-8, where GetEnvironmentStringsA
    // would have mangled anything outside the ANSI code page.
    for (const environment::key_value_pair_view entry : environment::current()) {
        const std::string text = entry.string();
        const std::string name = entry.key().string();

        // Entries with no ordinary name are Windows' per-drive working
        // directories ("=C:=C:\work"). They are not user variables and must be
        // passed through untouched.
        if (name.empty() || name.front() == '=') {
            entries.push_back(text);
            continue;
        }

        const auto match = std::find_if(
            overrides.begin(), overrides.end(),
            [&name](const EnvOverride &o) { return namesMatch(o.first, name); });

        if (match == overrides.end()) {
            entries.push_back(text);
        } else {
            applied[static_cast<size_t>(match - overrides.begin())] = true;
            entries.push_back(match->first + "=" + match->second);
        }
    }

    // Overrides that did not replace anything inherited.
    for (size_t i = 0; i < overrides.size(); ++i) {
        if (!applied[i]) {
            entries.push_back(overrides[i].first + "=" + overrides[i].second);
        }
    }

    return entries;
}

} // namespace proxima::detail
