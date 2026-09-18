#pragma once

#include <proxima/result.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace proxima::detail {

/// The outcome of one Maxima evaluation, as the session records it.
///
/// Internal: the public Kernel hands out a proxima::result<std::string>. This
/// is the shape the reply cache and the persistent cache store, which is why
/// it is plain data rather than an expected.
struct Reply {
    /// False when Maxima signalled an error while evaluating.
    bool ok = false;

    /// The result, as the text of Maxima's own internal s-expression, e.g.
    /// "((MTIMES SIMP) 2 $X ((%SIN SIMP) $X))". Empty when `ok` is false.
    std::string value;

    /// Maxima's own rendered message when `ok` is false, e.g.
    /// "integrate: variable must not be a number; found: 5". Empty on success.
    std::string reason;
};

/// The reply as the public result: the wire text, or Maxima's message with
/// its cause.
///
/// The one message that is not Maxima's own is the Lisp helper's, when Maxima
/// wanted to ask a question: that is the fact the caller has to supply, and
/// the cause says so, so nobody has to recognise the wording.
inline result<std::string> to_result(Reply reply) {
    if (reply.ok) {
        return std::move(reply.value);
    }
    constexpr std::string_view kQuestion = "this computation needs an assumption";
    const Cause cause = std::string_view(reply.reason).starts_with(kQuestion)
                            ? Cause::NeedsAssumption
                            : Cause::MaximaError;
    return fxt::unexpected(fail(cause, std::move(reply.reason)));
}

} // namespace proxima::detail
