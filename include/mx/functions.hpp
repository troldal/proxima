#pragma once

#include <mx/expr.hpp>

namespace mx {

/// Builders for the handful of functions common enough that spelling them
/// `Expr::function("sin", {x})` every time would be noise.
///
/// Each is an uninterpreted application — nothing is evaluated here, and
/// `sin(0)` stays `sin(0)` until Maxima is asked. `sqrt` is the exception, and
/// only because Maxima has no sqrt node either: it is `x^(1/2)` internally, so
/// building it that way keeps the two representations in step.

inline Expr sin(const Expr &x) {
    return Expr::function("sin", {x});
}
inline Expr cos(const Expr &x) {
    return Expr::function("cos", {x});
}
inline Expr tan(const Expr &x) {
    return Expr::function("tan", {x});
}
inline Expr asin(const Expr &x) {
    return Expr::function("asin", {x});
}
inline Expr acos(const Expr &x) {
    return Expr::function("acos", {x});
}
inline Expr atan(const Expr &x) {
    return Expr::function("atan", {x});
}
inline Expr sinh(const Expr &x) {
    return Expr::function("sinh", {x});
}
inline Expr cosh(const Expr &x) {
    return Expr::function("cosh", {x});
}
inline Expr log(const Expr &x) {
    return Expr::function("log", {x});
}
inline Expr abs(const Expr &x) {
    return Expr::function("abs", {x});
}

/// e^x. Maxima has no exp node either — it is %e^x internally.
inline Expr exp(const Expr &x) {
    return pow(Expr::symbol("%e"), x);
}

inline Expr sqrt(const Expr &x) {
    return pow(x, Expr::rational(1, 2));
}

/// The derivative of `f` with respect to `variable`, `order` times, *as a
/// noun*: Maxima's `'diff(f, x, n)`, which stays a derivative rather than
/// being computed. What a differential equation for mx::ode2 is written with,
/// since `diff(y, x)` of a plain symbol `y` would simply evaluate to 0. For
/// the derivative computed, use mx::diff.
inline Expr derivative(const Expr &f, const Expr &variable, unsigned order = 1) {
    return Expr::function("'diff", {f, variable, Expr(order)});
}

/// The constants, spelled as Maxima names them.
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
inline Expr minusInf() {
    return Expr::symbol("minf");
}

} // namespace mx
