#include <mx/expr.hpp>
#include <mx/render.hpp>

#include <charconv>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

// Expr::str(), as a renderer like any other.
//
// This file is the proof that mx/render.hpp is sufficient: the library's own
// printer is an ordinary user of it, with no privileged access, and it fits in
// a plain struct that inherits nothing. Everything that used to be here —
// precedence, when to parenthesise, hoisting a minus out of a term, rebuilding
// a product to drop a leading `-1` — has moved into the shared presentation
// layer, where every renderer gets it.
//
// The output is Maxima-compatible infix, for people: diagnostics, test
// failures, and anyone who wants to paste a result into a Maxima session. It is
// *not* how expressions reach Maxima — that is wire/to_maxima.cpp, which sends
// the internal s-expression — so nothing about the protocol depends on what
// this emits.

namespace mx {
namespace {

std::string renderReal(double value) {
    char buffer[40];
    const auto [stopped, error]
        = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (error != std::errc{}) {
        return "0.0";
    }
    std::string text(buffer, stopped);
    // Keep it recognisable as inexact rather than reading back as an integer.
    if (text.find_first_of(".eni") == std::string::npos) {
        text += ".0";
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

/// Maxima-compatible infix.
///
/// Note what is *absent*: no `root()`. The default synthesises one as
/// `base^(1/n)`, which is what both Maxima and Expr::parse understand — there
/// is no sqrt node in either, so emitting `sqrt(x)` here would print something
/// that no longer reads back as the same expression. A renderer declining an
/// optional operation is the mechanism working, not a gap.
struct InfixRenderer {
    std::string integer(const Integer &value) { return value.toString(); }

    std::string real(double value) { return renderReal(value); }

    std::string symbol(std::string_view name) { return std::string(name); }

    /// Already Maxima source; reproduced verbatim.
    std::string verbatim(std::string_view source) { return std::string(source); }

    std::string sum(std::span<const Term<std::string>> terms) {
        std::string out;
        for (std::size_t i = 0; i < terms.size(); ++i) {
            if (i == 0) {
                if (terms[i].negated) {
                    out += "-";
                }
            } else {
                out += terms[i].negated ? " - " : " + ";
            }
            out += terms[i].value;
        }
        return out;
    }

    std::string product(std::span<const std::string> factors) {
        return joined(factors, "*");
    }

    std::string fraction(const std::string &numerator,
                         const std::string &denominator) {
        return numerator + "/" + denominator;
    }

    std::string power(const std::string &base, const std::string &exponent) {
        return base + "^" + exponent;
    }

    std::string call(std::string_view head, std::span<const std::string> args) {
        return std::string(head) + "(" + joined(args, ", ") + ")";
    }

    /// Maxima has no textual `list(...)` constructor; brackets are the only
    /// spelling it has.
    std::string list(std::span<const std::string> items) {
        return "[" + joined(items, ", ") + "]";
    }

    std::string relation(RelOp op, const std::string &lhs,
                         const std::string &rhs) {
        return lhs + " " + std::string(symbolFor(op)) + " " + rhs;
    }

    std::string group(const std::string &inner) { return "(" + inner + ")"; }

    std::string negate(const std::string &inner) { return "-" + inner; }
};

} // namespace

std::string Expr::str() const {
    return render(*this, InfixRenderer{});
}

} // namespace mx
