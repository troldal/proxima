#pragma once

#include <proxima/expr.hpp>
#include <proxima/symbol.hpp>

#include <concepts>

namespace proxima {

/// Builders for the functions common enough that spelling them
/// `Expr::function("sin", {x})` every time would be noise.
///
/// Each is an uninterpreted application — nothing is evaluated here, and
/// `sin(0)` stays `sin(0)` until Maxima is asked. `exp` and `sqrt` are the
/// exceptions, and only because Maxima has no node for either: they are `%e^x`
/// and `x^(1/2)` internally, so building them that way keeps the two
/// representations in step.
///
/// They take an Expr or a Symbol and nothing else — proxima::ExprArgument, whose
/// note says why: they share their names with <cmath>, and must not be
/// candidates for a call on a plain number. For the square root of 2 as an
/// expression, write `proxima::sqrt(Expr(2))`.

namespace detail {
inline Expr applyNamed(const char *head, const Expr &argument) {
    return Expr::function(head, {argument});
}
} // namespace detail

template <ExprArgument T>
Expr sin(const T &x) {
    return detail::applyNamed("sin", x);
}
template <ExprArgument T>
Expr cos(const T &x) {
    return detail::applyNamed("cos", x);
}
template <ExprArgument T>
Expr tan(const T &x) {
    return detail::applyNamed("tan", x);
}
template <ExprArgument T>
Expr asin(const T &x) {
    return detail::applyNamed("asin", x);
}
template <ExprArgument T>
Expr acos(const T &x) {
    return detail::applyNamed("acos", x);
}
template <ExprArgument T>
Expr atan(const T &x) {
    return detail::applyNamed("atan", x);
}
template <ExprArgument T>
Expr sinh(const T &x) {
    return detail::applyNamed("sinh", x);
}
template <ExprArgument T>
Expr cosh(const T &x) {
    return detail::applyNamed("cosh", x);
}
template <ExprArgument T>
Expr tanh(const T &x) {
    return detail::applyNamed("tanh", x);
}
template <ExprArgument T>
Expr asinh(const T &x) {
    return detail::applyNamed("asinh", x);
}
template <ExprArgument T>
Expr acosh(const T &x) {
    return detail::applyNamed("acosh", x);
}
template <ExprArgument T>
Expr atanh(const T &x) {
    return detail::applyNamed("atanh", x);
}
template <ExprArgument T>
Expr log(const T &x) {
    return detail::applyNamed("log", x);
}
template <ExprArgument T>
Expr abs(const T &x) {
    return detail::applyNamed("abs", x);
}
template <ExprArgument T>
Expr erf(const T &x) {
    return detail::applyNamed("erf", x);
}
template <ExprArgument T>
Expr floor(const T &x) {
    return detail::applyNamed("floor", x);
}
/// Maxima's name, where <cmath> says ceil.
template <ExprArgument T>
Expr ceiling(const T &x) {
    return detail::applyNamed("ceiling", x);
}
/// -1, 0 or 1 by the sign of `x`.
template <ExprArgument T>
Expr signum(const T &x) {
    return detail::applyNamed("signum", x);
}

/// e^x. Maxima has no exp node either — it is %e^x internally.
template <ExprArgument T>
Expr exp(const T &x) {
    return pow(Expr::symbol("%e"), x);
}

template <ExprArgument T>
Expr sqrt(const T &x) {
    return pow(x, Expr::rational(1, 2));
}

/// The derivative of `f` with respect to `variable`, `order` times, *as a
/// noun*: Maxima's `'diff(f, x, n)`, which stays a derivative rather than
/// being computed. What a differential equation for proxima::ode2 is written with,
/// since `diff(y, x)` of a plain symbol `y` would simply evaluate to 0. For
/// the derivative computed, use proxima::diff.
inline Expr derivative(const Expr &f, const Expr &variable, unsigned order = 1) {
    return Expr::function("'diff", {f, variable, Expr(order)});
}

/// The constants, spelled as Maxima names them. Maxima knows two more that
/// have no builder, being rarer: `%phi`, the golden ratio, and `%gamma`, the
/// Euler–Mascheroni constant — `Expr::symbol("%phi")`. proxima::evalNumeric and
/// proxima::Compiled know all of them.
inline Expr pi() {
    return Expr::symbol("%pi");
}
inline Expr e() {
    return Expr::symbol("%e");
}
inline Expr i() {
    return Expr::symbol("%i");
}
inline Expr inf() {
    return Expr::symbol("inf");
}
/// Negative infinity, under Maxima's name for it, as inf() is.
inline Expr minf() {
    return Expr::symbol("minf");
}
[[deprecated("use proxima::minf(), which is Maxima's spelling, as inf() is")]]
inline Expr minusInf() {
    return minf();
}

} // namespace proxima
