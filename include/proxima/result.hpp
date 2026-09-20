#pragma once

// How every operation in this library reports that it produced no answer.
//
// A failure is a value, not an exception. Maxima genuinely cannot integrate
// every integrand or solve every equation, text is not always well formed, and
// an expression with a free symbol has no number in it: those are ordinary
// outcomes, and an ordinary outcome is returned. Only infrastructure failures
// — the kernel died, the protocol broke — are thrown, as proxima::KernelError.
//
// The types are FXT's, not this library's own. proxima::Failure *is*
// fxt::failure and proxima::result<T> *is* fxt::result<T>, so a Proxima result
// takes part in every FXT pipeline directly: fxt::and_then, fxt::transform,
// fxt::match, fxt::attempt, fxt::with and the rest, with nothing to convert.
// What Proxima adds is a Cause, carried as the failure's context, so a caller
// can tell "no closed form" from "Maxima needs an assumption" without reading
// the message.

// The file is `failure.hpp` in FXT's repository, though FXT's own headers
// include it as `Failure.hpp`: a case mismatch that a case-insensitive
// filesystem hides and Linux does not. The tracked spelling works on both.
//
// <ostream> first: fxt::failure defines an inline `operator<<` that streams
// a string_view, and MSVC's <string_view> needs basic_ostream complete to
// instantiate it. FXT's own header includes only <string_view>, which
// libstdc++ forgives and MSVC's STL does not.
#include <ostream>

#include <fxt/monads/Expected.hpp>
#include <fxt/utils/Failure.hpp>

#include <string>
#include <string_view>
#include <utility>

#ifdef FXT_USE_TL_EXPECTED
#error                                                                              \
    "Proxima is built on std::expected; FXT_USE_TL_EXPECTED would make its result type a different one"
#endif

namespace proxima {

/// @addtogroup results
/// @{

/// What went wrong, in terms a program can branch on.
enum class Cause {
    /// A failure made without a cause — by other code, or by fxt::attempt.
    Unknown,
    /// Maxima signalled an error while evaluating, and the message is its
    /// wording: a malformed argument, an undefined operation.
    MaximaError,
    /// Maxima needed a fact it had not been told — "Is n equal to -1?" — and
    /// over a pipe cannot ask. The message names the question; the answer is
    /// to ask again under proxima::Assumptions that settle it.
    NeedsAssumption,
    /// The assumptions an operation was asked under contradict one another,
    /// so it was not asked at all: `assuming({gt(x, 0), lt(x, 0)})`.
    Inconsistent,
    /// integrate, sum, product or limit found no closed form: Maxima handed
    /// the operation back unevaluated.
    NoClosedForm,
    /// solve or ode2 did not solve it: an equation still mentioning the
    /// unknown, or `false`.
    NotSolved,
    /// The limit does not exist: Maxima's `und` or `ind`.
    NoLimit,
    /// Maxima answered, but not with something of the shape the operation
    /// promised: a non-truth from `is`, a non-count from `nroots`.
    UnexpectedAnswer,
    /// Text could not be read as an expression. The message says where.
    Parse,
    /// An expression could not be turned into a number: a symbol with no
    /// value, an unknown function, a relation, an Opaque node.
    Eval,
    /// An argument was not usable: no unknowns to solve for, an empty
    /// interval.
    Argument,
    /// Folding the numbers in the text left the range of a double: the
    /// parser's report of what the operators throw as proxima::OverflowError.
    Overflow,
};

/// The cause's name, for messages and logs: "NoClosedForm", "Parse".
std::string_view to_string(Cause cause);

/// Why an operation produced no result: fxt::failure, carrying a message —
/// usually Maxima's own wording — and a proxima::Cause as its context.
using Failure = fxt::failure;

/// A value or a Failure: fxt::result<T>, that is fxt::expected<T, fxt::failure>.
template <typename T>
using result = fxt::expected<T, Failure>;

/// A Failure with its cause attached.
inline Failure fail(Cause cause, std::string message) {
    return Failure(std::move(message)).with_context(cause);
}

/// The failure's cause, or Cause::Unknown for one that carries none.
inline Cause cause_of(const Failure &failure) {
    return failure.get_context<Cause>().value_or(Cause::Unknown);
}

/// The value, or the failure thrown as the exception its cause names:
/// proxima::ParseError for Cause::Parse, proxima::EvalError for Cause::Eval,
/// proxima::OverflowError for Cause::Overflow, proxima::MaximaError for
/// everything else. The one place this library
/// throws those for an ordinary outcome, for callers who would rather catch
/// than check.
template <typename T>
T unwrap(result<T> value);

} // namespace proxima

#include <proxima/errors.hpp>

namespace proxima {

template <typename T>
T unwrap(result<T> value) {
    if (value) {
        return std::move(*value);
    }
    const Failure &failure = value.error();
    switch (cause_of(failure)) {
    case Cause::Parse:
        throw ParseError(failure.message());
    case Cause::Eval:
        throw EvalError(failure.message());
    case Cause::Overflow:
        throw OverflowError(failure.message());
    default:
        throw MaximaError(failure.message());
    }
}

/// @}

} // namespace proxima
