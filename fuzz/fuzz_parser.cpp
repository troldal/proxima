// Fuzzes Expr::parse, the offline reader of infix text.
//
// Two properties. Any text is parsed or refused with proxima::Error — nothing else.
// And what parses prints back to text that parses to the same expression: the
// printer's promise, which the round-trip tests check on a fixed list and this
// checks on whatever the fuzzer invents.

#include <proxima/errors.hpp>
#include <proxima/expr.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

[[noreturn]] void fail(std::string_view input, const std::string &printed,
                       const char *what) {
    std::fprintf(stderr, "round trip failed: %s\n  input:   %.*s\n  printed: %s\n",
                 what, static_cast<int>(input.size()), input.data(), printed.c_str());
    std::abort();
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char *>(data), size);

    proxima::Expr parsed;
    try {
        parsed = proxima::Expr::parse(text);
    } catch (const proxima::Error &) {
        return 0; // Refused, which is a fine answer to most text.
    }

    const std::string printed = parsed.str();
    try {
        if (!(proxima::Expr::parse(printed) == parsed)) {
            fail(text, printed, "the printed text parses to a different expression");
        }
    } catch (const proxima::Error &error) {
        fail(text, printed, error.what());
    }
    return 0;
}
