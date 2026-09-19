#include <proxima/numeric.hpp>

#include <proxima/errors.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <concepts>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace proxima {
namespace {

using detail::Instruction;

// --- what a function means ------------------------------------------------
//
// One table, consulted by both the one-shot evaluator and the compiler, so the
// two cannot end up disagreeing about what `log` is.
//
// Deliberately a closed set: an unknown head is an error rather than a guess.
// Silently returning something plausible for a function that is not actually
// implemented would be far worse than refusing.

struct Builtin {
    std::string_view name;
    int arity; ///< -1 for variadic, needing at least one argument.
    double (*apply)(const double *args, std::size_t count);
};

template <double (*Fn)(double)>
double unary(const double *args, std::size_t) {
    return Fn(args[0]);
}

template <double (*Fn)(double, double)>
double binary(const double *args, std::size_t) {
    return Fn(args[0], args[1]);
}

double signum_of(const double *args, std::size_t) {
    return static_cast<double>((args[0] > 0) - (args[0] < 0));
}

// mod and round are not their <cmath> namesakes. std::fmod truncates towards
// zero and std::round rounds halves away from zero; Maxima does neither, so a
// closed form containing either used to evaluate to one number here and
// another in Maxima — mod(-7, 3) was -1 against Maxima's 2, round(2.5) was 3
// against its 2. Both behaviours below were checked against Maxima itself, and
// test_numeric.cpp keeps checking them.

/// Maxima's mod: the remainder takes the sign of the divisor, and mod(x, 0) is
/// x rather than NaN.
///
/// Computed as fmod plus a sign correction rather than as x - y*floor(x/y),
/// which is how Maxima computes it for floats. The two agree except where that
/// formula loses precision: mod(1e20, 3) is exactly 1, since 1e20 is exactly
/// representable, but Maxima's floating arithmetic answers 0.0. This returns 1.
double floored_modulo(const double *args, std::size_t) {
    const double dividend = args[0];
    const double divisor = args[1];
    if (divisor == 0.0) {
        return dividend;
    }
    double remainder = std::fmod(dividend, divisor); // Exact; dividend's sign.
    if (remainder != 0.0 && (remainder < 0.0) != (divisor < 0.0)) {
        remainder += divisor;
    }
    // A zero takes the divisor's sign too, so mod(-6, 3) is 0 and not -0.
    return remainder == 0.0 ? std::copysign(0.0, divisor) : remainder;
}

/// Maxima's round: halves go to the nearest even integer, in both directions.
///
/// Written out rather than delegated to std::nearbyint, which would do the
/// same only while the floating-point rounding mode is the default — a mode
/// the host program is free to change.
double round_half_to_even(const double *args, std::size_t) {
    const double value = args[0];
    const double below = std::floor(value);
    // Exact: below is within one of value, so no bits are lost. For infinities
    // and NaN this is NaN, which falls through to the last branch and returns
    // the argument's own infinity or NaN.
    const double fraction = value - below;
    double result;
    if (fraction < 0.5) {
        result = below;
    } else if (fraction > 0.5) {
        result = below + 1.0;
    } else {
        result = std::fmod(below, 2.0) == 0.0 ? below : below + 1.0;
    }
    // -0.5 rounds to 0, and Maxima's 0 has no sign.
    return result + 0.0;
}

// The reciprocal functions, which Maxima prefers to write rather than their
// quotients: integrate(tan(x), x) is log(sec(x)). The inverses are Maxima's
// definitions, acot(x) = atan(1/x) and so on — which is also why acot(-1) is
// -pi/4 here and in Maxima, not the 3pi/4 of the other common convention.

double secant(double x) {
    return 1.0 / std::cos(x);
}
double cosecant(double x) {
    return 1.0 / std::sin(x);
}
double cotangent(double x) {
    return 1.0 / std::tan(x);
}
double hyperbolic_secant(double x) {
    return 1.0 / std::cosh(x);
}
double hyperbolic_cosecant(double x) {
    return 1.0 / std::sinh(x);
}
double hyperbolic_cotangent(double x) {
    // Not cosh/sinh, which is inf/inf, and NaN, from |x| of about 710.
    return 1.0 / std::tanh(x);
}
double arc_secant(double x) {
    return std::acos(1.0 / x);
}
double arc_cosecant(double x) {
    return std::asin(1.0 / x);
}
double arc_cotangent(double x) {
    return std::atan(1.0 / x);
}
double area_secant(double x) {
    return std::acosh(1.0 / x);
}
double area_cosecant(double x) {
    return std::asinh(1.0 / x);
}
double area_cotangent(double x) {
    return std::atanh(1.0 / x);
}

/// The largest n with n! exactly a double: 22! has an odd part below 2^53.
constexpr int kExactFactorials = 22;

/// The largest n with n!! a finite double: 301!! is about 1.1e309.
constexpr int kFiniteDoubleFactorials = 300;

/// Gamma, exact where it is an integer that fits: gamma(5) is 24 and not
/// whatever the platform's tgamma makes of it. The products are exact because
/// every partial product is a smaller factorial, which also fits.
double gamma_of(double x) {
    if (x >= 1 && x <= kExactFactorials + 1 && x == std::floor(x)) {
        const int n = static_cast<int>(x);
        double product = 1.0;
        for (int factor = 2; factor < n; ++factor) {
            product *= factor;
        }
        return product;
    }
    return std::tgamma(x);
}

double factorial_of(double x) {
    return gamma_of(x + 1.0);
}

/// Maxima's double factorial, n (n-2) (n-4) ... for an integer n from -1 up,
/// and otherwise the continuation Maxima uses:
///
///     z!! = 2^(z/2) (2/pi)^((1 - cos(pi z))/4) gamma(z/2 + 1)
///
/// which agrees with the product at every integer from -1 up, but only to
/// rounding, so the product is taken where it applies.
double double_factorial_of(double x) {
    if (x >= -1 && x == std::floor(x)) {
        if (x > kFiniteDoubleFactorials) {
            return std::numeric_limits<double>::infinity();
        }
        double product = 1.0;
        for (int factor = static_cast<int>(x); factor > 1; factor -= 2) {
            product *= factor;
        }
        return product;
    }
    return std::pow(2.0, x / 2)
           * std::pow(2.0 / std::numbers::pi,
                      (1.0 - std::cos(std::numbers::pi * x)) / 4)
           * gamma_of(x / 2 + 1);
}

// realpart and friends, for real arguments. Maxima writes them into a result
// about a symbol it does not know to be real; their complex meanings are in
// the complex table further down.

double real_part(double x) {
    return x;
}
double imaginary_part(double) {
    return 0.0;
}
/// Maxima's carg: pi for a negative number, 0 otherwise.
double argument_of(double x) {
    return std::atan2(0.0, x);
}

double maximum_of(const double *args, std::size_t count) {
    double result = args[0];
    for (std::size_t i = 1; i < count; ++i) {
        result = std::fmax(result, args[i]);
    }
    return result;
}

double minimum_of(const double *args, std::size_t count) {
    double result = args[0];
    for (std::size_t i = 1; i < count; ++i) {
        result = std::fmin(result, args[i]);
    }
    return result;
}

/// Every function either evaluator knows. numeric_functions() hands the names
/// out, and a test checks <proxima/functions.hpp> has a builder for each.
constexpr Builtin kBuiltins[] = {
    // clang-format off
    {"sin", 1, unary<std::sin>},       {"cos", 1, unary<std::cos>},
    {"tan", 1, unary<std::tan>},       {"sec", 1, unary<secant>},
    {"csc", 1, unary<cosecant>},       {"cot", 1, unary<cotangent>},
    {"asin", 1, unary<std::asin>},     {"acos", 1, unary<std::acos>},
    {"atan", 1, unary<std::atan>},     {"asec", 1, unary<arc_secant>},
    {"acsc", 1, unary<arc_cosecant>},  {"acot", 1, unary<arc_cotangent>},
    {"atan2", 2, binary<std::atan2>},

    {"sinh", 1, unary<std::sinh>},     {"cosh", 1, unary<std::cosh>},
    {"tanh", 1, unary<std::tanh>},     {"sech", 1, unary<hyperbolic_secant>},
    {"csch", 1, unary<hyperbolic_cosecant>},
    {"coth", 1, unary<hyperbolic_cotangent>},
    {"asinh", 1, unary<std::asinh>},   {"acosh", 1, unary<std::acosh>},
    {"atanh", 1, unary<std::atanh>},   {"asech", 1, unary<area_secant>},
    {"acsch", 1, unary<area_cosecant>},
    {"acoth", 1, unary<area_cotangent>},

    {"exp", 1, unary<std::exp>},       {"log", 1, unary<std::log>},
    {"sqrt", 1, unary<std::sqrt>},

    {"gamma", 1, unary<gamma_of>},     {"factorial", 1, unary<factorial_of>},
    {"double_factorial", 1, unary<double_factorial_of>},
    {"erf", 1, unary<std::erf>},       {"erfc", 1, unary<std::erfc>},

    {"abs", 1, unary<std::fabs>},      {"signum", 1, signum_of},
    {"floor", 1, unary<std::floor>},   {"ceiling", 1, unary<std::ceil>},
    {"round", 1, round_half_to_even},  {"mod", 2, floored_modulo},
    {"max", -1, maximum_of},           {"min", -1, minimum_of},

    {"realpart", 1, unary<real_part>}, {"imagpart", 1, unary<imaginary_part>},
    {"conjugate", 1, unary<real_part>},
    {"cabs", 1, unary<std::fabs>},     {"carg", 1, unary<argument_of>},
    // clang-format on
};

/// The names of kBuiltins, for numeric_functions().
constexpr auto kBuiltinNames = [] {
    std::array<std::string_view, std::size(kBuiltins)> names{};
    for (std::size_t i = 0; i < names.size(); ++i) {
        names[i] = kBuiltins[i].name;
    }
    return names;
}();

// --- the same, over the complex numbers -----------------------------------
//
// The functions with a complex meaning, for eval_complex. Every one is also in
// kBuiltins; a function that is not here — floor, gamma, mod — is evaluated by
// its real version, when its arguments are real, and refused otherwise.

using Complex = std::complex<double>;

// Branch cuts. Off the real axis, std::asin and the rest agree with Maxima.
// On it — a real argument, exactly on a cut — std:: goes by the sign of the
// zero imaginary part, which a real number in Maxima does not have, and not
// every standard library goes the same way: MinGW's acosh(0.5) is -1.047*%i,
// glibc's +1.047*%i. So the functions with a cut on the real axis compute a
// real argument's value from real functions, on the side Maxima takes. That
// is the upper side, except for asin, acos and atanh above 1, where it is the
// lower: asin(2.0) is 1.571 - 1.317*%i. Common Lisp's rule of
// counter-clockwise continuity, which Maxima follows. The test against Maxima
// covers both sides of every cut.

constexpr double kHalfPi = std::numbers::pi / 2;

Complex arc_sine(Complex z) {
    if (z.imag() != 0.0) {
        return std::asin(z);
    }
    const double x = z.real();
    if (x > 1.0) {
        return {kHalfPi, -std::acosh(x)};
    }
    if (x < -1.0) {
        return {-kHalfPi, std::acosh(-x)};
    }
    return std::asin(x);
}

Complex arc_cosine(Complex z) {
    if (z.imag() != 0.0) {
        return std::acos(z);
    }
    const double x = z.real();
    if (x > 1.0) {
        return {0.0, std::acosh(x)};
    }
    if (x < -1.0) {
        return {std::numbers::pi, -std::acosh(-x)};
    }
    return std::acos(x);
}

Complex area_cosine(Complex z) {
    if (z.imag() != 0.0) {
        return std::acosh(z);
    }
    const double x = z.real();
    if (x < -1.0) {
        return {std::acosh(-x), std::numbers::pi};
    }
    if (x < 1.0) {
        return {0.0, std::acos(x)};
    }
    return std::acosh(x);
}

Complex area_tangent(Complex z) {
    if (z.imag() != 0.0) {
        return std::atanh(z);
    }
    const double x = z.real();
    // Beyond ±1, atanh(1/x) is the real part: (1/2) log((x + 1)/(x - 1)).
    if (x > 1.0) {
        return {std::atanh(1.0 / x), -kHalfPi};
    }
    if (x < -1.0) {
        return {std::atanh(1.0 / x), kHalfPi};
    }
    return std::atanh(x);
}

Complex logarithm(Complex z) {
    if (z.imag() == 0.0 && z.real() < 0.0) {
        return {std::log(-z.real()), std::numbers::pi};
    }
    return std::log(z);
}

Complex square_root(Complex z) {
    if (z.imag() == 0.0 && z.real() < 0.0) {
        return {0.0, std::sqrt(-z.real())};
    }
    return std::sqrt(z);
}

/// 1/z, real for a real z, so that acsc(x) is asin(1/x) on the right side of
/// the cut: 1.0/z would give it a zero imaginary part of either sign.
Complex reciprocal(Complex z) {
    return z.imag() == 0.0 ? Complex(1.0 / z.real(), 0.0) : 1.0 / z;
}

struct ComplexBuiltin {
    std::string_view name;
    Complex (*apply)(Complex);
};

constexpr ComplexBuiltin kComplexBuiltins[] = {
    // clang-format off
    {"sin", [](Complex z) { return std::sin(z); }},
    {"cos", [](Complex z) { return std::cos(z); }},
    {"tan", [](Complex z) { return std::tan(z); }},
    {"sec", [](Complex z) { return 1.0 / std::cos(z); }},
    {"csc", [](Complex z) { return 1.0 / std::sin(z); }},
    {"cot", [](Complex z) { return 1.0 / std::tan(z); }},
    {"asin", arc_sine},
    {"acos", arc_cosine},
    {"atan", [](Complex z) { return std::atan(z); }},
    {"asec", [](Complex z) { return arc_cosine(reciprocal(z)); }},
    {"acsc", [](Complex z) { return arc_sine(reciprocal(z)); }},
    {"acot", [](Complex z) { return std::atan(reciprocal(z)); }},
    {"sinh", [](Complex z) { return std::sinh(z); }},
    {"cosh", [](Complex z) { return std::cosh(z); }},
    {"tanh", [](Complex z) { return std::tanh(z); }},
    {"sech", [](Complex z) { return 1.0 / std::cosh(z); }},
    {"csch", [](Complex z) { return 1.0 / std::sinh(z); }},
    {"coth", [](Complex z) { return 1.0 / std::tanh(z); }},
    {"asinh", [](Complex z) { return std::asinh(z); }},
    {"acosh", area_cosine},
    {"atanh", area_tangent},
    {"asech", [](Complex z) { return area_cosine(reciprocal(z)); }},
    {"acsch", [](Complex z) { return std::asinh(reciprocal(z)); }},
    {"acoth", [](Complex z) { return area_tangent(reciprocal(z)); }},
    {"exp", [](Complex z) { return std::exp(z); }},
    {"log", logarithm},
    {"sqrt", square_root},
    {"abs", [](Complex z) { return Complex(std::abs(z)); }},
    {"cabs", [](Complex z) { return Complex(std::abs(z)); }},
    {"carg", [](Complex z) {
        // pi for a negative real, whichever the sign of its zero.
        return Complex(z.imag() == 0.0 ? argument_of(z.real()) : std::arg(z));
    }},
    {"realpart", [](Complex z) { return Complex(z.real()); }},
    {"imagpart", [](Complex z) { return Complex(z.imag()); }},
    {"conjugate", [](Complex z) { return std::conj(z); }},
    // clang-format on
};

static_assert(
    [] {
        for (const ComplexBuiltin &complex : kComplexBuiltins) {
            bool found = false;
            for (const Builtin &real : kBuiltins) {
                found = found || (real.name == complex.name && real.arity == 1);
            }
            if (!found) {
                return false;
            }
        }
        return true;
    }(),
    "a function with a complex meaning must have a real one too");

const ComplexBuiltin *complex_builtin_for(std::string_view name) {
    for (const ComplexBuiltin &builtin : kComplexBuiltins) {
        if (builtin.name == name) {
            return &builtin;
        }
    }
    return nullptr;
}

/// Index into kBuiltins for a name applicable to `count` arguments.
std::optional<std::size_t> builtin_for(std::string_view name, std::size_t count) {
    for (std::size_t i = 0; i < std::size(kBuiltins); ++i) {
        const Builtin &builtin = kBuiltins[i];
        if (builtin.name != name) {
            continue;
        }
        const bool fits = builtin.arity < 0
                              ? count >= 1
                              : count == static_cast<std::size_t>(builtin.arity);
        if (fits) {
            return i;
        }
    }
    return std::nullopt;
}

/// Constants Maxima names, recognised so that a result containing %pi can be
/// evaluated without the caller having to bind it.
bool named_constant(std::string_view name, double &value) {
    if (name == "%pi") {
        value = std::numbers::pi;
    } else if (name == "%e") {
        value = std::numbers::e;
    } else if (name == "%phi") {
        value = std::numbers::phi;
    } else if (name == "%gamma") {
        value = std::numbers::egamma;
    } else if (name == "inf") {
        value = std::numeric_limits<double>::infinity();
    } else if (name == "minf") {
        value = -std::numeric_limits<double>::infinity();
    } else {
        return false;
    }
    return true;
}

/// b^e for a small integer e, by squaring. Called where std::pow would be,
/// which for e of 2 or -1 is an order of magnitude more work than it needs —
/// and, for a complex base, where it would be wrong in the last bits: %i^2 is
/// exactly -1 this way, and -1 plus a rounding error times %i by std::pow.
template <typename Number>
Number integer_power(Number base, std::int32_t exponent) {
    const bool invert = exponent < 0;
    auto remaining = static_cast<std::uint32_t>(
        invert ? -static_cast<std::int64_t>(exponent) : exponent);
    Number result = 1.0;
    while (remaining != 0) {
        if ((remaining & 1u) != 0) {
            result *= base;
        }
        base *= base;
        remaining >>= 1;
    }
    return invert ? 1.0 / result : result;
}

double power(double base, double exponent) {
    return std::pow(base, exponent);
}

/// The principal value of base^exponent, computed in real arithmetic where the
/// answer is real, so that a real expression evaluated as a complex one gives
/// exactly the number the real evaluator does.
Complex power(Complex base, Complex exponent) {
    if (exponent.imag() == 0.0) {
        const double n = exponent.real();
        const bool integral = n == std::floor(n);
        if (base.imag() == 0.0 && (base.real() >= 0.0 || integral)) {
            return std::pow(base.real(), n);
        }
        if (integral && std::fabs(n) <= 64) {
            return integer_power(base, static_cast<std::int32_t>(n));
        }
        // A square root, which is how sqrt(x) is written. By std::pow,
        // sqrt(-4) is 2*%i plus a rounding error in the real part.
        if (n == 0.5) {
            return square_root(base);
        }
    }
    // By logarithm() rather than std::pow, for a negative base on the side
    // of the cut Maxima takes, whatever the sign of its zero imaginary part.
    return std::exp(exponent * logarithm(base));
}

double numeric_value_of(const Expr &expr) {
    switch (expr.kind()) {
    case Kind::Integer:
        return expr.integer_value().to_double();
    case Kind::Rational:
        return expr.numerator().to_double() / expr.denominator().to_double();
    default:
        return expr.real_value();
    }
}

// --- the one-shot evaluator -----------------------------------------------
//
// Kept as a direct walk rather than being routed through Compiled: compiling
// costs a traversal and several allocations, which for a single evaluation
// would be more work than the evaluation itself.

/// Arguments a function call is evaluated into without touching the heap.
/// Every builtin takes one or two, except max and min; a longer call spills.
constexpr std::size_t kArgumentsOnStack = 8;

/// Reports a failure, if anyone is listening, and returns false. The message is
/// built by `message` only then: is_evaluable asks whether, not why, and building
/// "no value for the symbol x" — or printing a whole relation to say it has no
/// value — used to be most of what it cost.
template <typename Message>
bool fail(std::string *failure, Message &&message) {
    if (failure != nullptr) {
        *failure = message();
    }
    return false;
}

/// Applies a function to arguments already evaluated. Over the reals, the
/// builtin of that name; over the complex numbers, its complex meaning, or,
/// for a function with none, its real one when every argument is real.
bool apply(const Expr &call, const double *args, std::size_t count, double &result,
           std::string *failure) {
    if (const auto builtin = builtin_for(call.name(), count)) {
        result = kBuiltins[*builtin].apply(args, count);
        return true;
    }
    return fail(failure, [&] {
        return "cannot evaluate " + call.name() + " numerically with "
               + std::to_string(count) + " argument(s)";
    });
}

bool apply(const Expr &call, const Complex *args, std::size_t count, Complex &result,
           std::string *failure) {
    if (count == 1) {
        if (const ComplexBuiltin *builtin = complex_builtin_for(call.name())) {
            result = builtin->apply(args[0]);
            return true;
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (args[i].imag() != 0.0) {
            return fail(failure, [&] {
                return "cannot evaluate " + call.name()
                       + " of a complex argument: it has no complex meaning here";
            });
        }
    }
    std::array<double, kArgumentsOnStack> on_stack{};
    std::vector<double> spilled;
    double *real = on_stack.data();
    if (count > kArgumentsOnStack) {
        spilled.resize(count);
        real = spilled.data();
    }
    for (std::size_t i = 0; i < count; ++i) {
        real[i] = args[i].real();
    }
    double value = 0.0;
    if (!apply(call, real, count, value, failure)) {
        return false;
    }
    result = value;
    return true;
}

/// Evaluates `expr` over the reals (Number = double) or the complex numbers
/// (Number = std::complex<double>). One walk for both, so the two cannot
/// disagree about anything but what %i and a function of it mean.
template <typename Number>
bool walk(const Expr &expr, const Bindings &bindings, Number &result,
          std::string *failure) {
    constexpr bool kIsComplex = std::same_as<Number, Complex>;

    switch (expr.kind()) {
    case Kind::Integer:
    case Kind::Rational:
    case Kind::Real:
        result = numeric_value_of(expr);
        return true;

    case Kind::Symbol: {
        // An explicit binding wins over a named constant, so a symbol that
        // happens to be called %e can still be given a value.
        if (const auto bound = bindings.find(expr.name()); bound != bindings.end()) {
            result = bound->second;
            return true;
        }
        if (double constant = 0.0; named_constant(expr.name(), constant)) {
            result = constant;
            return true;
        }
        if (expr.name() == "%i") {
            if constexpr (kIsComplex) {
                result = Complex(0.0, 1.0);
                return true;
            } else {
                return fail(failure, [] {
                    return std::string("no value for the symbol %i, which is not a "
                                       "real number: see proxima::eval_complex");
                });
            }
        }
        return fail(failure,
                    [&] { return "no value for the symbol " + expr.name(); });
    }

    case Kind::Add:
    case Kind::Mul: {
        const bool is_sum = expr.is(Kind::Add);
        result = is_sum ? 0.0 : 1.0;
        for (const Expr &operand : expr.args()) {
            Number value = 0.0;
            if (!walk(operand, bindings, value, failure)) {
                return false;
            }
            result = is_sum ? result + value : result * value;
        }
        return true;
    }

    case Kind::Pow: {
        Number base = 0.0;
        Number exponent = 0.0;
        if (!walk(expr.arg(0), bindings, base, failure)
            || !walk(expr.arg(1), bindings, exponent, failure)) {
            return false;
        }
        result = power(base, exponent);
        return true;
    }

    case Kind::Function: {
        // The arguments go on the stack. A std::vector here was an allocation
        // for every function call in the expression, on every evaluation.
        const std::vector<Expr> &operands = expr.args();
        const std::size_t count = operands.size();
        std::array<Number, kArgumentsOnStack> on_stack{};
        std::vector<Number> spilled;
        Number *args = on_stack.data();
        if (count > kArgumentsOnStack) {
            spilled.resize(count);
            args = spilled.data();
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (!walk(operands[i], bindings, args[i], failure)) {
                return false;
            }
        }
        return apply(expr, args, count, result, failure);
    }

    case Kind::Relation:
        return fail(failure, [&] {
            return "a relation has no numeric value: " + expr.str();
        });

    case Kind::Opaque:
        // Maxima source this library never interpreted, which is exactly why
        // there is nothing here that could evaluate it.
        return fail(failure, [&] {
            return "cannot evaluate the unmodelled expression " + expr.str();
        });
    }
    return fail(failure, [&] { return "cannot evaluate " + expr.str(); });
}

} // namespace

result<double> eval_numeric(const Expr &expr, const Bindings &bindings) {
    double value = 0.0;
    std::string failure;
    if (!walk(expr, bindings, value, &failure)) {
        return fxt::unexpected(fail(Cause::Eval, std::move(failure)));
    }
    return value;
}

bool is_evaluable(const Expr &expr, const Bindings &bindings) {
    double result = 0.0;
    // No sink: only whether, not why, so no failure message is ever built.
    return walk(expr, bindings, result, nullptr);
}

result<std::complex<double>> eval_complex(const Expr &expr,
                                          const Bindings &bindings) {
    Complex value = 0.0;
    std::string failure;
    if (!walk(expr, bindings, value, &failure)) {
        return fxt::unexpected(fail(Cause::Eval, std::move(failure)));
    }
    return value;
}

std::span<const std::string_view> numeric_functions() {
    return kBuiltinNames;
}

// --- the compiled form ----------------------------------------------------

namespace {

/// Emits `expr` in postfix order, tracking how deep the working stack gets.
class Compiler {
public:
    Compiler(std::vector<Instruction> &code, std::vector<double> &constants,
             const std::vector<std::string> &variables, const Bindings &bound)
        : code_(code), constants_(constants), variables_(variables), bound_(bound) {}

    std::size_t max_depth() const { return max_depth_; }

    void emit(const Expr &expr) {
        switch (expr.kind()) {
        case Kind::Integer:
        case Kind::Rational:
        case Kind::Real:
            push_constant(numeric_value_of(expr));
            return;

        case Kind::Symbol:
            emit_symbol(expr);
            return;

        case Kind::Add:
        case Kind::Mul: {
            for (const Expr &operand : expr.args()) {
                emit(operand);
            }
            const auto count = static_cast<std::uint32_t>(expr.arity());
            append({expr.is(Kind::Add) ? Instruction::Op::Add
                                       : Instruction::Op::Multiply,
                    0, count},
                   count);
            return;
        }

        case Kind::Pow: {
            emit(expr.arg(0));
            // An integer exponent known now saves a std::pow on every single
            // evaluation. The bound keeps the squaring loop short; beyond it
            // std::pow is the better bet anyway.
            const Expr &exponent = expr.arg(1);
            const std::optional<std::int64_t> small
                = exponent.is(Kind::Integer) ? exponent.integer_value().to_int64()
                                             : std::nullopt;
            if (small && *small >= -64 && *small <= 64) {
                append(
                    {Instruction::Op::IntegerPower,
                     static_cast<std::uint32_t>(static_cast<std::int32_t>(*small)),
                     1},
                    1);
                return;
            }
            emit(exponent);
            append({Instruction::Op::Power, 0, 2}, 2);
            return;
        }

        case Kind::Function: {
            for (const Expr &operand : expr.args()) {
                emit(operand);
            }
            const auto count = static_cast<std::uint32_t>(expr.arity());
            const auto builtin = builtin_for(expr.name(), expr.arity());
            if (!builtin) {
                throw EvalError("cannot evaluate " + expr.name()
                                + " numerically with " + std::to_string(count)
                                + " argument(s)");
            }
            append(
                {Instruction::Op::Call, static_cast<std::uint32_t>(*builtin), count},
                count);
            return;
        }

        case Kind::Relation:
            throw EvalError("a relation has no numeric value: " + expr.str());

        case Kind::Opaque:
            throw EvalError("cannot evaluate the unmodelled expression "
                            + expr.str());
        }
        throw EvalError("cannot evaluate " + expr.str());
    }

private:
    /// Pops `consumed` values and pushes one, so the net effect is known.
    void append(Instruction instruction, std::uint32_t consumed) {
        code_.push_back(instruction);
        depth_ -= consumed;
        ++depth_;
        max_depth_ = std::max(max_depth_, depth_);
    }

    /// Whether two constants can share a slot. Not ==, which holds for 0.0 and
    /// -0.0 although atan2 and division tell them apart: the compiled form of
    /// atan2(0.0, -1) + atan2(-0.0, -1) used to answer 2 pi instead of 0. And
    /// NaN is not even equal to itself, so a NaN bound twice took two slots.
    static bool same_constant(double a, double b) {
        if (std::isnan(a) || std::isnan(b)) {
            return std::isnan(a) && std::isnan(b);
        }
        return a == b && std::signbit(a) == std::signbit(b);
    }

    void push_constant(double value) {
        // Folded here, so the same literal appearing twice costs one slot.
        const auto existing = std::find_if(
            constants_.begin(), constants_.end(),
            [value](double constant) { return same_constant(constant, value); });
        const auto index
            = existing != constants_.end()
                  ? static_cast<std::uint32_t>(existing - constants_.begin())
                  : static_cast<std::uint32_t>(constants_.size());
        if (existing == constants_.end()) {
            constants_.push_back(value);
        }
        append({Instruction::Op::Constant, index, 0}, 0);
    }

    void emit_symbol(const Expr &expr) {
        // A variable shadows a binding, which shadows a named constant.
        const auto variable
            = std::find(variables_.begin(), variables_.end(), expr.name());
        if (variable != variables_.end()) {
            append({Instruction::Op::Variable,
                    static_cast<std::uint32_t>(variable - variables_.begin()), 0},
                   0);
            return;
        }
        if (const auto found = bound_.find(expr.name()); found != bound_.end()) {
            push_constant(found->second);
            return;
        }
        double value = 0.0;
        if (named_constant(expr.name(), value)) {
            push_constant(value);
            return;
        }
        if (expr.name() == "%i") {
            throw EvalError("no value for the symbol %i, which is not a real "
                            "number: Compiled evaluates over the reals only");
        }
        throw EvalError("no value for the symbol " + expr.name());
    }

    std::vector<Instruction> &code_;
    std::vector<double> &constants_;
    const std::vector<std::string> &variables_;
    const Bindings &bound_;
    std::size_t depth_ = 0;
    std::size_t max_depth_ = 0;
};

} // namespace

Compiled::Compiled(const Expr &expr, std::span<const Symbol> variables,
                   const Bindings &constants) {
    variables_.reserve(variables.size());
    for (const Symbol &variable : variables) {
        variables_.push_back(variable.name());
    }

    Compiler compiler(code_, constants_, variables_, constants);
    compiler.emit(expr);
    depth_ = compiler.max_depth();
}

Compiled::Compiled(const Expr &expr, const Symbol &variable,
                   const Bindings &constants)
    : Compiled(expr, std::span<const Symbol>(&variable, 1), constants) {}

double Compiled::operator()(std::span<const double> values) const {
    if (values.size() != variables_.size()) {
        throw EvalError("expected " + std::to_string(variables_.size())
                        + " value(s) but was given "
                        + std::to_string(values.size()));
    }

    // Thread-local, so one Compiled can be shared across threads without
    // synchronisation and without allocating on every call.
    thread_local std::vector<double> scratch;
    if (scratch.size() < depth_) {
        scratch.resize(depth_);
    }
    double *stack = scratch.data();
    std::size_t top = 0;

    for (const Instruction &instruction : code_) {
        switch (instruction.op) {
        case Instruction::Op::Constant:
            stack[top++] = constants_[instruction.index];
            break;

        case Instruction::Op::Variable:
            stack[top++] = values[instruction.index];
            break;

        case Instruction::Op::Add: {
            top -= instruction.count;
            double sum = 0.0;
            for (std::uint32_t i = 0; i < instruction.count; ++i) {
                sum += stack[top + i];
            }
            stack[top++] = sum;
            break;
        }

        case Instruction::Op::Multiply: {
            top -= instruction.count;
            double product = 1.0;
            for (std::uint32_t i = 0; i < instruction.count; ++i) {
                product *= stack[top + i];
            }
            stack[top++] = product;
            break;
        }

        case Instruction::Op::Power:
            top -= 2;
            stack[top] = std::pow(stack[top], stack[top + 1]);
            ++top;
            break;

        case Instruction::Op::IntegerPower:
            stack[top - 1] = integer_power(
                stack[top - 1], static_cast<std::int32_t>(instruction.index));
            break;

        case Instruction::Op::Call:
            top -= instruction.count;
            stack[top]
                = kBuiltins[instruction.index].apply(stack + top, instruction.count);
            ++top;
            break;
        }
    }
    return stack[0];
}

double Compiled::operator()(double value) const {
    return (*this)(std::span<const double>(&value, 1));
}

result<Compiled> compile(const Expr &expr, std::span<const Symbol> variables,
                         const Bindings &constants) {
    // The constructor is the one place the preparation is written, and it
    // reports through EvalError; this is that report as a value.
    try {
        return Compiled(expr, variables, constants);
    } catch (const EvalError &error) {
        return fxt::unexpected(fail(Cause::Eval, error.what()));
    }
}

result<Compiled> compile(const Expr &expr, const Symbol &variable,
                         const Bindings &constants) {
    const Symbol variables[] = {variable};
    return compile(expr, std::span<const Symbol>(variables), constants);
}

Compiled as_function(const Expr &expr, const Symbol &variable,
                     const Bindings &fixed) {
    return Compiled(expr, variable, fixed);
}

} // namespace proxima
