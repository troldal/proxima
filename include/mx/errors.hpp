#pragma once

#include <stdexcept>

namespace mx {

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
/// and will be reported through std::expected instead. See PLAN.md step 11.
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

/// Maxima signalled an error while evaluating.
///
/// Thrown only by the operations that have no ordinary way to fail — diff,
/// expand, subst and the like. The ones that *can* ordinarily fail, such as
/// integrate and solve, report it as an mx::Failure instead, because "there is
/// no closed form" is an answer rather than a malfunction.
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

} // namespace mx
