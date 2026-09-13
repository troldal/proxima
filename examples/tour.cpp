// A tour of maxima_cpp: every public feature, in the order you are likely to
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

#include <mx/context.hpp>   // mx::Context — scopes of assumptions
#include <mx/errors.hpp>    // mx::Error and its family
#include <mx/expr.hpp>      // mx::Expr — the expression value type
#include <mx/functions.hpp> // sin, cos, exp, sqrt, pi, inf, ...
#include <mx/integer.hpp>   // mx::Integer — exact, unbounded integers
#include <mx/kernel.hpp>    // mx::Kernel — a Maxima session
#include <mx/mathml.hpp>    // mx::toMathML
#include <mx/numeric.hpp>   // evalNumeric, Compiled, asFunction
#include <mx/ops.hpp>       // diff, integrate, solve, limit, ...
#include <mx/render.hpp>    // mx::render — for renderers of your own
#include <mx/symbol.hpp>    // mx::Symbol — a named unknown
#include <mx/tex.hpp>       // mx::toTeX
#include <mx/version.hpp>   // mx::version

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

void show(std::string_view label, const mx::Expr &value) {
    show(label, value.str());
}

/// Several expressions on one line, separated by commas.
std::string joined(const std::vector<mx::Expr> &items) {
    std::string out;
    for (const mx::Expr &item : items) {
        if (!out.empty()) {
            out += ", ";
        }
        out += item.str();
    }
    return out.empty() ? "(none)" : out;
}

/// The library has no name for a Kind yet, so the tour carries its own.
std::string_view kindName(mx::Kind kind) {
    switch (kind) {
    case mx::Kind::Integer:
        return "Integer";
    case mx::Kind::Rational:
        return "Rational";
    case mx::Kind::Real:
        return "Real";
    case mx::Kind::Symbol:
        return "Symbol";
    case mx::Kind::Add:
        return "Add";
    case mx::Kind::Mul:
        return "Mul";
    case mx::Kind::Pow:
        return "Pow";
    case mx::Kind::Function:
        return "Function";
    case mx::Kind::Relation:
        return "Relation";
    case mx::Kind::Opaque:
        return "Opaque";
    }
    return "?";
}

// ===========================================================================
// PART ONE — no Maxima needed
// ===========================================================================

// --- 1. Symbols and expressions ---------------------------------------------

void symbolsAndExpressions() {
    section("1. Symbols and expressions");

    // A Symbol is a named unknown. Operations that differentiate, integrate or
    // solve take a Symbol rather than any expression, so `diff(f, 2*x)` is a
    // compile error rather than a runtime surprise.
    const mx::Symbol x("x");

    // The _sym literal says the same thing with less ceremony.
    using namespace mx::literals;
    const mx::Symbol y = "y"_sym;

    // Ordinary operators build expressions. A Symbol converts to an Expr
    // wherever one is expected, and so do integers and doubles, so this reads
    // the way you would write it on paper. `pow` is found by argument-
    // dependent lookup; there is no ^ operator, because C++'s ^ is xor and
    // binds more loosely than +.
    const mx::Expr f = pow(x, 2) + 3 * x + 2;
    show("f = pow(x, 2) + 3*x + 2", f);

    // An Expr is an immutable value. Copying one copies a pointer; nothing is
    // ever modified in place, so it is safe to share between threads and to
    // keep as a map key.
    const mx::Expr g = f; // Cheap.
    show("a copy compares equal", g == f ? "true" : "false");

    // Numbers stay exact unless you ask otherwise. 1/3 is a Rational, not
    // 0.333...; 2 and 2.0 are different values, because one is exact.
    show("Expr(1) / 3", mx::Expr(1) / 3);
    show("  kind", kindName((mx::Expr(1) / 3).kind()));
    show("Expr(1.0) / 3", mx::Expr(1.0) / 3);
    show("  kind", kindName((mx::Expr(1.0) / 3).kind()));
    show("Expr::rational(2, 4)", mx::Expr::rational(2, 4)); // Reduced on sight.

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
    const std::unordered_set<mx::Expr> seen{x + 1, 1 + x};
    show("unordered_set{x + 1, 1 + x}.size()", std::to_string(seen.size()));

    // Reading a tree: kind() says what a node is, args() gives its operands.
    // Every accessor checks the kind and throws mx::Error if you ask the
    // wrong question, rather than returning something meaningless.
    show("f.kind()", kindName(f.kind()));
    for (const mx::Expr &term : f.args()) {
        show("  term", term.str() + "   (" + std::string(kindName(term.kind()))
                           + ")");
    }

    // A function the library has no builder for is still an expression: an
    // *uninterpreted* application, with a name and arguments. Maxima gives it
    // meaning when it gets there.
    const mx::Expr bessel = mx::Expr::function("bessel_j", {0, x});
    show("Expr::function(\"bessel_j\", {0, x})", bessel);
    show("  name(), arity()", bessel.name() + ", " + std::to_string(bessel.arity()));

    // Some conversions are refused at compile time on purpose. C++ would
    // happily turn true into 1 and 'a' into 97; an expression should not.
    //
    //     mx::Expr oops(true);   // error: use of deleted function
    //     x + 'a';               // error: use of deleted function
    //
    // A Maxima boolean is a symbol: Expr::symbol("true").
}

// --- 2. Exact integers ------------------------------------------------------

void exactIntegers() {
    section("2. Exact integers");

    // mx::Integer has no fixed width. Maxima produces large integers in
    // ordinary use — 30! has 33 digits — so a 64-bit type would either wrap
    // or refuse. Values that do fit in 64 bits are stored inline and never
    // allocate, which is almost all of them.
    const mx::Integer factorial30("265252859812191058636308480000000");
    show("30!", factorial30.toString());
    show("30! * 30!", (factorial30 * factorial30).toString());
    show("30! / 7", (factorial30 / 7).toString()); // Truncating, as C++ does.
    show("gcd(30!, 1001)", gcd(factorial30, 1001).toString());

    // toInt64 tells you whether a value fits a built-in type.
    show("30!.toInt64() has a value", factorial30.toInt64() ? "yes" : "no");
    show("(30! / 30!).toInt64()",
         std::to_string(*(factorial30 / factorial30).toInt64()));

    // An Integer becomes an Expr like any other number, and stays exact.
    show("Expr(30!) * 2", mx::Expr(factorial30) * 2);

    // Integer::parse reports failure instead of throwing, for text you do
    // not control; the constructor throws mx::ParseError.
    show("Integer::parse(\"12a\") has a value",
         mx::Integer::parse("12a") ? "yes" : "no");
}

// --- 3. Parsing text, without Maxima ----------------------------------------

void parsingOffline() {
    section("3. Parsing text, without Maxima");

    // Expr::parse reads infix text with no kernel involved. It accepts a
    // deliberate subset of Maxima's syntax — arithmetic, comparisons, function
    // calls, lists, strings — and produces the same expression the operators
    // in section 1 would build.
    const mx::Symbol x("x");
    const mx::Expr parsed = mx::Expr::parse("x^2 + 3*x + 2");
    show("Expr::parse(\"x^2 + 3*x + 2\")", parsed);
    show("  same as the operators build?",
         parsed == pow(x, 2) + 3 * x + 2 ? "true" : "false");

    // Precedences are Maxima's, including the two that surprise people: ^ is
    // right-associative, and unary minus binds more loosely than ^. Printed,
    // both look just like their input, so compare them with expressions built
    // explicitly to see how they were grouped.
    const mx::Expr tower = mx::Expr::parse("2^3^2");
    show("Expr::parse(\"2^3^2\")", tower);
    show("  is it 2^(3^2)?",
         tower == pow(mx::Expr(2), pow(mx::Expr(3), 2)) ? "true" : "false");
    const mx::Expr negated = mx::Expr::parse("-x^2");
    show("Expr::parse(\"-x^2\")", negated);
    show("  is it -(x^2)?", negated == -pow(x, 2) ? "true" : "false");

    // It parses; it does not evaluate. 5! stays factorial(5). mx::parse, in
    // part two, hands text to Maxima's own parser instead, which evaluates
    // as it reads and accepts everything Maxima does.
    show("Expr::parse(\"5!\")", mx::Expr::parse("5!"));

    // Malformed text throws mx::ParseError, naming where it went wrong.
    try {
        mx::Expr::parse("x + * 2");
    } catch (const mx::ParseError &error) {
        show("Expr::parse(\"x + * 2\") throws", error.what());
    }
}

// --- 4. Functions and constants ---------------------------------------------

void functionsAndConstants() {
    section("4. Functions and constants");

    // <mx/functions.hpp> has builders for the common functions. Nothing is
    // evaluated here: sin(0) stays sin(0) until Maxima is asked.
    const mx::Symbol x("x");
    show("sin(x) * cos(x)", mx::sin(x) * mx::cos(x));
    show("log(abs(x))", mx::log(mx::abs(x)));
    show("sin(0)", mx::sin(0));

    // Two are not functions at all, because Maxima has no node for them:
    // sqrt is a power of 1/2 and exp is a power of %e. Building them that way
    // keeps both sides' representations in step.
    show("sqrt(x)", mx::sqrt(x));
    show("exp(x)", mx::exp(x));

    // Constants are spelled as Maxima names them.
    show("pi(), e(), i(), inf()", mx::pi().str() + ", " + mx::e().str() + ", "
                                      + mx::i().str() + ", " + mx::inf().str());
}

// --- 5. Rendering, including a renderer of your own -------------------------

/// A renderer you might write yourself: Lisp-style prefix notation.
///
/// A renderer is a plain struct. It inherits nothing and overrides nothing;
/// mx::render only needs the operations below to exist. Each receives its
/// children *already rendered*, and says how to combine them.
///
/// The library has already made the hard decisions before these are called:
/// the sign has been pulled out of `x - 1` so it does not read `-1 + x`, `x/3`
/// arrives as a fraction rather than a product with 1/3 in it, and the
/// question of where brackets belong has been answered. A renderer only
/// chooses spellings.
struct PrefixRenderer {
    /// A renderer may keep state. See std::ref below for reading it back.
    int symbolsSeen = 0;

    // Leaves.
    std::string integer(const mx::Integer &value) { return value.toString(); }
    std::string real(double value) { return mx::Expr(value).str(); }
    std::string symbol(std::string_view name) {
        ++symbolsSeen;
        return std::string(name);
    }
    /// Maxima source text the library never interpreted. Every renderer has
    /// to decide what to do with it, so there is no default.
    std::string verbatim(std::string_view source) { return std::string(source); }

    // Arithmetic. A sum's terms arrive with their signs already separated.
    std::string sum(std::span<const mx::Term<std::string>> terms) {
        std::string out = "(+";
        for (const mx::Term<std::string> &term : terms) {
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
    std::string relation(mx::RelOp op, const std::string &lhs,
                         const std::string &rhs) {
        return "(" + std::string(mx::symbolFor(op)) + " " + lhs + " " + rhs + ")";
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
    mx::Strength strengthOf(mx::Construct) { return mx::Strength::Atom; }

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

    const mx::Symbol x("x");
    const mx::Expr quotient = (x + 1) / (x - 1);

    // Three renderers come with the library. All are local — no kernel.
    //   str()       Maxima-compatible infix, which Expr::parse reads back.
    //   toTeX()     LaTeX, for a maths environment.
    //   toMathML()  Presentation MathML, which browsers display natively.
    show("str()", quotient);
    show("toTeX()", mx::toTeX(quotient));
    show("toMathML()", mx::toMathML(quotient));

    // The same machinery is open to you: pass any renderer to mx::render.
    // The output type is deduced from the renderer — here std::string, but a
    // two-dimensional renderer returns boxes instead (examples/text2d.hpp).
    show("mx::render(e, PrefixRenderer{})",
         mx::render(quotient, PrefixRenderer{}));
    show("  and -(x + 1)", mx::render(-(x + 1), PrefixRenderer{}));
    show("  and sqrt(x), with no root()", mx::render(mx::sqrt(x), PrefixRenderer{}));

    // mx::render takes the renderer by value, so state it collects is gone
    // once it returns. Wrap it in std::ref to keep your own object.
    PrefixRenderer counting;
    mx::render(pow(x, 2) + x * mx::pi(), std::ref(counting));
    show("symbols counted, via std::ref", std::to_string(counting.symbolsSeen));

    // mx::Renderer<T> holds any renderer producing T, erased to one type:
    // choose one at run time, keep it in a member, pass it around. It is
    // move-only, and stores small renderers without allocating.
    mx::Renderer<std::string> chosen = PrefixRenderer{};
    show("through mx::Renderer<std::string>", mx::render(eq(x, 2), chosen));
}

// --- 6. Numbers from expressions ----------------------------------------------

void numericEvaluation() {
    section("6. Numbers from expressions");

    // Once you have a closed form, turning it into numbers is arithmetic, not
    // algebra, so it happens locally. Asking Maxima for each point would cost
    // a round trip per point, which makes plotting absurd.
    const mx::Symbol x("x");
    const mx::Symbol a("a");
    const mx::Expr f = a * mx::sin(x) + pow(x, 2);

    // evalNumeric: one evaluation, with symbols bound by name. Maxima's named
    // constants (%pi, %e, inf) need no binding.
    show("evalNumeric(f, {x: 1, a: 2})",
         std::to_string(mx::evalNumeric(f, {{"x", 1.0}, {"a", 2.0}})));
    show("evalNumeric(pi() / 2)", std::to_string(mx::evalNumeric(mx::pi() / 2)));

    // isEvaluable asks first, without throwing.
    show("isEvaluable(f) with nothing bound", mx::isEvaluable(f) ? "yes" : "no");

    // Otherwise, anything that cannot become a number throws mx::EvalError:
    // an unbound symbol, a function with no numeric meaning here, a relation.
    try {
        mx::evalNumeric(f, {{"x", 1.0}});
    } catch (const mx::EvalError &error) {
        show("with a left unbound", error.what());
    }

    // For many evaluations, compile once. Compiled resolves every symbol and
    // function up front into a flat instruction list, so each call is just
    // arithmetic — and errors surface here, at construction, not per point.
    const mx::Compiled compiled(f, x, {{"a", 2.0}}); // a is fixed at 2.
    std::string samples;
    for (const double at : {0.0, 0.5, 1.0, 1.5}) {
        samples += std::to_string(compiled(at)).substr(0, 6) + "  ";
    }
    show("Compiled f(x) at 0, 0.5, 1, 1.5", samples);

    // Several variables, in the order you list them.
    const std::vector<mx::Symbol> variables{x, a};
    const mx::Compiled twoVariables(f, variables);
    const double point[] = {1.0, 2.0}; // x = 1, a = 2
    show("Compiled f(x, a) at (1, 2)", std::to_string(twoVariables(point)));

    // asFunction wraps a Compiled in a std::function<double(double)>, for
    // APIs that want a callable.
    const std::function<double(double)> asCallable
        = mx::asFunction(f, x, {{"a", 2.0}});
    show("asFunction(f, x, {a: 2})(1)", std::to_string(asCallable(1.0)));

    // A Compiled can be shared between threads freely; its working space is
    // thread-local.
}

// ===========================================================================
// PART TWO — Maxima does the mathematics
// ===========================================================================

// --- 7. Calculus and algebra --------------------------------------------------

void calculusAndAlgebra() {
    section("7. Calculus and algebra");

    // Every operation takes an optional Kernel as its last argument. Leave it
    // out and the process-wide sharedKernel() is used, started on first use —
    // which is why the first call below takes a moment.
    const mx::Symbol x("x");
    const mx::Symbol y("y");
    const mx::Expr f = pow(x, 3) * mx::sin(x);

    show("diff(x^3 sin(x), x)", mx::diff(f, x));
    show("diff(x^3 sin(x), x, 2)", mx::diff(f, x, 2)); // Second derivative.
    show("expand((x + y)^3)", mx::expand(pow(x + y, 3)));
    show("factor(x^2 - y^2)", mx::factor(pow(x, 2) - pow(y, 2)));
    show("simplify((x^2 - 1)/(x - 1))", mx::simplify((pow(x, 2) - 1) / (x - 1)));
    show("subst(x^2 + y, x, 3)", mx::subst(pow(x, 2) + y, x, 3));

    // Results are expressions like any other, so they compare structurally
    // with ones you build yourself.
    show("expand((x+1)^2) == x^2 + 2x + 1",
         mx::expand(pow(x + 1, 2)) == pow(x, 2) + 2 * x + 1 ? "true" : "false");

    // Operations that can legitimately fail return std::expected. Check it
    // before using the value.
    if (const auto antiderivative = mx::integrate(f, x)) {
        show("integrate(x^3 sin(x), x)", *antiderivative);
    }
    if (const auto area = mx::integrate(pow(x, 2), x, 0, 1)) {
        show("integrate(x^2, x, 0, 1)", *area); // Exactly 1/3.
    }

    // Limits, from either side where it matters.
    if (const auto l = mx::limit(mx::sin(x) / x, x, 0)) {
        show("limit(sin(x)/x, x, 0)", *l);
    }
    if (const auto l = mx::limit(mx::Expr(1) / x, x, 0, mx::Side::FromAbove)) {
        show("limit(1/x, x, 0, FromAbove)", *l);
    }
    if (const auto l = mx::limit(mx::Expr(1) / x, x, 0, mx::Side::FromBelow)) {
        show("limit(1/x, x, 0, FromBelow)", *l);
    }

    // solve returns every solution.
    if (const auto roots = mx::solve(eq(pow(x, 2), 2), x)) {
        show("solve(x^2 = 2, x)", joined(*roots));
    }

    // A system: equations, then unknowns. Each Solution holds one value per
    // unknown, in the order you asked for them, however Maxima ordered its
    // answer.
    const std::vector<mx::Expr> equations{eq(x + y, 3), eq(x - y, 1)};
    const std::vector<mx::Symbol> unknowns{x, y};
    if (const auto solutions = mx::solve(equations, unknowns)) {
        for (const mx::Solution &solution : *solutions) {
            show("solve({x+y=3, x-y=1}, {x, y})",
                 "x = " + solution[0].str() + ", y = " + solution[1].str());
        }
    }

    // mx::parse hands text to Maxima's own parser, so it accepts all of
    // Maxima's syntax — and evaluates as it reads, unlike Expr::parse.
    if (const auto parsed = mx::parse("5!")) {
        show("mx::parse(\"5!\")", *parsed);
    }
}

// --- 8. When there is no answer ---------------------------------------------

void whenThereIsNoAnswer() {
    section("8. When there is no answer");

    const mx::Symbol x("x");

    // Failing to find a closed form is an ordinary outcome, not an error, so
    // it comes back as an mx::Failure inside the std::expected, carrying a
    // reason — usually Maxima's own words.
    const auto hopeless = mx::integrate(mx::exp(mx::sin(x)), x);
    show("integrate(e^sin(x), x)",
         hopeless ? hopeless->str() : "Failure: " + hopeless.error().message);

    // Maxima does not always signal failure as an error. For solve it hands
    // back something that is not a solution, such as x = sin(x); the library
    // recognises that and reports a Failure, so a success really is one.
    const auto circular = mx::solve(eq(x, mx::sin(x)), x);
    show("solve(x = sin(x), x)",
         circular ? "solved?" : "Failure: " + circular.error().message);

    // Some questions Maxima itself rejects, like a divergent integral. That
    // arrives as a Failure too, in Maxima's words.
    const auto divergent = mx::integrate(mx::Expr(1) / x, x, 0, 1);
    show("integrate(1/x, x, 0, 1)",
         divergent ? divergent->str() : "Failure: " + divergent.error().message);

    // An infinite answer is still an answer. From both sides at once, 1/x
    // grows without a sign, and Maxima says `infinity` — its complex
    // infinity, not the inf and minf of the one-sided limits in section 7.
    const auto unsigned_ = mx::limit(mx::Expr(1) / x, x, 0);
    show("limit(1/x, x, 0), both sides",
         unsigned_ ? unsigned_->str() : "Failure: " + unsigned_.error().message);

    // Not every limit that fails to exist is a Failure, and this one is worth
    // knowing about. abs(x)/x is -1 on one side of 0 and 1 on the other.
    // Maxima answers `ind` — indefinite, but bounded — and that arrives as a
    // *successful* result holding the symbol ind. Only Maxima's `und`
    // (undefined) is reported as a Failure. Where it matters, check for ind
    // yourself.
    const auto bounded = mx::limit(mx::abs(x) / x, x, 0);
    const bool indefinite = bounded && bounded->is(mx::Kind::Symbol)
                            && bounded->name() == "ind";
    show("limit(abs(x)/x, x, 0)",
         indefinite ? "ind: bounded, but no single value"
         : bounded  ? bounded->str()
                    : "Failure: " + bounded.error().message);

    // Operations with no ordinary way to fail — diff, expand, factor,
    // simplify, subst — throw mx::MaximaError instead, because a failure
    // there means something is wrong with the question. An Opaque node holds
    // Maxima source the library never interpreted, and this one does not
    // parse.
    try {
        mx::diff(mx::Expr::opaque("(1"), x);
    } catch (const mx::MaximaError &error) {
        show("diff(<unparseable text>, x) throws", error.what());
    }
}

// --- 9. Assumptions -------------------------------------------------------------

void assumptions() {
    section("9. Assumptions");

    const mx::Symbol x("x");
    const mx::Symbol n("n");
    const mx::Symbol k("k");

    // Some answers depend on facts Maxima has not been told. Interactively it
    // would ask; over a pipe it cannot, so the question comes back as a
    // Failure naming exactly the fact that is missing.
    const auto unknown = mx::integrate(pow(x, n), x);
    show("integrate(x^n, x)",
         unknown ? unknown->str() : "Failure: " + unknown.error().message);

    // Supply facts in an mx::Context. Everything assumed inside it is
    // discarded when it goes out of scope — a real Maxima context, not a
    // best-effort undo.
    {
        mx::Context scope;
        scope.assume(gt(n, 0));
        if (const auto known = mx::integrate(pow(x, n), x)) {
            show("  with n > 0", *known);
        }

        // The same expression can simplify differently under different facts.
        show("  sqrt(x^2), nothing assumed of x", mx::expand(mx::sqrt(pow(x, 2))));
        scope.assume(gt(x, 0));
        show("  sqrt(x^2), with x > 0", mx::expand(mx::sqrt(pow(x, 2))));

        // Contexts nest.
        {
            mx::Context inner;

            // An inner scope still sees what the outer one assumed...
            show("    sqrt(x^2), inherited x > 0", mx::expand(mx::sqrt(pow(x, 2))));

            // ...and adds facts of its own. declare records a property of a
            // symbol, rather than a relation.
            inner.declare(k, mx::Feature::Integer);
            show("    sin(k*pi), k declared integer",
                 mx::expand(mx::sin(k * mx::pi())));

            // facts() lists the facts established in the scope itself, not
            // those it inherited: here, only the declaration.
            show("    inner.facts()", joined(inner.facts()));
        } // k is no longer an integer here.

        show("  sin(k*pi), after the inner scope", mx::expand(mx::sin(k * mx::pi())));
        show("  scope.facts()", joined(scope.facts()));
    } // n > 0 and x > 0 are gone here.

    const auto again = mx::integrate(pow(x, n), x);
    show("integrate(x^n, x), after the scope",
         again ? again->str() : "Failure again, as it should be");

    // Contexts survive a kernel restart: they are recorded and replayed, so a
    // Maxima that crashed mid-scope comes back with the same facts in force.
    // An assumption that contradicts one already in force throws MaximaError.
}

// --- 10. The kernel, configuration and caching --------------------------------

void theKernel() {
    section("10. The kernel, configuration and caching");

    const mx::Symbol x("x");
    const mx::Symbol y("y");
    const mx::Symbol z("z");

    // You can run a Kernel of your own instead of the shared one: several
    // kernels are several Maxima processes, which is how to get real
    // parallelism. A Kernel is safe to share between threads, but calls on
    // one kernel take turns.
    //
    // mx::Config configures it. Every field has a sensible default:
    //   maximaRoot      where Maxima is installed; empty means discover it
    //   timeout         how long one call may take (default two minutes)
    //   startupTimeout  how long starting or restarting may take
    //   cacheEntries    in-memory reply cache size; 0 disables it
    //   cacheDirectory  keep replies between runs; empty means do not
    //   loadUserInit    read the user's maxima-init.mac (default: no, so
    //                   results do not vary from machine to machine)
    //   userDir         where Maxima keeps user state when that is off

    // --- caching between runs --------------------------------------------
    //
    // With cacheDirectory set, answers are written to disk and shared by
    // every kernel using the same directory, in this process or another.
    // Every entry is keyed on the Maxima version, this library's version and
    // the assumptions in force, so an answer is only reused under the
    // conditions that produced it.
    const std::filesystem::path cacheDirectory
        = std::filesystem::temp_directory_path() / "maxima_cpp_tour_cache";
    std::error_code ignored;
    std::filesystem::remove_all(cacheDirectory, ignored); // Start clean.

    mx::Config config;
    config.cacheDirectory = cacheDirectory;

    const mx::Expr question = pow(x, 5) * mx::exp(x);
    {
        mx::Kernel first(config);
        mx::integrate(question, x, first); // Computed by Maxima, written to disk.
    }
    mx::Kernel second(config); // A fresh Maxima process, same directory.
    const auto fromDisk = mx::integrate(question, x, second);
    show("integrate(x^5 e^x), second kernel", fromDisk ? fromDisk->str() : "Failure");
    show("  answers that came from disk",
         std::to_string(second.cacheStats().persistentHits));

    // --- talking to Maxima directly --------------------------------------
    //
    // Beneath the operations, a Kernel evaluates anything you give it and
    // hands back Maxima's reply as a Reply: ok, and either the value — as
    // the text of Maxima's internal form — or Maxima's reason for failing.
    //
    // Prefer evalPure for questions. It uses the cache, on the promise that
    // the text changes nothing in Maxima.
    const mx::Reply pi = second.evalPure("float(%pi)");
    show("evalPure(\"float(%pi)\")", pi.ok ? pi.value : pi.reason);

    // An Expr can be sent instead of text; it travels as structure, so
    // nothing about it can be misread. Note the reply is Maxima's internal
    // form, here the rational 1/2.
    const mx::Reply viaExpr = second.evalPure(mx::sin(mx::pi() / 6));
    show("evalPure(sin(pi()/6))", viaExpr.ok ? viaExpr.value : viaExpr.reason);

    // Text Maxima cannot parse is an ordinary failure with a reason, not an
    // exception, and costs one round trip.
    const mx::Reply broken = second.evalPure("(1");
    show("evalPure(\"(1\") ok?", broken.ok ? "yes" : "no: " + broken.reason);

    // Asking the same question again is answered from memory.
    const auto before = second.cacheStats();
    second.evalPure("float(%pi)");
    show("the same question again: cache hits",
         std::to_string(before.hits) + " -> "
             + std::to_string(second.cacheStats().hits));

    // eval, as opposed to evalPure, is for statements that change Maxima —
    // an assignment, a definition. Since there is no telling from the text
    // what changed, it empties the cache, and turns off the on-disk cache for
    // this kernel for good. Use it only when you mean it.
    const mx::Reply assigned = second.eval("tour_value: 42");
    show("eval(\"tour_value: 42\")", assigned.ok ? assigned.value : assigned.reason);

    // --- timeouts and recovery -------------------------------------------
    //
    // A call that runs past its timeout throws mx::TimeoutError. Before it
    // does, the kernel is restarted and its assumptions replayed, so only
    // that call is lost: the next one works. setTimeout changes the deadline
    // for later calls. The restart takes a moment.
    second.setTimeout(std::chrono::milliseconds(1));
    try {
        mx::expand(pow(x + y + z, 200), second);
    } catch (const mx::TimeoutError &error) {
        show("expand((x+y+z)^200), 1 ms timeout", error.what());
    }
    second.setTimeout(std::chrono::minutes(2));
    show("  and the next call", mx::expand(pow(x + 1, 2), second));

    std::filesystem::remove_all(cacheDirectory, ignored); // Tidy up.
}

} // namespace

// ===========================================================================

int main() {
    std::cout << "maxima_cpp " << mx::version << ": a tour\n";

    // Part one is plain C++. Any mx::Error here would be a bug in the tour.
    try {
        symbolsAndExpressions();
        exactIntegers();
        parsingOffline();
        functionsAndConstants();
        rendering();
        numericEvaluation();
    } catch (const mx::Error &error) {
        std::cerr << "\nunexpected error in part one: " << error.what() << "\n";
        return 1;
    }

    // Part two needs Maxima. The error types form a small hierarchy under
    // mx::Error, and the split that matters is between two kinds of failure:
    //
    //   mx::KernelError   the conversation with Maxima broke down: it could
    //                     not be found or started, it died, or (as
    //                     mx::TimeoutError) it did not answer in time
    //   mx::MaximaError   Maxima answered, with an error, to a question that
    //                     should not fail
    //   mx::EvalError     local numeric evaluation was impossible
    //   mx::ParseError    text could not be read
    //
    // A *mathematical* failure — no closed form, no solution — is none of
    // these. It is an mx::Failure returned in a std::expected (section 8).
    try {
        calculusAndAlgebra();
        whenThereIsNoAnswer();
        assumptions();
        theKernel();
    } catch (const mx::KernelError &error) {
        std::cerr << "\nPart two needs Maxima, which could not be used:\n  "
                  << error.what()
                  << "\nPart one above ran without it. See 'Requirements' in "
                     "README.md for installing Maxima.\n";
        return 1;
    } catch (const mx::Error &error) {
        std::cerr << "\nunexpected error in part two: " << error.what() << "\n";
        return 1;
    }

    std::cout << "\nEnd of the tour.\n";
    return 0;
}
