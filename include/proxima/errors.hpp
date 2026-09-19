#pragma once

#include <stdexcept>

namespace proxima {

/// @addtogroup results
/// @{

/// Base of every exception thrown by this library.
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// The Maxima kernel could not be started, has died, or the protocol
/// desynchronised.
///
/// This is an infrastructure failure, deliberately distinct from a
/// mathematical one ("no closed form exists"), which is an ordinary outcome
/// and is reported as an proxima::Failure through std::expected instead.
class KernelError : public Error {
public:
    using Error::Error;
};

/// Maxima did not answer within Config::timeout.
///
/// A KernelError, because the conversation has broken down rather than the
/// mathematics having failed: nothing can be said about whether the computation
/// would ever have finished. The kernel is restarted and its assumption state
/// replayed before this is thrown, so the *next* call starts from a working
/// session — only this one is lost.
class TimeoutError : public KernelError {
public:
    using KernelError::KernelError;
};

/// Maxima produced no answer, and the caller asked for the value anyway.
///
/// Thrown by proxima::unwrap on a result whose Cause is not one of the
/// others' (Parse, Eval, Overflow). Nothing in this library throws it of its
/// own accord: every operation returns a proxima::result, and this is the
/// exception a caller who prefers catching to checking gets from it.
class MaximaError : public Error {
public:
    using Error::Error;
};

/// An expression could not be turned into a number.
///
/// A symbol with no value, a function with no numeric meaning here, a relation,
/// or an Opaque node — Maxima source this library never interpreted, and so has
/// no way to evaluate. Local: nothing to do with the kernel.
class EvalError : public Error {
public:
    using Error::Error;
};

/// Text that should have been a Maxima reply could not be read as one.
///
/// Distinct from KernelError because the causes differ: a KernelError means the
/// conversation broke down, a ParseError means it carried something this
/// library cannot interpret. In practice the latter means either a Maxima
/// construct the reader does not yet handle, or a genuine protocol bug.
class ParseError : public Error {
public:
    using Error::Error;
};

/// A fold of floating-point numbers left the range of a double: `1e308 *
/// 10.0`, or an integer of a few hundred digits multiplied by a real. Thrown
/// by the arithmetic operators and builders, since a value cannot be
/// returned from them — and distinct, so a caller can tell it from misuse.
/// Maxima refuses the same arithmetic.
class OverflowError : public Error {
public:
    using Error::Error;
};

/// @}

} // namespace proxima
