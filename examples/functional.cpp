// Proxima in a functional style, with FXT.
//
// Every operation that can fail returns a proxima::result<T>. That is FXT's
// own fxt::expected<T, fxt::failure>, not a lookalike, so FXT's adaptors
// apply to Proxima's results as they are. Proxima adds no pipe operators of
// its own. This file composes the two:
//
//   and_then, transform          a chain that stops at the first failure
//   ensure                       a validation step inside the chain
//   attempt                      bring code that throws into the chain
//   tap_error, or_else           observe a failure, then recover from it
//   traverse                     many operations, all or nothing
//   curry + with, zip + mapply   combine independent results
//   match                        consume a result, success or failure
//
// Part one needs no Maxima: the offline parser and numeric evaluation return
// results too. Part two asks Maxima, through the shared kernel.
//
// traverse, zip, mapply, curry and match use std::forward_like and deducing
// this, and GCC 13, the oldest compiler Proxima supports, has neither. Built
// with GCC 13, those sections say so instead of running (curry is written
// out by hand instead). GCC 14, Clang 19 and MSVC run everything.
//
//     cmake --build --preset windows        (or linux, or wsl)
//     ./build/<preset>/functional

#include <proxima/assumptions.hpp>
#include <proxima/errors.hpp>
#include <proxima/expr.hpp>
#include <proxima/functions.hpp>
#include <proxima/numeric.hpp>
#include <proxima/ops.hpp>
#include <proxima/result.hpp>
#include <proxima/symbol.hpp>
#include <proxima/tex.hpp>
#include <proxima/traverse.hpp>

#include <fxt/monads/AndThen.hpp>
#include <fxt/monads/Attempt.hpp>
#include <fxt/monads/Ensure.hpp>
#include <fxt/monads/OrElse.hpp>
#include <fxt/monads/Tap.hpp>
#include <fxt/monads/Transform.hpp>
#include <fxt/monads/Value.hpp>
#include <fxt/monads/ValueOr.hpp>
#include <fxt/monads/With.hpp>
#include <fxt/utils/Lift.hpp>

#if defined(__cpp_lib_forward_like) && defined(__cpp_explicit_this_parameter)
#define DEMO_FULL_FXT 1
#include <fxt/monads/Match.hpp>
#include <fxt/monads/Sequence.hpp>
#include <fxt/monads/Zip.hpp>
#include <fxt/tuples/Apply.hpp>
#include <fxt/utils/Curry.hpp>
#endif

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using proxima::Cause;
using proxima::Expr;
using proxima::Failure;
using proxima::result;
using proxima::Symbol;

void section(std::string_view title) {
    std::cout << "\n" << title << "\n" << std::string(title.size(), '-') << "\n";
}

/// A result as one line of text. Written as a pipeline itself: a value is
/// printed, and a failure is recovered into a description of why there was
/// none, so the final value() cannot fail.
template <typename T, typename Print>
std::string describe(const result<T> &r, Print print) {
    return r | fxt::transform(print)
           | fxt::or_else([](const Failure &failure) -> result<std::string> {
                 return "no answer ("
                        + std::string(proxima::to_string(proxima::cause_of(failure)))
                        + "): " + failure.message();
             })
           | fxt::value();
}

std::string describe(const result<Expr> &r) {
    return describe(r, [](const Expr &e) { return e.str(); });
}

std::string describe(const result<double> &r) {
    return describe(r, [](double v) { return std::to_string(v); });
}

void show(std::string_view label, const std::string &text) {
    std::cout << "  " << label << "\n      " << text << "\n";
}

[[maybe_unused]] void needs_newer_compiler(std::string_view what) {
    std::cout << "  (skipped: this uses " << what
              << ", which needs std::forward_like and deducing this:"
              << " GCC 14, Clang 19 or MSVC)\n";
}

const Symbol x("x");
const Symbol n("n");

// --- Part one: no Maxima -----------------------------------------------------

void local_chain() {
    section("and_then, transform: a chain with no kernel");

    // Expr::parse, compile and a Compiled's call all run in this process.
    // Each step either hands its value on or ends the chain with its failure.
    const auto at_three = [](std::string_view source) {
        return Expr::parse(source)
               | fxt::and_then([](const Expr &f) { return proxima::compile(f, x); })
               | fxt::transform([](const proxima::Compiled &f) { return f(3.0); });
    };
    show("x^2 - 2*x + 1 at x = 3", describe(at_three("x^2 - 2*x + 1")));
    // The parser fails, so compile and the call never run.
    show("x^^2 at x = 3", describe(at_three("x^^2")));
}

void validation() {
    section("ensure: a rule inside the chain");

    // ensure passes a value that satisfies the predicate and turns one that
    // does not into the failure given. The failure is Proxima's, so it has a
    // Cause like any other.
    const auto positive_at = [](const Expr &f, double at) {
        return proxima::eval_numeric(f, {{x, at}})
               | fxt::ensure([](double v) { return v > 0.0; },
                             proxima::fail(Cause::Argument, "not positive there"));
    };
    const Expr f = proxima::log(x);
    show("log(x) at 2, if positive", describe(positive_at(f, 2.0)));
    show("log(x) at 0.5, if positive", describe(positive_at(f, 0.5)));
}

void exceptions_into_values() {
    section("attempt: code that throws, as a result");

    // std::stod throws. fxt::attempt catches it and returns
    // fxt::expected<double, fxt::failure>, which is proxima::result<double>:
    // one type, so the chain goes straight on into Proxima.
    const auto evaluate_at = [](const std::string &input) {
        return fxt::attempt([&] { return std::stod(input); })
               | fxt::and_then([](double at) {
                     return proxima::eval_numeric(pow(x, 2) + 1, {{x, at}});
                 });
    };
    show("x^2 + 1 at \"1.5\"", describe(evaluate_at("1.5")));
    show("x^2 + 1 at \"one\"", describe(evaluate_at("one")));
}

// --- Part two: Maxima --------------------------------------------------------

void kernel_chain() {
    section("and_then, transform: a chain through the kernel");

    // Each operation takes its subject first. One with a single argument
    // lifts as it is; one that also needs the variable takes a lambda.
    const Expr f = pow(x, 3) - 3 * pow(x, 2) + 3 * x - 1;
    const std::string tex
        = proxima::diff(f, x)                            // result<Expr>
          | fxt::and_then(FXT_LIFT(proxima::factor))     // result<Expr>
          | fxt::transform(proxima::to_tex)              // result<std::string>
          | fxt::value_or(std::string("no derivative")); // std::string
    show("d/dx (x^3 - 3x^2 + 3x - 1), factored, as TeX", tex);
}

void recovery() {
    section("tap_error, or_else: observe a failure, then recover");

    // Integrating x^n needs to know whether n is -1, and Maxima cannot ask.
    // The failure's Cause says so, and or_else can act on exactly that
    // cause, retrying with the fact supplied. Any other failure passes on.
    const auto antiderivative = [](const Expr &f) {
        return proxima::integrate(f, x) | fxt::tap_error([](const Failure &failure) {
                   std::cout << "  (first attempt: " << failure.message() << ")\n";
               })
               | fxt::or_else([&](const Failure &failure) -> result<Expr> {
                     if (proxima::cause_of(failure) != Cause::NeedsAssumption) {
                         return fxt::unexpected<Failure>(failure);
                     }
                     return proxima::integrate(f, x, proxima::assuming(gt(n, 0)));
                 });
    };
    show("integral of x^n, assuming n > 0 if asked",
         describe(antiderivative(pow(x, n))));
}

void all_or_nothing() {
    section("traverse: many operations, all or nothing");
#if defined(DEMO_FULL_FXT)
    // traverse applies an operation to every element and gathers the values,
    // or stops at the first failure: result<std::vector<Expr>>.
    const auto integrate_all = [](const std::vector<Expr> &integrands) {
        return fxt::traverse(integrands,
                             [](const Expr &f) { return proxima::integrate(f, x); });
    };
    const auto show_all = [](const result<std::vector<Expr>> &r) {
        return describe(r, [](const std::vector<Expr> &values) {
            std::string text;
            for (const Expr &value : values) {
                text += (text.empty() ? "" : ",  ") + value.str();
            }
            return text;
        });
    };

    const std::vector<Expr> fine = {proxima::sin(x), proxima::exp(x), 1 / x};
    show("integrals of sin(x), exp(x), 1/x", show_all(integrate_all(fine)));

    // x^n fails, as in the section before. One failure fails the whole list.
    const std::vector<Expr> one_bad = {proxima::sin(x), pow(x, n), 1 / x};
    show("integrals of sin(x), x^n, 1/x", show_all(integrate_all(one_bad)));
#else
    needs_newer_compiler("fxt::traverse");
#endif
}

void combining() {
    section("curry + with, zip + mapply: combine independent results");

    // The tangent to f at x = 1 needs f(1) and f'(1): two results, either
    // of which could fail. curry the function that combines them, then feed
    // it each result with `with`. The first failure wins.
    const Expr f = pow(x, 3) - 2 * x;
    const Expr a = 1;
#if defined(DEMO_FULL_FXT)
    const auto combine = fxt::curry([&](const Expr &value, const Expr &slope) {
        return value + slope * (x - a);
    });
#else
    // What fxt::curry builds, written out: `with` itself needs nothing newer.
    const auto combine = [&](const Expr &value) {
        return [&, value](const Expr &slope) { return value + slope * (x - a); };
    };
#endif
    const auto tangent
        = combine | fxt::with(result<Expr>{proxima::replace(f, x, a)})
          | fxt::with(proxima::diff(f, x) | fxt::transform([&](const Expr &d) {
                          return proxima::replace(d, x, a);
                      }))
          | fxt::and_then(FXT_LIFT(proxima::expand));
    show("tangent to x^3 - 2x at x = 1", describe(tangent));

#if defined(DEMO_FULL_FXT)
    // zip gathers several results into one result holding a tuple, and
    // mapply spreads the tuple over a function: L'Hopital's rule for
    // sin(x)/x at 0, as the limit of the ratio of the derivatives.
    const auto lhopital
        = fxt::zip(proxima::diff(proxima::sin(x), x), proxima::diff(Expr(x), x))
          | fxt::mapply(
              [](const Expr &top, const Expr &bottom) { return top / bottom; })
          | fxt::and_then(
              [](const Expr &ratio) { return proxima::limit(ratio, x, 0); });
    show("limit of sin(x)/x at 0, by L'Hopital", describe(lhopital));
#else
    needs_newer_compiler("fxt::zip and fxt::mapply");
#endif
}

void consuming() {
    section("match: one handler for each outcome");
#if defined(DEMO_FULL_FXT)
    const auto report = [](const result<std::vector<Expr>> &roots) {
        return roots
               | fxt::match(
                   [](const std::vector<Expr> &values) {
                       return std::to_string(values.size()) + " roots";
                   },
                   [](const Failure &failure) {
                       return "unsolved: " + failure.message();
                   });
    };
    show("x^2 - 5x + 6 = 0", report(proxima::solve(pow(x, 2) - 5 * x + 6, x)));
#else
    needs_newer_compiler("fxt::match");
    std::cout << "  (describe() above does the same with or_else)\n";
#endif
}

} // namespace

int main() {
    std::cout << "Proxima in a functional style, with FXT\n";

    std::cout << "\n=== Part one: no Maxima ===\n";
    local_chain();
    validation();
    exceptions_into_values();

    std::cout << "\n=== Part two: with Maxima ===\n";
    try {
        kernel_chain();
        recovery();
        all_or_nothing();
        combining();
        consuming();
    } catch (const proxima::KernelError &error) {
        // Failures of the mathematics arrive as values. A kernel that cannot
        // run at all is an exception, since no chain could go on without it.
        std::cerr << "\nPart two needs Maxima, which could not be used:\n  "
                  << error.what() << "\n";
        return 1;
    }
    return 0;
}
