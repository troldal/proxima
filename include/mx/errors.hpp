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
