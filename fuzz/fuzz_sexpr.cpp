// Fuzzes the reader of Maxima's replies and the mapping to Expr behind it.
//
// Every reply from the child process passes through here, so what matters is
// that no text can do worse than be refused: no crash, no hang, no overflow,
// and no exception but mx::Error, which the kernel turns into a failed call.

#include "wire/from_maxima.hpp"
#include "wire/sexpr.hpp"

#include <mx/errors.hpp>
#include <mx/expr.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char *>(data), size);
    try {
        const mx::detail::SExpr form = mx::detail::parseSExpr(text);
        static_cast<void>(form.toString());
        const mx::Expr expr = mx::detail::fromMaxima(form);
        static_cast<void>(expr.str());
    } catch (const mx::Error &) {
        // Refusing input that is not a Maxima term is the reader's job. Any
        // other exception escapes, and the fuzzer reports it as a crash.
    }
    return 0;
}
