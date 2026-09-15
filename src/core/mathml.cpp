#include <mx/mathml.hpp>

#include <mx/render.hpp>

#include <charconv>
#include <cmath>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

// A Presentation MathML renderer, written as an ordinary user of
// mx/render.hpp — a plain struct, inheriting nothing — like the TeX one beside
// it.
//
// Every operation returns exactly one element, wrapping several in <mrow>
// where it has to. That invariant is what lets a parent treat each rendered
// child as a single argument: <mfrac> and <msup> take exactly two children,
// and a stray sibling would silently become a third.

namespace mx {
namespace {

// Numeric character references, so the output is plain ASCII.
constexpr std::string_view kMinus = "&#x2212;";
constexpr std::string_view kInvisibleTimes = "&#x2062;";
constexpr std::string_view kFunctionApplication = "&#x2061;";
constexpr std::string_view kTimes = "&#xD7;";
constexpr std::string_view kInfinity = "&#x221E;";

/// Escapes text for XML character data.
std::string escaped(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&apos;";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
}

std::string element(std::string_view tag, std::string_view content) {
    std::string out = "<";
    out += tag;
    out += ">";
    out += content;
    out += "</";
    out += tag;
    out += ">";
    return out;
}

std::string mo(std::string_view op) { return element("mo", op); }
std::string mrow(std::string_view content) { return element("mrow", content); }

/// Names with a glyph of their own: Maxima's constants, and the Greek letters,
/// which Maxima spells out and a typeset formula shows as letters.
std::string_view glyphFor(std::string_view name) {
    static constexpr std::pair<std::string_view, std::string_view> kGlyphs[] = {
        {"%pi", "&#x3C0;"},   {"%e", "e"},          {"%i", "i"},
        {"%gamma", "&#x3B3;"}, {"%phi", "&#x3C6;"}, {"inf", "&#x221E;"},
        {"alpha", "&#x3B1;"}, {"beta", "&#x3B2;"},  {"gamma", "&#x3B3;"},
        {"delta", "&#x3B4;"}, {"epsilon", "&#x3B5;"}, {"theta", "&#x3B8;"},
        {"lambda", "&#x3BB;"}, {"mu", "&#x3BC;"},   {"nu", "&#x3BD;"},
        {"pi", "&#x3C0;"},    {"rho", "&#x3C1;"},   {"sigma", "&#x3C3;"},
        {"tau", "&#x3C4;"},   {"phi", "&#x3C6;"},   {"chi", "&#x3C7;"},
        {"psi", "&#x3C8;"},   {"omega", "&#x3C9;"},
    };
    for (const auto &[known, glyph] : kGlyphs) {
        if (name == known) {
            return glyph;
        }
    }
    return {};
}

std::string renderReal(double value) {
    // Always finite: Expr::real refuses NaN and turns an infinity into the
    // symbol inf or minf, which renders as the infinity sign.
    const bool negative = std::signbit(value);
    char buffer[40];
    const auto [stopped, error]
        = std::to_chars(buffer, buffer + sizeof(buffer), std::fabs(value));
    std::string text = error == std::errc{} ? std::string(buffer, stopped)
                                            : std::string("0");

    std::string body;
    if (const auto marker = text.find('e'); marker != std::string::npos) {
        // 1e+300 is 1 x 10^300, set as such.
        // to_chars pads the exponent (1e-07); a formula shows 10^-7.
        std::string exponent = text.substr(marker + 1);
        const bool negativeExponent = exponent.front() == '-';
        exponent.erase(0, exponent.find_first_not_of("+-0"));
        if (exponent.empty()) {
            exponent = "0";
        }
        if (negativeExponent) {
            exponent.insert(exponent.begin(), '-');
        }
        std::string exponentElement
            = exponent.front() == '-'
                  ? mrow(mo(kMinus) + element("mn", exponent.substr(1)))
                  : element("mn", exponent);
        body = mrow(element("mn", text.substr(0, marker)) + mo(kTimes)
                    + element("msup", element("mn", "10") + exponentElement));
    } else {
        // Keep an inexact whole number recognisable as inexact.
        if (text.find('.') == std::string::npos) {
            text += ".0";
        }
        body = element("mn", text);
    }
    return negative ? mrow(mo(kMinus) + body) : body;
}

struct MathMLRenderer {
    std::string integer(const Integer &value) {
        // The presentation layer hands over magnitudes, with signs carried
        // separately, but nothing is lost by coping with a negative anyway.
        if (value.isNegative()) {
            return mrow(mo(kMinus) + element("mn", (-value).toString()));
        }
        return element("mn", value.toString());
    }

    std::string real(double value) { return renderReal(value); }

    std::string symbol(std::string_view name) {
        if (name == "minf") {
            return mrow(mo(kMinus) + element("mi", kInfinity));
        }
        if (const std::string_view glyph = glyphFor(name); !glyph.empty()) {
            return element("mi", glyph);
        }
        return element("mi", escaped(name));
    }

    /// Maxima source this library never interpreted: shown as text, not
    /// guessed at.
    std::string verbatim(std::string_view source) {
        return element("mtext", escaped(source));
    }

    std::string sum(std::span<const Term<std::string>> terms) {
        std::string content;
        for (std::size_t i = 0; i < terms.size(); ++i) {
            if (i != 0 || terms[i].negated) {
                content += mo(terms[i].negated ? kMinus : "+");
            }
            content += terms[i].value;
        }
        return mrow(content);
    }

    std::string product(std::span<const std::string> factors) {
        std::string content;
        for (std::size_t i = 0; i < factors.size(); ++i) {
            if (i != 0) {
                // An explicit times sign only where juxtaposition would fuse
                // two numerals into one; otherwise the invisible operator,
                // which tells assistive technology this is a product.
                const bool numeral = factors[i].starts_with("<mn>");
                content += mo(numeral ? kTimes : kInvisibleTimes);
            }
            content += factors[i];
        }
        return mrow(content);
    }

    std::string fraction(const std::string &numerator,
                         const std::string &denominator) {
        return element("mfrac", numerator + denominator);
    }

    std::string power(const std::string &base, const std::string &exponent) {
        return element("msup", base + exponent);
    }

    std::string root(const std::string &radicand, unsigned index) {
        if (index == 2) {
            return element("msqrt", radicand);
        }
        return element("mroot",
                       radicand + element("mn", std::to_string(index)));
    }

    std::string call(std::string_view head, std::span<const std::string> args) {
        std::string arguments;
        for (std::size_t i = 0; i < args.size(); ++i) {
            if (i != 0) {
                arguments += mo(",");
            }
            arguments += args[i];
        }
        // The function-application operator is what distinguishes sin(x) from
        // a product of sin and x for anything reading the markup.
        return mrow(element("mi", escaped(head)) + mo(kFunctionApplication)
                    + mrow(mo("(") + mrow(arguments) + mo(")")));
    }

    std::string list(std::span<const std::string> items) {
        std::string content;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i != 0) {
                content += mo(",");
            }
            content += items[i];
        }
        return mrow(mo("[") + mrow(content) + mo("]"));
    }

    std::string relation(RelOp op, const std::string &lhs,
                         const std::string &rhs) {
        std::string_view symbol;
        switch (op) {
        case RelOp::Equal:
            symbol = "=";
            break;
        case RelOp::NotEqual:
            symbol = "&#x2260;";
            break;
        case RelOp::Less:
            symbol = "&lt;";
            break;
        case RelOp::LessEqual:
            symbol = "&#x2264;";
            break;
        case RelOp::Greater:
            symbol = "&gt;";
            break;
        case RelOp::GreaterEqual:
            symbol = "&#x2265;";
            break;
        }
        return mrow(lhs + mo(symbol) + rhs);
    }

    /// Stretchy by default: a browser grows an <mo> bracket to its row.
    std::string group(const std::string &inner) {
        return mrow(mo("(") + inner + mo(")"));
    }

    std::string negate(const std::string &inner) {
        return mrow(mo(kMinus) + inner);
    }

    // --- where the layout delimits itself -----------------------------------

    Strength strengthOf(Construct construct) {
        // A fraction bar and a radical sign both show plainly where they end.
        if (construct == Construct::Fraction || construct == Construct::Root) {
            return Strength::Atom;
        }
        return defaultStrength(construct);
    }

    Strength contextFor(Slot slot) {
        switch (slot) {
        case Slot::Numerator:
        case Slot::Denominator:
        case Slot::Exponent:
        case Slot::Radicand:
            // Above or below a bar, raised, or under a radical. The base of a
            // power is *not* here: x+1 with a raised 2 beside it would not say
            // whether the 2 applies to the whole sum.
            return Strength::Loosest;
        default:
            return defaultContext(slot);
        }
    }
};

} // namespace

std::string toMathML(const Expr &expr) {
    return R"(<math xmlns="http://www.w3.org/1998/Math/MathML">)"
           + render(expr, MathMLRenderer{}) + "</math>";
}

} // namespace mx
