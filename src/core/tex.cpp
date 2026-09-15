#include <proxima/tex.hpp>

#include <proxima/render.hpp>

#include <charconv>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

// A LaTeX renderer, written the way a user of this library would write one: a
// plain struct with the operations proxima/render.hpp asks for, inheriting nothing
// and knowing nothing about the library's internals.
//
// Worth reading beside src/core/printer.cpp. The two produce entirely
// different notation from the same walk, and the only structural differences
// between them are the two queries at the bottom — `\frac{}{}` and `\sqrt{}`
// carry their own braces, so TeX needs brackets in places infix text does not,
// and does not need them in places infix text does.

namespace proxima {
namespace {

/// Characters TeX reads as markup rather than as text.
std::string escaped(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '_':
        case '#':
        case '%':
        case '&':
        case '$':
        case '{':
        case '}':
            // The one that matters in practice: a Maxima name like `bessel_j`
            // is a subscript to TeX, and emitting it raw produces a document
            // that does not compile.
            out.push_back('\\');
            out.push_back(c);
            break;
        case '\\':
            out += "\\backslash ";
            break;
        case '^':
            out += "\\hat{}";
            break;
        case '~':
            out += "\\sim ";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
}

/// Names with a symbol of their own. Maxima's constants, and the Greek
/// letters, which are spelled out in Maxima and are expected to be letters in
/// a typeset formula.
std::string_view symbolMacro(std::string_view name) {
    static constexpr std::pair<std::string_view, std::string_view> kMacros[] = {
        {"%pi", "\\pi"},       {"%e", "e"},          {"%i", "i"},
        {"%gamma", "\\gamma"}, {"%phi", "\\varphi"}, {"inf", "\\infty"},
        {"minf", "-\\infty"},  {"und", "\\mathrm{und}"},
        {"true", "\\mathrm{true}"}, {"false", "\\mathrm{false}"},
        {"alpha", "\\alpha"},  {"beta", "\\beta"},   {"gamma", "\\gamma"},
        {"delta", "\\delta"},  {"epsilon", "\\epsilon"}, {"theta", "\\theta"},
        {"lambda", "\\lambda"}, {"mu", "\\mu"},      {"nu", "\\nu"},
        {"rho", "\\rho"},      {"sigma", "\\sigma"}, {"tau", "\\tau"},
        {"phi", "\\phi"},      {"chi", "\\chi"},     {"psi", "\\psi"},
        {"omega", "\\omega"},  {"pi", "\\pi"},
    };
    for (const auto &[name_, macro] : kMacros) {
        if (name == name_) {
            return macro;
        }
    }
    return {};
}

/// Functions TeX sets upright and spaces as operators.
bool hasOperatorMacro(std::string_view name) {
    static constexpr std::string_view kNames[] = {
        "sin",  "cos",  "tan",  "sec",  "csc",  "cot",  "sinh", "cosh",
        "tanh", "log",  "ln",   "exp",  "min",  "max",  "gcd",  "det",
        "dim",  "deg",  "arg",  "lim",  "inf",  "sup",  "arcsin", "arccos",
        "arctan",
    };
    for (const std::string_view known : kNames) {
        if (name == known) {
            return true;
        }
    }
    return false;
}

std::string renderReal(double value) {
    char buffer[40];
    const auto [stopped, error]
        = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (error != std::errc{}) {
        return "0";
    }
    std::string text(buffer, stopped);

    // to_chars writes 1e+300; a formula wants 1 \times 10^{300}.
    if (const auto marker = text.find('e'); marker != std::string::npos) {
        std::string mantissa = text.substr(0, marker);
        // to_chars pads the exponent (1e-07); a formula shows 10^{-7}.
        std::string exponent = text.substr(marker + 1);
        const bool negativeExponent = exponent.front() == '-';
        exponent.erase(0, exponent.find_first_not_of("+-0"));
        if (exponent.empty()) {
            exponent = "0";
        }
        if (negativeExponent) {
            exponent.insert(exponent.begin(), '-');
        }
        return mantissa + " \\times 10^{" + exponent + "}";
    }
    return text;
}

std::string joined(std::span<const std::string> parts,
                   std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) {
            out += separator;
        }
        out += parts[i];
    }
    return out;
}

struct TeXRenderer {
    std::string integer(const Integer &value) { return value.toString(); }

    std::string real(double value) { return renderReal(value); }

    std::string symbol(std::string_view name) {
        if (const std::string_view macro = symbolMacro(name); !macro.empty()) {
            return std::string(macro);
        }
        // A single letter is already italic in maths mode; a longer name would
        // otherwise be set as a product of its letters.
        if (name.size() == 1) {
            return escaped(name);
        }
        return "\\mathit{" + escaped(name) + "}";
    }

    /// Maxima source this library never interpreted, so there is nothing to
    /// typeset. Set as text rather than silently emitting it as markup.
    std::string verbatim(std::string_view source) {
        return "\\text{" + escaped(source) + "}";
    }

    std::string sum(std::span<const Term<std::string>> terms) {
        std::string out;
        for (std::size_t i = 0; i < terms.size(); ++i) {
            if (i == 0) {
                if (terms[i].negated) {
                    out += '-';
                }
            } else {
                out += terms[i].negated ? " - " : " + ";
            }
            out += terms[i].value;
        }
        return out;
    }

    std::string product(std::span<const std::string> factors) {
        // Juxtaposition is the convention — `2x`, not `2 \cdot x` — but two
        // adjacent numerals would read as one, so a dot goes in when the next
        // factor starts with a digit. A heuristic on the rendered text, which
        // is all a fold sees; it is right for everything Maxima produces,
        // where canonical order puts the single numeric factor first.
        std::string out;
        for (std::size_t i = 0; i < factors.size(); ++i) {
            if (i != 0) {
                const bool startsWithDigit
                    = !factors[i].empty() && factors[i].front() >= '0'
                      && factors[i].front() <= '9';
                out += startsWithDigit ? " \\cdot " : " ";
            }
            out += factors[i];
        }
        return out;
    }

    std::string fraction(const std::string &numerator,
                         const std::string &denominator) {
        return "\\frac{" + numerator + "}{" + denominator + "}";
    }

    std::string power(const std::string &base, const std::string &exponent) {
        return base + "^{" + exponent + "}";
    }

    std::string root(const std::string &radicand, unsigned index) {
        if (index == 2) {
            return "\\sqrt{" + radicand + "}";
        }
        return "\\sqrt[" + std::to_string(index) + "]{" + radicand + "}";
    }

    std::string call(std::string_view head, std::span<const std::string> args) {
        std::string name = hasOperatorMacro(head)
                               ? "\\" + std::string(head)
                               : "\\operatorname{" + escaped(head) + "}";
        return name + "\\left(" + joined(args, ", ") + "\\right)";
    }

    std::string list(std::span<const std::string> items) {
        return "\\left[" + joined(items, ", ") + "\\right]";
    }

    std::string relation(RelOp op, const std::string &lhs,
                         const std::string &rhs) {
        std::string_view macro;
        switch (op) {
        case RelOp::Equal:
            macro = "=";
            break;
        case RelOp::NotEqual:
            macro = "\\neq";
            break;
        case RelOp::Less:
            macro = "<";
            break;
        case RelOp::LessEqual:
            macro = "\\leq";
            break;
        case RelOp::Greater:
            macro = ">";
            break;
        case RelOp::GreaterEqual:
            macro = "\\geq";
            break;
        }
        return lhs + " " + std::string(macro) + " " + rhs;
    }

    std::string group(const std::string &inner) {
        // \left and \right so the brackets grow with what they contain.
        return "\\left(" + inner + "\\right)";
    }

    std::string negate(const std::string &inner) { return "-" + inner; }

    // --- the two queries that make this TeX rather than infix text ---------

    Strength strengthOf(Construct construct) {
        // A fraction is braced, so it needs no brackets of its own however it
        // is used: `\frac{a}{b}x` is unambiguous where `a/b*x` would not be.
        if (construct == Construct::Fraction) {
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
            // All brace-delimited, so nothing inside them needs bracketing:
            // `\frac{1+x}{x-1}`, not `\frac{\left(1+x\right)}{...}`.
            return Strength::Loosest;
        default:
            return defaultContext(slot);
        }
    }
};

} // namespace

std::string toTeX(const Expr &expr) {
    return render(expr, TeXRenderer{});
}

} // namespace proxima
