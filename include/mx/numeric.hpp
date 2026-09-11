#pragma once

#include <mx/expr.hpp>
#include <mx/symbol.hpp>

#include <functional>
#include <map>
#include <string>

namespace mx {

/// Values for the symbols in an expression, by name.
///
/// Keyed on the name rather than on Symbol so that a lookup can be made from a
/// string_view without building a Symbol, and so that the common case reads as
/// `{{"x", 2.0}, {"y", 3.0}}`.
using Bindings = std::map<std::string, double, std::less<>>;

/// Evaluates an expression to a double.
///
/// Entirely local: no kernel, no round trip. That is the point — once Maxima
/// has produced a closed form, turning it into numbers is ordinary arithmetic,
/// and paying milliseconds per point to ask Maxima would make plotting or
/// integrating it numerically absurd.
///
/// Named constants are recognised as Maxima spells them: `%pi`, `%e`, `inf`,
/// `minf`. An explicit binding wins over them, so a symbol called `%e` can be
/// given a different value if that is genuinely what is wanted.
///
/// Throws mx::EvalError for anything it cannot turn into a number: an unbound
/// symbol, a function it does not know, a relation, or an Opaque node — the
/// last being Maxima source text this library never interpreted, which is
/// precisely why it cannot be evaluated here.
double evalNumeric(const Expr &expr, const Bindings &bindings = {});

/// True when evalNumeric could succeed: every symbol bound and every function
/// known. Cheaper than catching, when the caller wants to ask before committing.
bool isEvaluable(const Expr &expr, const Bindings &bindings = {});

/// Binds `expr` to one variable, for repeated evaluation.
///
/// A convenience over evalNumeric, not a faster path: the tree is still walked
/// per call. If that ever shows up in a profile, the expression can be printed
/// and handed to a dedicated evaluator — see the note in PLAN.md step 15.
std::function<double(double)> asFunction(const Expr &expr, const Symbol &variable,
                                         Bindings fixed = {});

} // namespace mx
