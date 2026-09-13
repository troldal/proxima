// Renderers: the shared presentation layer, and user-supplied renderers.
//
// Nothing here needs Maxima. The point of the layer is that rendering is
// local, so these are all pure.

#include <doctest/doctest.h>

#include <mx/expr.hpp>
#include <mx/functions.hpp>
#include <mx/render.hpp>
#include <mx/symbol.hpp>
#include <mx/mathml.hpp>
#include <mx/tex.hpp>

#include <algorithm>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using mx::Construct;
using mx::Expr;
using mx::Slot;
using mx::Strength;
using mx::Symbol;
using mx::Term;

TEST_CASE("the presentation layer fixes what every renderer would get wrong") {
    // Each of these was a defect in a TeX renderer written directly against
    // Expr's accessors, and each is a presentation decision rather than a
    // question about TeX — so each is made once, in the shared layer, and
    // shows up in both renderers below.
    const Symbol x("x");
    const Symbol y("y");

    SUBCASE("canonical order is not display order") {
        // Numbers sort first, so the tree says -1 + x.
        CHECK((Expr(x) - 1).str() == "x - 1");
        CHECK(mx::toTeX(Expr(x) - 1) == "x - 1");
    }
    SUBCASE("a reciprocal is a fraction, not a negative power") {
        CHECK((Expr(1) / Expr(x)).str() == "1/x");
        CHECK(mx::toTeX(Expr(1) / Expr(x)) == "\\frac{1}{x}");
    }
    SUBCASE("a rational coefficient is dismantled") {
        // Not a product containing 1/3.
        CHECK((Expr(x) / Expr(3)).str() == "x/3");
        CHECK(mx::toTeX(Expr(x) / Expr(3)) == "\\frac{x}{3}");
    }
    SUBCASE("negation is a sign, not a factor of -1") {
        CHECK((-(Expr(x) + 1)).str() == "-(1 + x)");
        CHECK(mx::toTeX(-(Expr(x) + 1)) == "-\\left(1 + x\\right)");
        CHECK((-Expr(x) - Expr(y)).str() == "-x - y");
    }
    SUBCASE("a negated term keeps its brackets") {
        // This one used to print `x - 1 + y`, which reads back as a different
        // expression: the sign only reached the first term of the bracket.
        const Expr subtracted = Expr(x) - (Expr(1) + Expr(y));
        CHECK(subtracted.str() == "x - (1 + y)");
        CHECK(Expr::parse(subtracted.str()) == subtracted);
    }
}

TEST_CASE("infix output still reads back as the same expression") {
    // str() is no longer how expressions reach Maxima, but it is still meant
    // to be Maxima-compatible, and Expr::parse is the check that costs no
    // kernel.
    const Symbol x("x");
    const Symbol y("y");

    const Expr corpus[] = {
        Expr(x) + 1,
        Expr(x) - 1,
        Expr(1) / Expr(x),
        Expr(2) * Expr(x) / Expr(3),
        (Expr(x) + 1) / (Expr(x) - 1),
        -(Expr(x) + 1),
        Expr(x) - (Expr(1) + Expr(y)),
        -Expr(x) - Expr(y),
        pow(Expr(x), 2),
        pow(-Expr(x), 2),
        pow(pow(Expr(x), 2), 3),
        mx::sqrt(Expr(x)),
        // Compound radicands. Every root here used to have a bare symbol under
        // it, which is how `1 - x^2^(1/2)` got past this test.
        mx::sqrt(Expr(x) + 1),
        mx::sqrt(Expr(1) - pow(Expr(x), 2)),
        Expr(x) * mx::sqrt(Expr(1) - pow(Expr(x), 2)) / Expr(2),
        pow(Expr(x) * Expr(y), Expr::rational(1, 3)),
        pow(pow(Expr(x), 2), Expr::rational(1, 2)),
        mx::exp(Expr(x)),
        mx::sin(Expr(x)) / mx::cos(Expr(x)),
        Expr::function("list", {Expr(1), Expr(x)}),
        eq(pow(Expr(x), 2), Expr(y)),
        Expr::rational(-1, 2),
    };
    for (const Expr &expr : corpus) {
        CAPTURE(expr.str());
        CHECK(Expr::parse(expr.str()) == expr);
    }
}

TEST_CASE("a root is spelled as a power where that is what reads back") {
    // InfixRenderer defines no root(), so it gets the synthesised default —
    // and the default has to apply the same grouping the walk would, or it
    // emits `x^1/2`, which is `(x^1)/2`.
    CHECK(mx::sqrt(Expr(Symbol("x"))).str() == "x^(1/2)");
    CHECK(pow(Expr(Symbol("x")), Expr::rational(1, 3)).str() == "x^(1/3)");

    SUBCASE("and the radicand is grouped as a power base") {
        // Regression: this printed `1 - x^2^(1/2)`, a different expression,
        // because the default root was assembled from already-rendered text
        // that could not say it needed brackets. It is now a Power node,
        // walked like any other.
        const Symbol x("x");
        CHECK(mx::sqrt(Expr(1) - pow(Expr(x), 2)).str() == "(1 - x^2)^(1/2)");
        CHECK(mx::sqrt(Expr(x) + 1).str() == "(1 + x)^(1/2)");
        CHECK(mx::sqrt(pow(Expr(x), 2)).str() == "(x^2)^(1/2)");
        // TeX has a radical, so it never needed the brackets.
        CHECK(mx::toTeX(mx::sqrt(Expr(1) - pow(Expr(x), 2)))
              == "\\sqrt{1 - x^{2}}");
    }
    // TeX does define root(), so it gets a radical.
    CHECK(mx::toTeX(mx::sqrt(Expr(Symbol("x")))) == "\\sqrt{x}");
    CHECK(mx::toTeX(pow(Expr(Symbol("x")), Expr::rational(1, 3)))
          == "\\sqrt[3]{x}");
}

TEST_CASE("TeX") {
    const Symbol x("x");
    const Symbol y("y");

    CHECK(mx::toTeX(pow(Expr(x), 2) + 3 * Expr(x) + 2) == "2 + x^{2} + 3 x");
    CHECK(mx::toTeX(Expr::rational(11, 15)) == "\\frac{11}{15}");
    CHECK(mx::toTeX(mx::sin(Expr(x))) == "\\sin\\left(x\\right)");
    CHECK(mx::toTeX(eq(pow(Expr(x), 2), Expr(y))) == "x^{2} = y");
    CHECK(mx::toTeX(mx::pi()) == "\\pi");
    CHECK(mx::toTeX(Expr::function("list", {Expr(1), Expr(x)}))
          == "\\left[1, x\\right]");

    SUBCASE("a brace-delimited slot needs no brackets") {
        // The infix renderer must bracket both sides here; TeX must not.
        const Expr quotient = (Expr(x) + 1) / (Expr(x) - 1);
        CHECK(quotient.str() == "(1 + x)/(x - 1)");
        CHECK(mx::toTeX(quotient) == "\\frac{1 + x}{x - 1}");
        CHECK(mx::toTeX(pow(Expr(x), Expr(y) + 1)) == "x^{1 + y}");
    }
    SUBCASE("but a bracket is still a bracket where TeX has none of its own") {
        CHECK(mx::toTeX(pow(Expr(x) + 1, 2)) == "\\left(1 + x\\right)^{2}");
        CHECK(mx::toTeX(pow(Expr(-3), 2)) == "\\left(-3\\right)^{2}");
    }
    SUBCASE("a name that is TeX markup is escaped") {
        // The underscore is a subscript; emitting it raw produces a document
        // that does not compile.
        CHECK(mx::toTeX(Expr::function("bessel_j", {Expr(0), Expr(x)}))
              == "\\operatorname{bessel\\_j}\\left(0, x\\right)");
        CHECK(mx::toTeX(Expr(Symbol("x_1"))) == "\\mathit{x\\_1}");
    }
    SUBCASE("a multi-letter name is not a product of its letters") {
        CHECK(mx::toTeX(Expr(Symbol("mass"))) == "\\mathit{mass}");
        CHECK(mx::toTeX(Expr(Symbol("alpha"))) == "\\alpha");
    }
}

// --- a user-supplied renderer, over a type that is not a string -------------
//
// The reason the interface is generic over its output type. Two-dimensional
// layout needs each subexpression's width, height and baseline before it can
// place it; an interface fixed to std::string would foreclose exactly the
// renderer that most wants this.

namespace {

struct Box {
    std::vector<std::string> lines{""};
    /// Index of the line the expression sits on, so that `a + b/c` aligns the
    /// plus with the fraction's rule rather than with its numerator.
    std::size_t baseline = 0;

    std::size_t width() const {
        std::size_t widest = 0;
        for (const std::string &line : lines) {
            widest = std::max(widest, line.size());
        }
        return widest;
    }
};

Box text(std::string_view value) {
    return Box{{std::string(value)}, 0};
}

/// Pads to a given width and puts `above` blank lines over the baseline.
Box aligned(const Box &box, std::size_t above, std::size_t height) {
    Box out;
    out.lines.clear();
    const std::size_t width = box.width();
    const std::size_t leading = above - box.baseline;
    for (std::size_t i = 0; i < height; ++i) {
        if (i >= leading && i - leading < box.lines.size()) {
            std::string line = box.lines[i - leading];
            line.resize(width, ' ');
            out.lines.push_back(std::move(line));
        } else {
            out.lines.push_back(std::string(width, ' '));
        }
    }
    out.baseline = above;
    return out;
}

/// Side by side, baselines aligned.
Box beside(const Box &left, const Box &right) {
    const std::size_t above = std::max(left.baseline, right.baseline);
    const std::size_t below
        = std::max(left.lines.size() - left.baseline,
                   right.lines.size() - right.baseline);
    const std::size_t height = above + below;

    const Box a = aligned(left, above, height);
    const Box b = aligned(right, above, height);

    Box out;
    out.lines.clear();
    for (std::size_t i = 0; i < height; ++i) {
        out.lines.push_back(a.lines[i] + b.lines[i]);
    }
    out.baseline = above;
    return out;
}

/// The renderer itself: a plain struct, inheriting nothing.
struct Layout {
    Box integer(const mx::Integer &value) { return text(value.toString()); }
    Box real(double value) { return text(std::to_string(value)); }
    Box symbol(std::string_view name) { return text(name); }
    Box verbatim(std::string_view source) { return text(source); }

    Box sum(std::span<const Term<Box>> terms) {
        Box out = terms.empty() ? text("") : terms[0].value;
        if (!terms.empty() && terms[0].negated) {
            out = beside(text("-"), out);
        }
        for (std::size_t i = 1; i < terms.size(); ++i) {
            out = beside(out, text(terms[i].negated ? " - " : " + "));
            out = beside(out, terms[i].value);
        }
        return out;
    }

    Box product(std::span<const Box> factors) {
        Box out = factors.empty() ? text("") : factors[0];
        for (std::size_t i = 1; i < factors.size(); ++i) {
            out = beside(beside(out, text("*")), factors[i]);
        }
        return out;
    }

    /// The whole reason for boxes: the numerator sits above the rule and the
    /// baseline lands on the rule, so surrounding terms line up with it.
    Box fraction(const Box &numerator, const Box &denominator) {
        const std::size_t width
            = std::max(numerator.width(), denominator.width());
        Box out;
        out.lines.clear();
        const auto centred = [width](const std::string &line) {
            const std::size_t pad = (width - line.size()) / 2;
            std::string padded(pad, ' ');
            padded += line;
            padded.resize(width, ' ');
            return padded;
        };
        for (const std::string &line : numerator.lines) {
            out.lines.push_back(centred(line));
        }
        out.baseline = out.lines.size();
        out.lines.push_back(std::string(width, '-'));
        for (const std::string &line : denominator.lines) {
            out.lines.push_back(centred(line));
        }
        return out;
    }

    /// A superscript really is raised, which is what the user asked for.
    Box power(const Box &base, const Box &exponent) {
        Box out;
        out.lines.clear();
        const std::size_t baseWidth = base.width();
        for (const std::string &line : exponent.lines) {
            out.lines.push_back(std::string(baseWidth, ' ') + line);
        }
        const std::size_t raised = exponent.lines.size();
        for (const std::string &line : base.lines) {
            std::string padded = line;
            padded.resize(baseWidth, ' ');
            out.lines.push_back(padded);
        }
        out.baseline = base.baseline + raised;
        return out;
    }

    Box call(std::string_view head, std::span<const Box> args) {
        Box out = beside(text(head), text("("));
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i != 0) {
                out = beside(out, text(", "));
            }
            out = beside(out, args[i]);
        }
        return beside(out, text(")"));
    }

    Box relation(mx::RelOp op, const Box &lhs, const Box &rhs) {
        return beside(beside(lhs, text(" " + std::string(mx::symbolFor(op)) + " ")),
                      rhs);
    }

    /// Brackets that grow with what they contain.
    Box group(const Box &inner) {
        if (inner.lines.size() == 1) {
            return beside(beside(text("("), inner), text(")"));
        }
        Box left;
        Box right;
        left.lines.clear();
        right.lines.clear();
        for (std::size_t i = 0; i < inner.lines.size(); ++i) {
            const bool top = i == 0;
            const bool bottom = i + 1 == inner.lines.size();
            left.lines.push_back(top ? "/" : (bottom ? "\\" : "|"));
            right.lines.push_back(top ? "\\" : (bottom ? "/" : "|"));
        }
        left.baseline = inner.baseline;
        right.baseline = inner.baseline;
        return beside(beside(left, inner), right);
    }

    // No root(), no list(), no negate(): all three are synthesised from the
    // operations above.

    Strength strengthOf(Construct construct) {
        // A drawn fraction needs no brackets of its own.
        return construct == Construct::Fraction ? Strength::Atom
                                                : mx::defaultStrength(construct);
    }

    Strength contextFor(Slot slot) {
        return slot == Slot::Numerator || slot == Slot::Denominator
                   ? Strength::Loosest
                   : mx::defaultContext(slot);
    }
};

std::string drawn(const Expr &expr) {
    const Box box = mx::render(expr, Layout{});
    std::string out;
    for (std::size_t i = 0; i < box.lines.size(); ++i) {
        if (i != 0) {
            out += "\n";
        }
        std::string line = box.lines[i];
        while (!line.empty() && line.back() == ' ') {
            line.pop_back();
        }
        out += line;
    }
    return out;
}

} // namespace

TEST_CASE("a renderer whose output is not a string") {
    const Symbol x("x");
    const Symbol y("y");

    SUBCASE("a fraction is stacked, and the baseline lands on the rule") {
        CHECK(drawn((Expr(x) + 1) / (Expr(y) - 1)) == "1 + x\n-----\ny - 1");
    }
    SUBCASE("an exponent is raised") {
        CHECK(drawn(pow(Expr(x), 2)) == " 2\nx");
    }
    SUBCASE("and the two compose") {
        //  1
        //  --
        //   2
        //  x
        CHECK(drawn(Expr(1) / pow(Expr(x), 2)) == "1\n--\n 2\nx");
    }
    SUBCASE("brackets grow with what they contain") {
        // A multi-line sum used as a factor has to be bracketed, and the
        // renderer draws the bracket at the height of what it holds:
        //
        //        /    1\
        //  sin(x)|1 + -|
        //        \    y/
        const std::string picture
            = drawn(mx::sin(Expr(x)) * (Expr(1) / Expr(y) + 1));
        CHECK(picture.find('/') != std::string::npos);
        CHECK(picture.find('|') != std::string::npos);
        CHECK(picture.find('\\') != std::string::npos);
    }
    SUBCASE("a delimited slot is not bracketed twice") {
        // The call already supplies parentheses, so the fraction inside needs
        // none of its own.
        CHECK(drawn(mx::sin(Expr(1) / Expr(x))) == "    1\nsin(-)\n    x");
    }
}

TEST_CASE("optional operations are synthesised from the required ones") {
    // Layout defines neither root(), list() nor negate(), and renders all
    // three anyway — through its own power(), call() and sum().
    const Symbol x("x");

    CHECK(drawn(mx::sqrt(Expr(x))).find("1") != std::string::npos); // x^(1/2)
    CHECK(drawn(Expr::function("list", {Expr(1), Expr(2)})) == "list(1, 2)");
    CHECK(drawn(-(Expr(x) + 1)) == "-(1 + x)");
}

TEST_CASE("a renderer can be held by reference and read afterwards") {
    // The wrapper owns what it is given, so a renderer accumulating state
    // would otherwise be unreachable once rendering finished. std::ref is the
    // way to keep your own object.
    struct Counting {
        int symbols = 0;
        std::string integer(const mx::Integer &v) { return v.toString(); }
        std::string real(double v) { return std::to_string(v); }
        std::string symbol(std::string_view n) {
            ++symbols;
            return std::string(n);
        }
        std::string verbatim(std::string_view s) { return std::string(s); }
        std::string sum(std::span<const Term<std::string>> terms) {
            std::string out;
            for (const Term<std::string> &term : terms) {
                if (!out.empty()) {
                    out += term.negated ? "-" : "+";
                }
                out += term.value;
            }
            return out;
        }
        std::string product(std::span<const std::string> factors) {
            std::string out;
            for (const std::string &factor : factors) {
                out += factor;
            }
            return out;
        }
        std::string fraction(const std::string &a, const std::string &b) {
            return a + "/" + b;
        }
        std::string power(const std::string &a, const std::string &b) {
            return a + "^" + b;
        }
        std::string call(std::string_view head,
                         std::span<const std::string> args) {
            std::string out(head);
            out += "(";
            for (const std::string &arg : args) {
                out += arg;
            }
            return out + ")";
        }
        std::string relation(mx::RelOp, const std::string &a,
                             const std::string &b) {
            return a + "=" + b;
        }
        std::string group(const std::string &inner) { return "(" + inner + ")"; }
    };

    const Symbol x("x");
    const Symbol y("y");

    Counting counter;
    const std::string out
        = mx::render(Expr(x) + Expr(y) + Expr(x), std::ref(counter));
    CHECK(out == "x+x+y");
    // Reachable afterwards, which is the whole point of std::ref here.
    CHECK(counter.symbols == 3);

    SUBCASE("whereas a renderer passed by value is consumed") {
        Counting owned;
        static_cast<void>(mx::render(Expr(x) + Expr(y), owned));
        // `owned` was copied into the wrapper; the copy did the counting.
        CHECK(owned.symbols == 0);
    }
}

TEST_CASE("the erased renderer is a movable value") {
    struct Trivial {
        std::string integer(const mx::Integer &v) { return v.toString(); }
        std::string real(double) { return "r"; }
        std::string symbol(std::string_view n) { return std::string(n); }
        std::string verbatim(std::string_view) { return "?"; }
        std::string sum(std::span<const Term<std::string>>) { return "+"; }
        std::string product(std::span<const std::string>) { return "*"; }
        std::string fraction(const std::string &, const std::string &) {
            return "/";
        }
        std::string power(const std::string &, const std::string &) {
            return "^";
        }
        std::string call(std::string_view, std::span<const std::string>) {
            return "f";
        }
        std::string relation(mx::RelOp, const std::string &,
                             const std::string &) {
            return "=";
        }
        std::string group(const std::string &s) { return s; }
    };

    mx::Renderer<std::string> renderer{Trivial{}};
    CHECK(mx::render(Expr(7), renderer) == "7");

    mx::Renderer<std::string> moved = std::move(renderer);
    CHECK(mx::render(Expr(8), moved) == "8");

    SUBCASE("and one type holds any renderer") {
        // The point of erasing: these are the same type despite being
        // different renderers.
        std::vector<mx::Renderer<std::string>> renderers;
        renderers.emplace_back(Trivial{});
        renderers.emplace_back(std::move(moved));
        CHECK(renderers.size() == 2);
        CHECK(mx::render(Expr(9), renderers[0]) == "9");
    }
}

// --- MathML -----------------------------------------------------------------

namespace {

constexpr std::string_view kMathOpen
    = R"(<math xmlns="http://www.w3.org/1998/Math/MathML">)";
constexpr std::string_view kMathClose = "</math>";

/// toMathML without the <math> wrapper, which every case would otherwise
/// repeat.
std::string mathml(const Expr &expr) {
    const std::string out = mx::toMathML(expr);
    REQUIRE(out.starts_with(kMathOpen));
    REQUIRE(out.ends_with(kMathClose));
    return out.substr(kMathOpen.size(),
                      out.size() - kMathOpen.size() - kMathClose.size());
}

/// "ok", or what is wrong with the markup: unbalanced or unknown tags, an
/// <mfrac>, <msup> or <mroot> without exactly two children, a bare ampersand,
/// or a byte outside ASCII.
std::string wellFormed(const std::string &xml) {
    static constexpr std::string_view kKnown[] = {
        "math", "mrow", "mi", "mn", "mo", "mtext", "mfrac", "msup", "msqrt",
        "mroot"};
    struct Open {
        std::string tag;
        int children = 0;
    };
    std::vector<Open> stack;
    for (std::size_t at = xml.find('<'); at != std::string::npos;
         at = xml.find('<', at)) {
        const std::size_t close = xml.find('>', at);
        if (close == std::string::npos) {
            return "unterminated tag";
        }
        std::string inside = xml.substr(at + 1, close - at - 1);
        at = close + 1;
        const bool closing = inside.starts_with('/');
        if (closing) {
            inside.erase(0, 1);
        }
        const std::string name = inside.substr(0, inside.find(' '));
        if (std::find(std::begin(kKnown), std::end(kKnown), name)
            == std::end(kKnown)) {
            return "unknown element <" + name + ">";
        }
        if (!closing) {
            if (!stack.empty()) {
                ++stack.back().children;
            }
            stack.push_back({name, 0});
            continue;
        }
        if (stack.empty() || stack.back().tag != name) {
            return "mismatched </" + name + ">";
        }
        const Open done = stack.back();
        stack.pop_back();
        if ((name == "mfrac" || name == "msup" || name == "mroot")
            && done.children != 2) {
            return "<" + name + "> with " + std::to_string(done.children)
                   + " children";
        }
    }
    if (!stack.empty()) {
        return "unclosed <" + stack.back().tag + ">";
    }
    for (std::size_t amp = xml.find('&'); amp != std::string::npos;
         amp = xml.find('&', amp + 1)) {
        const std::size_t semi = xml.find(';', amp);
        if (semi == std::string::npos || semi - amp > 10) {
            return "bare ampersand";
        }
    }
    for (const char c : xml) {
        if (static_cast<unsigned char>(c) > 127) {
            return "non-ASCII byte";
        }
    }
    return "ok";
}

} // namespace

TEST_CASE("MathML") {
    const Symbol x("x");
    const Symbol y("y");

    CHECK(mathml(Expr(x) - 1) == "<mrow><mi>x</mi><mo>&#x2212;</mo><mn>1</mn></mrow>");
    CHECK(mathml(2 * Expr(x))
          == "<mrow><mn>2</mn><mo>&#x2062;</mo><mi>x</mi></mrow>");
    CHECK(mathml(mx::pi()) == "<mi>&#x3C0;</mi>");
    CHECK(mathml(le(x, y)) == "<mrow><mi>x</mi><mo>&#x2264;</mo><mi>y</mi></mrow>");

    SUBCASE("structure maps onto elements") {
        CHECK(mathml(pow(Expr(x), Expr::rational(1, 3)))
              == "<mroot><mi>x</mi><mn>3</mn></mroot>");
        CHECK(mathml(mx::sqrt(Expr(1) - pow(Expr(x), 2)))
              == "<msqrt><mrow><mn>1</mn><mo>&#x2212;</mo>"
                 "<msup><mi>x</mi><mn>2</mn></msup></mrow></msqrt>");
    }
    SUBCASE("a fraction bar and a raised exponent need no brackets") {
        CHECK(mathml((Expr(x) + 1) / (Expr(x) - 1))
              == "<mfrac><mrow><mn>1</mn><mo>+</mo><mi>x</mi></mrow>"
                 "<mrow><mi>x</mi><mo>&#x2212;</mo><mn>1</mn></mrow></mfrac>");
        CHECK(mathml(pow(Expr(x), Expr(y) + 1))
              == "<msup><mi>x</mi><mrow><mn>1</mn><mo>+</mo><mi>y</mi></mrow></msup>");
    }
    SUBCASE("but the base of a power does") {
        // Without the brackets, a 2 raised beside x+1 would not say whether
        // it applied to the whole sum.
        CHECK(mathml(pow(Expr(x) + 1, 2))
              == "<msup><mrow><mo>(</mo><mrow><mn>1</mn><mo>+</mo><mi>x</mi></mrow>"
                 "<mo>)</mo></mrow><mn>2</mn></msup>");
    }
    SUBCASE("text is escaped") {
        CHECK(mathml(Expr(Symbol("a<b"))) == "<mi>a&lt;b</mi>");
        CHECK(mathml(Expr::opaque("\"R&D\"")) == "<mtext>&quot;R&amp;D&quot;</mtext>");
    }
    SUBCASE("a small real has an unpadded exponent") {
        // to_chars writes 1e-07. TeX had the same padding.
        CHECK(mathml(Expr(1e-7))
              == "<mrow><mn>1</mn><mo>&#xD7;</mo><msup><mn>10</mn>"
                 "<mrow><mo>&#x2212;</mo><mn>7</mn></mrow></msup></mrow>");
        CHECK(mx::toTeX(Expr(1e-7)) == "1 \\times 10^{-7}");
        CHECK(mx::toTeX(Expr(1e300)) == "1 \\times 10^{300}");
    }
}

TEST_CASE("MathML output is well formed for every kind of node") {
    const Symbol x("x");
    const Symbol y("y");
    const Symbol a("a");
    const Symbol b("b");
    const Symbol c("c");

    const Expr corpus[] = {
        Expr(x) + 1,
        -(Expr(x) + 1),
        Expr::rational(-1, 2),
        Expr(-2.5) * Expr(x),
        Expr(1e300),
        Expr(std::numeric_limits<double>::infinity()),
        mx::minusInf(),
        mx::sin(Expr(x)) / mx::cos(Expr(x)),
        pow(pow(Expr(x), 2), 3),
        pow(Expr(x) * Expr(y), Expr::rational(1, 3)),
        ne(Expr(x), Expr(0)),
        Expr::function("list", {Expr(1), Expr(x), mx::sin(Expr(y))}),
        Expr::function("bessel_j", {Expr(0), Expr(x)}),
        Expr::opaque("<script>&"),
        (mx::sqrt(pow(Expr(b), 2) - 4 * Expr(a) * Expr(c)) - Expr(b))
            / (2 * Expr(a)),
    };
    for (const Expr &expr : corpus) {
        const std::string out = mx::toMathML(expr);
        CAPTURE(out);
        CHECK(wellFormed(out) == "ok");
    }
}
