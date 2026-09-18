// A tour of Proxima: every public feature, in the order you are likely to
// need them, with the reasoning in the comments. Read it top to bottom as a
// quick-start guide, or run it and read the output alongside.
//
//     cmake --build --preset windows        (or linux, or wsl)
//     ./build/<preset>/tour
//
// It comes in two parts.
//
//   Part one needs no Maxima at all: building expressions, exact integers,
//   the offline parser, rendering (including a renderer of your own), and
//   numeric evaluation. All of that is plain C++.
//
//   Part two starts Maxima behind the scenes: calculus, solving, assumptions,
//   the kernel itself, caching, timeouts. If Maxima is not installed, part
//   one still runs and part two says what is missing.
//
// The companion examples/demo.cpp is the short version. This is the long one.

#include <proxima/assumptions.hpp> // proxima::Assumptions — facts, as a value
#include <proxima/errors.hpp>      // proxima::Error and its family
#include <proxima/expr.hpp>        // proxima::Expr — the expression value type
#include <proxima/functions.hpp>   // sin, cos, exp, sqrt, pi, inf, ...
#include <proxima/integer.hpp>     // proxima::Integer — exact, unbounded integers
#include <proxima/kernel.hpp>      // proxima::Kernel — a Maxima session
#include <proxima/mathml.hpp>      // proxima::to_mathml
#include <proxima/numeric.hpp>     // eval_numeric, Compiled, as_function
#include <proxima/ops.hpp>         // diff, integrate, solve, limit, ...
#include <proxima/render.hpp>      // proxima::render — for renderers of your own
#include <proxima/symbol.hpp>      // proxima::Symbol — a named unknown
#include <proxima/tex.hpp>         // proxima::to_tex
#include <proxima/version.hpp>     // proxima::version

// Proxima's results are FXT's, so FXT's adaptors apply to them directly.
#include <fxt/monads/AndThen.hpp>
#include <fxt/monads/Transform.hpp>
#include <fxt/monads/ValueOr.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace {

// --- a little presentation ------------------------------------------------

void section(std::string_view title) {
    std::cout << "\n" << title << "\n" << std::string(title.size(), '-') << "\n";
}

/// Prints `label` and a value, aligned, so the output reads as a table.
void show(std::string_view label, std::string_view value) {
    std::cout << "  " << std::left << std::setw(36) << label << value << "\n";
}

void show(std::string_view label, const proxima::Expr &value) {
    show(label, value.str());
}

/// Several expressions on one line, separated by commas.
std::string joined(const std::vector<proxima::Expr> &items) {
    std::string out;
    for (const proxima::Expr &item : items) {
        if (!out.empty()) {
            out += ", ";
        }
        out += item.str();
    }
    return out.empty() ? "(none)" : out;
}

// ===========================================================================
// PART ONE — no Maxima needed
// ===========================================================================

// --- 1. Symbols and expressions ---------------------------------------------

void symbols_and_expressions() {
    section("1. Symbols and expressions");

    // A Symbol is a named unknown. Operations that differentiate, integrate or
    // solve take a Symbol rather than any expression, so `diff(f, 2*x)` is a
    // compile error rather than a runtime surprise.
    const proxima::Symbol x("x");

    // The _sym literal says the same thing with less ceremony.
    using namespace proxima::literals;
    const proxima::Symbol y = "y"_sym;

    // Ordinary operators build expressions. A Symbol converts to an Expr
    // wherever one is expected, and so do integers and doubles, so this reads
    // the way you would write it on paper. `pow` is found by argument-
    // dependent lookup; there is no ^ operator, because C++'s ^ is xor and
    // binds more loosely than +.
    const proxima::Expr f = pow(x, 2) + 3 * x + 2;
    show("f = pow(x, 2) + 3*x + 2", f);

    // An Expr is an immutable value. Copying one copies a pointer; nothing is
    // ever modified in place, so it is safe to share between threads and to
    // keep as a map key.
    const proxima::Expr g = f; // Cheap.
    show("a copy compares equal", g == f ? "true" : "false");

    // Numbers stay exact unless you ask otherwise. 1/3 is a Rational, not
    // 0.333...; 2 and 2.0 are different values, because one is exact.
    show("Expr(1) / 3", proxima::Expr(1) / 3);
    show("  kind", kind_name((proxima::Expr(1) / 3).kind()));
    show("Expr(1.0) / 3", proxima::Expr(1.0) / 3);
    show("  kind", kind_name((proxima::Expr(1.0) / 3).kind()));
    show("Expr::rational(2, 4)", proxima::Expr::rational(2, 4)); // Reduced on sight.

    // Expressions are *normalised* when built: nested sums are flattened,
    // numbers are folded, and operands are put in a canonical order. That is
    // what makes x + 1 and 1 + x the same expression — and why f above
    // printed with its constant first.
    show("(x + 1) == (1 + x)", (x + 1) == (1 + x) ? "true" : "false");

    // But normalising is not algebra. x - x is still x - x here; collecting
    // like terms, expanding and factoring are Maxima's job (part two). One
    // canonicaliser, not two that could disagree.
    show("x - x", x - x);

    // == is structural equality and returns bool, so Expr works in standard
    // containers and test assertions. Equations are built with eq(), and the
    // other relations with ne, lt, le, gt and ge.
    show("eq(pow(x, 2), 2)", eq(pow(x, 2), 2));
    show("lt(x, y)", lt(x, y));

    // Hashing is consistent with ==, so expressions can go in unordered sets
    // and maps. These two insertions are one element.
    const std::unordered_set<proxima::Expr> seen{x + 1, 1 + x};
    show("unordered_set{x + 1, 1 + x}.size()", std::to_string(seen.size()));

    // Reading a tree: kind() says what a node is, args() gives its operands.
    // Every accessor checks the kind and throws proxima::Error if you ask the
    // wrong question, rather than returning something meaningless.
    show("f.kind()", kind_name(f.kind()));
    for (const proxima::Expr &term : f.args()) {
        show("  term",
             term.str() + "   (" + std::string(kind_name(term.kind())) + ")");
    }

    // A function the library has no builder for is still an expression: an
    // *uninterpreted* application, with a name and arguments. Maxima gives it
    // meaning when it gets there.
    const proxima::Expr bessel = proxima::Expr::function("bessel_j", {0, x});
    show("Expr::function(\"bessel_j\", {0, x})", bessel);
    show("  name(), arity()", bessel.name() + ", " + std::to_string(bessel.arity()));

    // Some conversions are refused at compile time on purpose. C++ would
    // happily turn true into 1 and 'a' into 97; an expression should not.
    //
    //     proxima::Expr oops(true);   // error: use of deleted function
    //     x + 'a';               // error: use of deleted function
    //
    // A Maxima boolean is a symbol: Expr::symbol("true").
}

// --- 2. Exact integers ------------------------------------------------------

void exact_integers() {
    section("2. Exact integers");

    // proxima::Integer has no fixed width. Maxima produces large integers in
    // ordinary use — 30! has 33 digits — so a 64-bit type would either wrap
    // or refuse. Values that do fit in 64 bits are stored inline and never
    // allocate, which is almost all of them.
    const proxima::Integer factorial30("265252859812191058636308480000000");
    show("30!", factorial30.to_string());
    show("30! * 30!", (factorial30 * factorial30).to_string());
    show("30! / 7", (factorial30 / 7).to_string()); // Truncating, as C++ does.
    show("gcd(30!, 1001)", gcd(factorial30, 1001).to_string());

    // to_int64 tells you whether a value fits a built-in type.
    show("30!.to_int64() has a value", factorial30.to_int64() ? "yes" : "no");
    show("(30! / 30!).to_int64()",
         std::to_string(*(factorial30 / factorial30).to_int64()));

    // An Integer becomes an Expr like any other number, and stays exact.
    show("Expr(30!) * 2", proxima::Expr(factorial30) * 2);

    // Integer::parse reports failure instead of throwing, for text you do
    // not control; the constructor throws proxima::ParseError.
    show("Integer::parse(\"12a\") has a value",
         proxima::Integer::parse("12a") ? "yes" : "no");
}

// --- 3. Parsing text, without Maxima ----------------------------------------

void parsing_offline() {
    section("3. Parsing text, without Maxima");

    // Expr::parse reads infix text with no kernel involved. It accepts a
    // deliberate subset of Maxima's syntax — arithmetic, comparisons, function
    // calls, lists, strings — and produces the same expression the operators
    // in section 1 would build.
    const proxima::Symbol x("x");
    const proxima::Expr parsed = *proxima::Expr::parse("x^2 + 3*x + 2");
    show("Expr::parse(\"x^2 + 3*x + 2\")", parsed);
    show("  same as the operators build?",
         parsed == pow(x, 2) + 3 * x + 2 ? "true" : "false");

    // Precedences are Maxima's, including the two that surprise people: ^ is
    // right-associative, and unary minus binds more loosely than ^. Printed,
    // both look just like their input, so compare them with expressions built
    // explicitly to see how they were grouped.
    const proxima::Expr tower = *proxima::Expr::parse("2^3^2");
    show("Expr::parse(\"2^3^2\")", tower);
    show("  is it 2^(3^2)?", tower == pow(proxima::Expr(2), pow(proxima::Expr(3), 2))
                                 ? "true"
                                 : "false");
    const proxima::Expr negated = *proxima::Expr::parse("-x^2");
    show("Expr::parse(\"-x^2\")", negated);
    show("  is it -(x^2)?", negated == -pow(x, 2) ? "true" : "false");

    // It parses; it does not evaluate. 5! stays factorial(5). proxima::parse, in
    // part two, hands text to Maxima's own parser instead, which evaluates
    // as it reads and accepts everything Maxima does.
    show("Expr::parse(\"5!\")", *proxima::Expr::parse("5!"));

    // Malformed text is a Failure, not an exception — Expr::parse returns a
    // proxima::result, like everything here that can fail for an ordinary
    // reason — and its message names where the text went wrong. The `*`
    // above is the result being read on the spot; check it first when the
    // text is not yours. proxima::unwrap turns a failure into the exception
    // its cause names, for code that would rather catch.
    const auto malformed = proxima::Expr::parse("x + * 2");
    show("Expr::parse(\"x + * 2\") has a value", malformed ? "yes" : "no");
    show("  its failure says", malformed.error().message());
    show("  and its cause is Parse",
         proxima::cause_of(malformed.error()) == proxima::Cause::Parse ? "yes"
                                                                       : "no");
}

// --- 4. Functions and constants ---------------------------------------------

void functions_and_constants() {
    section("4. Functions and constants");

    // <proxima/functions.hpp> has builders for the common functions. Nothing is
    // evaluated here: sin(0) stays sin(0) until Maxima is asked.
    //
    // They take an Expr or a Symbol, never a plain number — so proxima::sin never
    // competes with std::sin for sin(0.5) — which is why the 0 is spelled out.
    const proxima::Symbol x("x");
    show("sin(x) * cos(x)", proxima::sin(x) * proxima::cos(x));
    show("log(abs(x))", proxima::log(proxima::abs(x)));
    show("sin(0)", proxima::sin(proxima::Expr(0)));

    // Two are not functions at all, because Maxima has no node for them:
    // sqrt is a power of 1/2 and exp is a power of %e. Building them that way
    // keeps both sides' representations in step.
    show("sqrt(x)", proxima::sqrt(x));
    show("exp(x)", proxima::exp(x));

    // Constants are spelled as Maxima names them.
    show("pi(), e(), i(), inf()", proxima::pi().str() + ", " + proxima::e().str()
                                      + ", " + proxima::i().str() + ", "
                                      + proxima::inf().str());
}

// --- 5. Rendering, including a renderer of your own -------------------------

/// A renderer you might write yourself: Lisp-style prefix notation.
///
/// A renderer is a plain struct. It inherits nothing and overrides nothing;
/// proxima::render only needs the operations below to exist. Each receives its
/// children *already rendered*, and says how to combine them.
///
/// The library has already made the hard decisions before these are called:
/// the sign has been pulled out of `x - 1` so it does not read `-1 + x`, `x/3`
/// arrives as a fraction rather than a product with 1/3 in it, and the
/// question of where brackets belong has been answered. A renderer only
/// chooses spellings.
struct PrefixRenderer {
    /// A renderer may keep state. See std::ref below for reading it back.
    int symbols_seen = 0;

    // Leaves.
    std::string integer(const proxima::Integer &value) { return value.to_string(); }
    std::string real(double value) { return proxima::Expr(value).str(); }
    std::string symbol(std::string_view name) {
        ++symbols_seen;
        return std::string(name);
    }
    /// Maxima source text the library never interpreted. Every renderer has
    /// to decide what to do with it, so there is no default.
    std::string verbatim(std::string_view source) { return std::string(source); }

    // Arithmetic. A sum's terms arrive with their signs already separated.
    std::string sum(std::span<const proxima::Term<std::string>> terms) {
        std::string out = "(+";
        for (const proxima::Term<std::string> &term : terms) {
            out += term.negated ? " (- " + term.value + ")" : " " + term.value;
        }
        return out + ")";
    }
    std::string product(std::span<const std::string> factors) {
        return prefix("*", factors);
    }
    std::string fraction(const std::string &numerator,
                         const std::string &denominator) {
        return "(/ " + numerator + " " + denominator + ")";
    }
    std::string power(const std::string &base, const std::string &exponent) {
        return "(^ " + base + " " + exponent + ")";
    }

    // Applications.
    std::string call(std::string_view head, std::span<const std::string> args) {
        return prefix(head, args);
    }
    std::string relation(proxima::RelOp op, const std::string &lhs,
                         const std::string &rhs) {
        return "(" + std::string(proxima::symbol_for(op)) + " " + lhs + " " + rhs
               + ")";
    }

    /// How to parenthesise. The library decides *when*; this decides *how*.
    /// Prefix notation is never ambiguous, so it needs no brackets at all.
    std::string group(const std::string &inner) { return inner; }

    // --- optional operations ---------------------------------------------
    //
    // negate() is optional; without it the library would build a negation
    // out of sum(). root() and list() are optional too, and are left out
    // here: a square root is then rendered through power(), as (^ x (/ 1 2)).

    std::string negate(const std::string &inner) { return "(- " + inner + ")"; }

    /// Also optional: how tightly each construct binds. Saying that
    /// everything is an atom tells the library never to ask for brackets,
    /// which is right for a notation that brackets everything already.
    proxima::Strength strength_of(proxima::Construct) {
        return proxima::Strength::Atom;
    }

private:
    static std::string prefix(std::string_view head,
                              std::span<const std::string> parts) {
        std::string out = "(" + std::string(head);
        for (const std::string &part : parts) {
            out += " " + part;
        }
        return out + ")";
    }
};

void rendering() {
    section("5. Rendering, including a renderer of your own");

    const proxima::Symbol x("x");
    const proxima::Expr quotient = (x + 1) / (x - 1);

    // Three renderers come with the library. All are local — no kernel.
    //   str()       Maxima-compatible infix, which Expr::parse reads back.
    //   to_tex()     LaTeX, for a maths environment.
    //   to_mathml()  Presentation MathML, which browsers display natively.
    show("str()", quotient);
    show("to_tex()", proxima::to_tex(quotient));
    show("to_mathml()", proxima::to_mathml(quotient));

    // The same machinery is open to you: pass any renderer to proxima::render.
    // The output type is deduced from the renderer — here std::string, but a
    // two-dimensional renderer returns boxes instead (examples/text2d.hpp).
    show("proxima::render(e, PrefixRenderer{})",
         proxima::render(quotient, PrefixRenderer{}));
    show("  and -(x + 1)", proxima::render(-(x + 1), PrefixRenderer{}));
    show("  and sqrt(x), with no root()",
         proxima::render(proxima::sqrt(x), PrefixRenderer{}));

    // proxima::render copies a named renderer, so state it collects is gone
    // once it returns. Wrap it in std::ref to keep your own object.
    PrefixRenderer counting;
    proxima::render(pow(x, 2) + x * proxima::pi(), std::ref(counting));
    show("symbols counted, via std::ref", std::to_string(counting.symbols_seen));

    // proxima::Renderer<T> holds any renderer producing T, erased to one type:
    // choose one at run time, keep it in a member, pass it around. It is
    // move-only, and stores small renderers without allocating.
    proxima::Renderer<std::string> chosen = PrefixRenderer{};
    show("through proxima::Renderer<std::string>",
         proxima::render(eq(x, 2), chosen));
}

// --- 6. Numbers from expressions ----------------------------------------------

void numeric_evaluation() {
    section("6. Numbers from expressions");

    // Once you have a closed form, turning it into numbers is arithmetic, not
    // algebra, so it happens locally. Asking Maxima for each point would cost
    // a round trip per point, which makes plotting absurd.
    const proxima::Symbol x("x");
    const proxima::Symbol a("a");
    const proxima::Expr f = a * proxima::sin(x) + pow(x, 2);

    // eval_numeric: one evaluation, with symbols bound by name. Maxima's named
    // constants (%pi, %e, inf) need no binding.
    show("eval_numeric(f, {x: 1, a: 2})",
         std::to_string(*proxima::eval_numeric(f, {{"x", 1.0}, {"a", 2.0}})));
    show("eval_numeric(pi() / 2)",
         std::to_string(*proxima::eval_numeric(proxima::pi() / 2)));

    // is_evaluable asks first, without throwing.
    show("is_evaluable(f) with nothing bound",
         proxima::is_evaluable(f) ? "yes" : "no");

    // Otherwise, anything that cannot become a number is a Failure with
    // Cause::Eval: an unbound symbol, a function with no numeric meaning
    // here, a relation.
    const auto unbound = proxima::eval_numeric(f, {{"x", 1.0}});
    show("with a left unbound", unbound ? std::to_string(*unbound)
                                        : "Failure: " + unbound.error().message());

    // For many evaluations, compile once. Compiled resolves every symbol and
    // function up front into a flat instruction list, so each call is just
    // arithmetic — and errors surface here, at construction, not per point.
    const proxima::Compiled compiled(f, x, {{"a", 2.0}}); // a is fixed at 2.
    std::string samples;
    for (const double at : {0.0, 0.5, 1.0, 1.5}) {
        samples += std::to_string(compiled(at)).substr(0, 6) + "  ";
    }
    show("Compiled f(x) at 0, 0.5, 1, 1.5", samples);

    // Several variables, in the order you list them.
    const std::vector<proxima::Symbol> variables{x, a};
    const proxima::Compiled two_variables(f, variables);
    const double point[] = {1.0, 2.0}; // x = 1, a = 2
    show("Compiled f(x, a) at (1, 2)", std::to_string(two_variables(point)));

    // as_function binds one variable and hands back the Compiled, which is
    // itself a callable — and converts to a std::function<double(double)>
    // for an API that wants one.
    const std::function<double(double)> as_callable
        = proxima::as_function(f, x, {{"a", 2.0}});
    show("as_function(f, x, {a: 2})(1)", std::to_string(as_callable(1.0)));

    // A Compiled can be shared between threads freely; its working space is
    // thread-local.
}

// ===========================================================================
// PART TWO — Maxima does the mathematics
// ===========================================================================

// --- 7. Calculus and algebra --------------------------------------------------

void calculus_and_algebra() {
    section("7. Calculus and algebra");

    // Every operation takes an optional Kernel as its last argument. Leave it
    // out and the process-wide shared_kernel() is used, started on first use —
    // which is why the first call below takes a moment.
    const proxima::Symbol x("x");
    const proxima::Symbol y("y");
    const proxima::Expr f = pow(x, 3) * proxima::sin(x);

    show("diff(x^3 sin(x), x)", *proxima::diff(f, x));
    show("diff(x^3 sin(x), x, 2)", *proxima::diff(f, x, 2)); // Second derivative.
    show("expand((x + y)^3)", *proxima::expand(pow(x + y, 3)));
    show("factor(x^2 - y^2)", *proxima::factor(pow(x, 2) - pow(y, 2)));
    show("ratsimp((x^2 - 1)/(x - 1))", *proxima::ratsimp((pow(x, 2) - 1) / (x - 1)));
    show("subst(x^2 + y, x, 3)", *proxima::subst(pow(x, 2) + y, x, 3));

    // Results are expressions like any other, so they compare structurally
    // with ones you build yourself.
    show("expand((x+1)^2) == x^2 + 2x + 1",
         *proxima::expand(pow(x + 1, 2)) == pow(x, 2) + 2 * x + 1 ? "true"
                                                                  : "false");

    // Operations that can legitimately fail return std::expected. Check it
    // before using the value.
    if (const auto antiderivative = proxima::integrate(f, x)) {
        show("integrate(x^3 sin(x), x)", *antiderivative);
    }
    if (const auto area = proxima::integrate(pow(x, 2), x, 0, 1)) {
        show("integrate(x^2, x, 0, 1)", *area); // Exactly 1/3.
    }

    // Limits, from either side where it matters.
    if (const auto l = proxima::limit(proxima::sin(x) / x, x, 0)) {
        show("limit(sin(x)/x, x, 0)", *l);
    }
    if (const auto l
        = proxima::limit(proxima::Expr(1) / x, x, 0, proxima::Side::FromAbove)) {
        show("limit(1/x, x, 0, FromAbove)", *l);
    }
    if (const auto l
        = proxima::limit(proxima::Expr(1) / x, x, 0, proxima::Side::FromBelow)) {
        show("limit(1/x, x, 0, FromBelow)", *l);
    }

    // solve returns every solution.
    if (const auto roots = proxima::solve(eq(pow(x, 2), 2), x)) {
        show("solve(x^2 = 2, x)", joined(*roots));
    }

    // A system: equations, then unknowns. Each Solution holds one value per
    // unknown, in the order you asked for them, however Maxima ordered its
    // answer.
    const std::vector<proxima::Expr> equations{eq(x + y, 3), eq(x - y, 1)};
    const std::vector<proxima::Symbol> unknowns{x, y};
    if (const auto solutions = proxima::solve(equations, unknowns)) {
        for (const proxima::Solution &solution : *solutions) {
            show("solve({x+y=3, x-y=1}, {x, y})",
                 "x = " + solution[0].str() + ", y = " + solution[1].str());
        }
    }

    // proxima::parse hands text to Maxima's own parser, so it accepts all of
    // Maxima's syntax — and evaluates as it reads, unlike Expr::parse.
    if (const auto parsed = proxima::parse("5!")) {
        show("proxima::parse(\"5!\")", *parsed);
    }
}

// --- 8. When there is no answer ---------------------------------------------

void when_there_is_no_answer() {
    section("8. When there is no answer");

    const proxima::Symbol x("x");

    // Failing to find a closed form is an ordinary outcome, not an error, so
    // it comes back as an proxima::Failure inside the std::expected, carrying a
    // reason — usually Maxima's own words.
    const auto hopeless = proxima::integrate(proxima::exp(proxima::sin(x)), x);
    show("integrate(e^sin(x), x)",
         hopeless ? hopeless->str() : "Failure: " + hopeless.error().message());

    // Maxima does not always signal failure as an error. For solve it hands
    // back something that is not a solution, such as x = sin(x); the library
    // recognises that and reports a Failure, so a success really is one.
    const auto circular = proxima::solve(eq(x, proxima::sin(x)), x);
    show("solve(x = sin(x), x)",
         circular ? "solved?" : "Failure: " + circular.error().message());

    // Some questions Maxima itself rejects, like a divergent integral. That
    // arrives as a Failure too, in Maxima's words.
    const auto divergent = proxima::integrate(proxima::Expr(1) / x, x, 0, 1);
    show("integrate(1/x, x, 0, 1)",
         divergent ? divergent->str() : "Failure: " + divergent.error().message());

    // An infinite answer is still an answer. From both sides at once, 1/x
    // grows without a sign, and Maxima says `infinity` — its complex
    // infinity, not the inf and minf of the one-sided limits in section 7.
    const auto both_sides = proxima::limit(proxima::Expr(1) / x, x, 0);
    show("limit(1/x, x, 0), both sides",
         both_sides ? both_sides->str()
                    : "Failure: " + both_sides.error().message());

    // A limit that does not exist is a Failure, however Maxima puts it.
    // abs(x)/x is -1 on one side of 0 and 1 on the other: Maxima answers `ind`,
    // bounded but with no single value, and the Failure says so. Maxima's
    // `und`, undefined, is a Failure the same way. From one side, the same
    // limit is an ordinary value.
    const auto bounded = proxima::limit(proxima::abs(x) / x, x, 0);
    show("limit(abs(x)/x, x, 0)",
         bounded ? bounded->str() : "Failure: " + bounded.error().message());
    const auto one_sided
        = proxima::limit(proxima::abs(x) / x, x, 0, proxima::Side::FromAbove);
    show("limit(abs(x)/x, x, 0, FromAbove)",
         one_sided ? one_sided->str() : "Failure: " + one_sided.error().message());

    // Every operation returns a proxima::result, diff included: it fails only
    // when something is wrong with the question, but that is still an
    // outcome, not an exception. An Opaque node holds Maxima source the
    // library never interpreted, and this one does not parse. The cause says
    // which kind of failure it was.
    const auto refused = proxima::diff(proxima::Expr::opaque("(1"), x);
    show("diff(<unparseable text>, x)",
         refused ? refused->str() : "Failure: " + refused.error().message());
    show("  cause", proxima::to_string(proxima::cause_of(refused.error())));

    // The results are FXT's, so they compose with FXT's adaptors directly:
    // and_then, transform, value_or and the rest, and a chain stops at the
    // first failure.
    const auto chained
        = proxima::diff(pow(x, 3), x)
          | fxt::and_then([](const proxima::Expr &d) { return proxima::factor(d); })
          | fxt::transform([](const proxima::Expr &e) { return e.str(); })
          | fxt::value_or(std::string("no answer"));
    show("diff(x^3) | and_then(factor) | str", chained);
}

// --- 9. Assumptions -------------------------------------------------------------

void assumptions() {
    section("9. Assumptions");

    const proxima::Symbol x("x");
    const proxima::Symbol n("n");
    const proxima::Symbol k("k");

    // Some answers depend on facts Maxima has not been told. Interactively it
    // would ask; over a pipe it cannot, so the question comes back as a
    // Failure naming exactly the fact that is missing.
    const auto unknown = proxima::integrate(pow(x, n), x);
    show("integrate(x^n, x)",
         unknown ? unknown->str() : "Failure: " + unknown.error().message());

    // Supply the fact with the question. Assumptions are a value, passed as
    // the last argument like the kernel is: nothing is set up beforehand, and
    // nothing is left behind for the next call.
    const auto positive_n = proxima::assuming(gt(n, 0));
    if (const auto known = proxima::integrate(pow(x, n), x, positive_n)) {
        show("  under n > 0", *known);
    }

    // The same expression can simplify differently under different facts.
    const proxima::Expr root = proxima::sqrt(pow(x, 2));
    show("  sqrt(x^2), nothing assumed of x", *proxima::expand(root));
    show("  sqrt(x^2), under x > 0",
         *proxima::expand(root, proxima::assuming(gt(x, 0))));
    show("  sqrt(x^2), under x < 0",
         *proxima::expand(root, proxima::assuming(lt(x, 0))));

    // Assumptions grow by making new values. declaring records a property of
    // a symbol, rather than a relation.
    const auto more = positive_n.with(gt(x, 0)).with(k, proxima::Feature::Integer);
    show("  sin(k*pi), k declared integer",
         *proxima::expand(proxima::sin(k * proxima::pi()), more));
    show("  sin(k*pi), nothing declared",
         *proxima::expand(proxima::sin(k * proxima::pi())));
    show("  more.facts()", joined({more.facts().begin(), more.facts().end()}));

    // A value compares by what it says, not how it was built, so the same
    // facts in another order are the same assumptions — the same Maxima
    // context, and the same cached answers.
    show("  order does not matter",
         proxima::assuming(gt(x, 0)).with(gt(n, 0))
                 == proxima::assuming(gt(n, 0)).with(gt(x, 0))
             ? "true"
             : "false");

    // And the question without them still fails, as it should: nothing was
    // left in force.
    const auto again = proxima::integrate(pow(x, n), x);
    show("integrate(x^n, x), under nothing",
         again ? again->str() : "Failure again, as it should be");

    // Facts that contradict one another are refused, with their own cause.
    const auto contradiction
        = proxima::expand(root, proxima::assuming({gt(x, 0), lt(x, 0)}));
    show("  under x > 0 and x < 0",
         contradiction ? contradiction->str()
                       : std::string(proxima::to_string(
                             proxima::cause_of(contradiction.error()))));
}

// --- 10. The kernel, configuration and caching --------------------------------

void the_kernel() {
    section("10. The kernel, configuration and caching");

    const proxima::Symbol x("x");
    const proxima::Symbol y("y");
    const proxima::Symbol z("z");

    // You can run a Kernel of your own instead of the shared one: several
    // kernels are several Maxima processes, which is how to get real
    // parallelism. A Kernel is safe to share between threads, but calls on
    // one kernel take turns.
    //
    // proxima::Config configures it. Every field has a sensible default:
    //   maxima_root      where Maxima is installed; empty means discover it
    //   timeout         how long one call may take (default two minutes)
    //   startup_timeout  how long starting or restarting may take
    //   cache_entries    in-memory reply cache size; 0 disables it
    //   cache_directory  keep replies between runs; empty means do not
    //   load_user_init    read the user's maxima-init.mac (default: no, so
    //                   results do not vary from machine to machine)
    //   user_dir         where Maxima keeps user state when that is off

    // --- caching between runs --------------------------------------------
    //
    // With cache_directory set, answers are written to disk and shared by
    // every kernel using the same directory, in this process or another.
    // Every entry is keyed on the Maxima version, this library's version and
    // the assumptions in force, so an answer is only reused under the
    // conditions that produced it.
    const std::filesystem::path cache_directory
        = std::filesystem::temp_directory_path() / "proxima_tour_cache";
    std::error_code ignored;
    std::filesystem::remove_all(cache_directory, ignored); // Start clean.

    proxima::Config config;
    config.cache_directory = cache_directory;

    const proxima::Expr question = pow(x, 5) * proxima::exp(x);
    {
        proxima::Kernel first(config);
        // Computed by Maxima and written to disk. Only that side effect is
        // wanted here, so the answer is discarded — explicitly, because
        // integrate's std::expected is [[nodiscard]]: ignoring a result that
        // may be a Failure is usually a mistake.
        static_cast<void>(proxima::integrate(question, x, first));
    }
    proxima::Kernel second(config); // A fresh Maxima process, same directory.
    const auto from_disk = proxima::integrate(question, x, second);
    show("integrate(x^5 e^x), second kernel",
         from_disk ? from_disk->str() : "Failure");
    show("  answers that came from disk",
         std::to_string(second.cache_stats().persistent_hits));

    // --- talking to Maxima directly --------------------------------------
    //
    // Beneath the operations, a Kernel has two verbs. ask answers a Query —
    // an expression or Maxima text that changes nothing — and is cached;
    // tell carries out a Statement, and is not. The type says which is
    // which, so a question cannot be mistaken for a statement.
    const auto pi = second.ask(proxima::Query::text("float(%pi)"));
    show("ask(float(%pi))", pi ? pi->str() : pi.error().message());

    // An Expr can be sent instead of text; it travels as structure, so
    // nothing about it can be misread — and under assumptions, as any
    // operation can.
    const auto via_expr
        = second.ask(proxima::Query::form(proxima::sin(proxima::pi() / 6)));
    show("ask(sin(pi()/6))",
         via_expr ? via_expr->str() : via_expr.error().message());

    // Text Maxima cannot parse is an ordinary failure with a reason, not an
    // exception, and costs one round trip.
    const auto broken = second.ask(proxima::Query::text("(1"));
    show("ask(\"(1\") has a value?",
         broken ? "yes" : "no: " + broken.error().message());

    // Asking the same question again is answered from memory.
    const auto before = second.cache_stats();
    static_cast<void>(second.ask(proxima::Query::text("float(%pi)")));
    show("the same question again: cache hits",
         std::to_string(before.hits) + " -> "
             + std::to_string(second.cache_stats().hits));

    // tell is for statements that change Maxima — an assignment, a
    // definition. Since there is no telling from the text what changed, it
    // empties the cache, and turns off the on-disk cache for this kernel
    // until restart(). Use it only when you mean it.
    const auto assigned = second.tell(proxima::Statement::text("tour_value: 42"));
    show("tell(tour_value: 42)", assigned ? "done" : assigned.error().message());
    show("  then ask(tour_value)",
         second.ask(proxima::Query::text("tour_value"))->str());

    // --- timeouts and recovery -------------------------------------------
    //
    // A call that runs past its timeout throws proxima::TimeoutError. Before it
    // does, the kernel is restarted, so only that call is lost: the next one
    // works, under whatever assumptions it brings. set_timeout changes the deadline
    // for later calls. The restart takes a moment.
    second.set_timeout(std::chrono::milliseconds(1));
    try {
        static_cast<void>(proxima::expand(pow(x + y + z, 200), second));
    } catch (const proxima::TimeoutError &error) {
        show("expand((x+y+z)^200), 1 ms timeout", error.what());
    }
    second.set_timeout(std::chrono::minutes(2));
    show("  and the next call", *proxima::expand(pow(x + 1, 2), second));

    std::filesystem::remove_all(cache_directory, ignored); // Tidy up.
}

} // namespace

// ===========================================================================

int main() {
    std::cout << "Proxima " << proxima::version << ": a tour\n";

    // Part one is plain C++. Any proxima::Error here would be a bug in the tour.
    try {
        symbols_and_expressions();
        exact_integers();
        parsing_offline();
        functions_and_constants();
        rendering();
        numeric_evaluation();
    } catch (const proxima::Error &error) {
        std::cerr << "\nunexpected error in part one: " << error.what() << "\n";
        return 1;
    }

    // Part two needs Maxima. The error types form a small hierarchy under
    // proxima::Error, and the split that matters is between two kinds of failure:
    //
    //   proxima::KernelError   the conversation with Maxima broke down: it could
    //                     not be found or started, it died, or (as
    //                     proxima::TimeoutError) it did not answer in time
    //   proxima::MaximaError   Maxima answered, with an error, to a question that
    //                     should not fail
    //   proxima::EvalError     local numeric evaluation was impossible
    //   proxima::ParseError    text could not be read
    //
    // A *mathematical* failure — no closed form, no solution — is none of
    // these. It is an proxima::Failure returned in a std::expected (section 8).
    try {
        calculus_and_algebra();
        when_there_is_no_answer();
        assumptions();
        the_kernel();
    } catch (const proxima::KernelError &error) {
        std::cerr << "\nPart two needs Maxima, which could not be used:\n  "
                  << error.what()
                  << "\nPart one above ran without it. See 'Requirements' in "
                     "README.md for installing Maxima.\n";
        return 1;
    } catch (const proxima::Error &error) {
        std::cerr << "\nunexpected error in part two: " << error.what() << "\n";
        return 1;
    }

    std::cout << "\nEnd of the tour.\n";
    return 0;
}
