#pragma once

// The library's own ways in to a Kernel, beneath the public ask and tell.

#include <proxima/assumptions.hpp>
#include <proxima/kernel.hpp>
#include <proxima/result.hpp>

#include "kernel/session.hpp"

#include <string_view>

namespace proxima::detail {

/// Reads Maxima's wire text — its internal s-expression — into an Expr.
///
/// Throws proxima::ParseError if the text is not a Maxima term. No reply from
/// a kernel should be one: it would mean the protocol itself had failed.
result<Expr> to_expr(std::string_view wire);

/// What the session needs to answer under `assumptions`: a key that names
/// them — their canonical text, the same for equal values — and the declare
/// and assume statements that establish them in a fresh context.
Environment environment_for(const Assumptions &assumptions);

} // namespace proxima::detail
