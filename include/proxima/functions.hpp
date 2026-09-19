#pragma once

#include <proxima/expr.hpp>
#include <proxima/symbol.hpp>

#include <concepts>

namespace proxima {

/// @addtogroup expressions
/// @{

/// Builders for the functions common enough that spelling them
/// `Expr::function("sin", {x})` every time would be noise.
///
/// Each is an uninterpreted application — nothing is evaluated here, and
/// `sin(0)` stays `sin(0)` until Maxima is asked. `exp` and `sqrt` are the
/// exceptions, and only because Maxima has no node for either: they are `%e^x`
/// and `x^(1/2)` internally, so building them that way keeps the two
/// representations in step.
///
/// There is a builder for every function proxima::eval_numeric knows — the
/// list proxima::numeric_functions() gives, which a test holds these to — so
/// whatever can be built here can be evaluated without Maxima.
///
/// They take an Expr or a Symbol and nothing else — proxima::ExprArgument, whose
/// note says why: they share their names with <cmath>, and must not be
/// candidates for a call on a plain number. For the square root of 2 as an
/// expression, write `proxima::sqrt(Expr(2))`.

namespace detail {
inline Expr apply_named(const char *head, const Expr &argument) {
    return Expr::function(head, {argument});
}
} // namespace detail

template <ExprArgument T>
Expr sin(const T &x) {
    return detail::apply_named("sin", x);
}
template <ExprArgument T>
Expr cos(const T &x) {
    return detail::apply_named("cos", x);
}
template <ExprArgument T>
Expr tan(const T &x) {
    return detail::apply_named("tan", x);
}
template <ExprArgument T>
Expr asin(const T &x) {
    return detail::apply_named("asin", x);
}
template <ExprArgument T>
Expr acos(const T &x) {
    return detail::apply_named("acos", x);
}
template <ExprArgument T>
Expr atan(const T &x) {
    return detail::apply_named("atan", x);
}
template <ExprArgument T>
Expr sinh(const T &x) {
    return detail::apply_named("sinh", x);
}
template <ExprArgument T>
Expr cosh(const T &x) {
    return detail::apply_named("cosh", x);
}
template <ExprArgument T>
Expr tanh(const T &x) {
    return detail::apply_named("tanh", x);
}
template <ExprArgument T>
Expr asinh(const T &x) {
    return detail::apply_named("asinh", x);
}
template <ExprArgument T>
Expr acosh(const T &x) {
    return detail::apply_named("acosh", x);
}
template <ExprArgument T>
Expr atanh(const T &x) {
    return detail::apply_named("atanh", x);
}
template <ExprArgument T>
Expr log(const T &x) {
    return detail::apply_named("log", x);
}
template <ExprArgument T>
Expr abs(const T &x) {
    return detail::apply_named("abs", x);
}
template <ExprArgument T>
Expr erf(const T &x) {
    return detail::apply_named("erf", x);
}
template <ExprArgument T>
Expr floor(const T &x) {
    return detail::apply_named("floor", x);
}
/// Maxima's name, where `<cmath>` says ceil.
template <ExprArgument T>
Expr ceiling(const T &x) {
    return detail::apply_named("ceiling", x);
}
/// -1, 0 or 1 by the sign of `x`.
template <ExprArgument T>
Expr signum(const T &x) {
    return detail::apply_named("signum", x);
}
/// Maxima's round, which takes a half to the even neighbour: round(2.5) is 2.
template <ExprArgument T>
Expr round(const T &x) {
    return detail::apply_named("round", x);
}

// The reciprocal functions and their inverses, which Maxima writes into its
// answers rather than the quotients: the integral of tan(x) is log(sec(x)).
// The inverses are Maxima's: acot(x) is atan(1/x), so acot(-1) is -%pi/4.

template <ExprArgument T>
Expr sec(const T &x) {
    return detail::apply_named("sec", x);
}
template <ExprArgument T>
Expr csc(const T &x) {
    return detail::apply_named("csc", x);
}
template <ExprArgument T>
Expr cot(const T &x) {
    return detail::apply_named("cot", x);
}
template <ExprArgument T>
Expr asec(const T &x) {
    return detail::apply_named("asec", x);
}
template <ExprArgument T>
Expr acsc(const T &x) {
    return detail::apply_named("acsc", x);
}
template <ExprArgument T>
Expr acot(const T &x) {
    return detail::apply_named("acot", x);
}
template <ExprArgument T>
Expr sech(const T &x) {
    return detail::apply_named("sech", x);
}
template <ExprArgument T>
Expr csch(const T &x) {
    return detail::apply_named("csch", x);
}
template <ExprArgument T>
Expr coth(const T &x) {
    return detail::apply_named("coth", x);
}
template <ExprArgument T>
Expr asech(const T &x) {
    return detail::apply_named("asech", x);
}
template <ExprArgument T>
Expr acsch(const T &x) {
    return detail::apply_named("acsch", x);
}
template <ExprArgument T>
Expr acoth(const T &x) {
    return detail::apply_named("acoth", x);
}

/// The gamma function; gamma(n) is (n - 1)! for a positive integer n.
template <ExprArgument T>
Expr gamma(const T &x) {
    return detail::apply_named("gamma", x);
}
/// `x!`, and gamma(x + 1) for an `x` that is not an integer.
template <ExprArgument T>
Expr factorial(const T &x) {
    return detail::apply_named("factorial", x);
}
/// `x!!`: x (x - 2) (x - 4) ..., down to 1 or 2.
template <ExprArgument T>
Expr double_factorial(const T &x) {
    return detail::apply_named("double_factorial", x);
}
/// The complementary error function, 1 - erf(x).
template <ExprArgument T>
Expr erfc(const T &x) {
    return detail::apply_named("erfc", x);
}

// Parts of a complex number. Maxima simplifies them away for a symbol it knows
// to be real, and keeps them for one it does not.

template <ExprArgument T>
Expr realpart(const T &x) {
    return detail::apply_named("realpart", x);
}
template <ExprArgument T>
Expr imagpart(const T &x) {
    return detail::apply_named("imagpart", x);
}
template <ExprArgument T>
Expr conjugate(const T &x) {
    return detail::apply_named("conjugate", x);
}
/// The modulus of a complex number.
template <ExprArgument T>
Expr cabs(const T &x) {
    return detail::apply_named("cabs", x);
}
/// The argument of a complex number, in (-%pi, %pi].
template <ExprArgument T>
Expr carg(const T &x) {
    return detail::apply_named("carg", x);
}

// Functions of several arguments. As for pow, either argument may be a plain
// number, but not every one: `mod(x, 3)` is an expression, and `mod(7, 3)`
// has no candidate here.

/// The angle of the point (x, y), in (-%pi, %pi]: note y first, as in C.
template <typename Y, typename X>
    requires(ExprArgument<Y> || ExprArgument<X>)
            && std::convertible_to<const Y &, Expr>
            && std::convertible_to<const X &, Expr>
Expr atan2(const Y &y, const X &x) {
    return Expr::function("atan2", {Expr(y), Expr(x)});
}

/// Maxima's mod, whose remainder takes the sign of the divisor: mod(-7, 3) is 2.
template <typename A, typename B>
    requires(ExprArgument<A> || ExprArgument<B>)
            && std::convertible_to<const A &, Expr>
            && std::convertible_to<const B &, Expr>
Expr mod(const A &dividend, const B &divisor) {
    return Expr::function("mod", {Expr(dividend), Expr(divisor)});
}

/// The largest of its arguments, however many.
template <typename... Ts>
    requires(ExprArgument<Ts> || ...)
            && (std::convertible_to<const Ts &, Expr> && ...)
Expr max(const Ts &...xs) {
    return Expr::function("max", {Expr(xs)...});
}

/// The smallest of its arguments, however many.
template <typename... Ts>
    requires(ExprArgument<Ts> || ...)
            && (std::convertible_to<const Ts &, Expr> && ...)
Expr min(const Ts &...xs) {
    return Expr::function("min", {Expr(xs)...});
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
///
/// The variable is a Symbol, as for every other calculus operation, so
/// `derivative(y, x * 2)` does not compile.
inline Expr derivative(const Expr &f, const Symbol &variable, unsigned order = 1) {
    return Expr::function("'diff", {f, variable, Expr(order)});
}

/// The constants, spelled as Maxima names them. Maxima knows two more that
/// have no builder, being rarer: `%phi`, the golden ratio, and `%gamma`, the
/// Euler–Mascheroni constant — `Expr::symbol("%phi")`. proxima::eval_numeric and
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
inline Expr minus_inf() {
    return minf();
}

/// @}

} // namespace proxima
