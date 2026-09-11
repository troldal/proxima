#include "wire/from_maxima.hpp"

#include <mx/errors.hpp>

#include <bit>
#include <cctype>
#include <cstdint>
#include <utility>
#include <string>
#include <vector>

namespace mx::detail {
namespace {

bool isUpper(unsigned char c) {
    return std::isupper(c) != 0;
}
bool isLower(unsigned char c) {
    return std::islower(c) != 0;
}

std::string toLower(std::string_view text) {
    std::string out(text);
    for (char &c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string toUpper(std::string_view text) {
    std::string out(text);
    for (char &c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

/// Renders a Maxima string literal, escaping what Maxima's reader would
/// otherwise interpret.
std::string quoteString(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

/// Heads whose Maxima display name is not simply the head with its sigil
/// removed and its case restored.
///
/// Two groups. Nouns whose display name differs outright: `'diff(f(x), x)` has
/// internal head %DERIVATIVE, and printing `derivative(f(x), x)` would be an
/// undefined function rather than a derivative — the leading quote keeps it a
/// noun instead of asking Maxima to evaluate it. And Maxima's `M`-prefixed
/// operator heads, which carry no sigil at all, so the general rule would turn
/// MABS into `mabs`.
///
/// Only heads with a `name(args...)` spelling belong here. Maxima's infix
/// operators (MAND, MOR, MNCTIMES) have none, and are handled by the note at
/// the fallback in fromMaxima.
constexpr std::pair<std::string_view, std::string_view> kDisplayNames[] = {
    {"%DERIVATIVE", "'diff"},
    {"MABS", "abs"},
    {"MFACTORIAL", "factorial"},
    {"MNOT", "not"},
};

/// The head of an application, as Maxima would spell it in source.
///
/// `%SIN` and `$F` are the two ordinary spellings — `%` for Maxima's own
/// operators and nouns, `$` for user-defined names — and neither sigil is part
/// of the name. Note that a *symbol* keeps any `%`: the constant `%pi` really
/// is called that, and arrives as `$%PI`.
std::string decodeHead(std::string_view raw) {
    for (const auto &[internal, display] : kDisplayNames) {
        if (raw == internal) {
            return std::string(display);
        }
    }
    if (!raw.empty() && (raw.front() == '%' || raw.front() == '$')) {
        raw.remove_prefix(1);
    }
    return decodeMaximaName(raw);
}

Expr mapInteger(const SExpr &form) {
    if (const auto value = form.asInt64()) {
        return Expr::integer(*value);
    }
    // Beyond mx::Integer. Kept exactly, as text, rather than wrapped — the
    // fallback mx::Integer's documentation promises.
    return Expr::opaque(form.digits());
}

Expr mapRational(const SExpr &form) {
    const auto numerator = form.at(1).asInt64();
    const auto denominator = form.at(2).asInt64();
    if (numerator && denominator) {
        return Expr::rational(*numerator, *denominator);
    }
    // A rational whose parts do not fit. Parenthesised because Opaque is
    // treated as an atom by the printer and this content is not one.
    return Expr::opaque("(" + form.at(1).digits() + "/" + form.at(2).digits()
                        + ")");
}

/// A Maxima bigfloat is mantissa * 2^(exponent - bits(mantissa)).
///
/// There is no arbitrary-precision float here to map it onto, so it becomes the
/// exact rational it equals, as Opaque text. The *value* is preserved exactly —
/// nothing is rounded — but its bigfloat-ness is not: sending it back gives an
/// exact rational rather than a bfloat. That is the honest trade for not
/// carrying a multiprecision float type.
Expr mapBigfloat(const SExpr &form) {
    const auto mantissa = form.at(1).asInt64();
    const auto exponent = form.at(2).asInt64();
    if (!mantissa || !exponent) {
        // Mantissa past int64 means an fpprec far above the default. Kept
        // structurally; this one does not round-trip.
        return Expr::function("bigfloat",
                              {fromMaxima(form.at(1)), fromMaxima(form.at(2))});
    }

    const auto magnitude = static_cast<std::uint64_t>(
        *mantissa < 0 ? -(*mantissa + 1) + 1 : *mantissa);
    const auto bits = static_cast<std::int64_t>(std::bit_width(magnitude));
    const std::int64_t shift = *exponent - bits;

    const std::string digits = std::to_string(*mantissa);
    if (shift >= 0) {
        return Expr::opaque("(" + digits + "*2^" + std::to_string(shift) + ")");
    }
    return Expr::opaque("(" + digits + "/2^" + std::to_string(-shift) + ")");
}

std::vector<Expr> mapArguments(const SExpr &form) {
    std::vector<Expr> args;
    args.reserve(form.size() - 1);
    for (std::size_t i = 1; i < form.size(); ++i) {
        args.push_back(fromMaxima(form.at(i)));
    }
    return args;
}

} // namespace

std::string decodeMaximaName(std::string_view raw) {
    bool sawUpper = false;
    bool sawLower = false;
    for (const char c : raw) {
        const auto uc = static_cast<unsigned char>(c);
        sawUpper = sawUpper || isUpper(uc);
        sawLower = sawLower || isLower(uc);
    }

    // Uniformly cased names were inverted on the way in; mixed-case names
    // reached Maxima through bar quoting and were left alone.
    if (sawUpper && !sawLower) {
        return toLower(raw);
    }
    if (sawLower && !sawUpper) {
        return toUpper(raw);
    }
    return std::string(raw);
}

Expr fromMaxima(const SExpr &form) {
    switch (form.kind()) {
    case SExpr::Kind::Integer:
        return mapInteger(form);

    case SExpr::Kind::Real:
        return Expr::real(form.realValue());

    case SExpr::Kind::String:
        // Maxima strings have no typed node; they become source text, which is
        // exactly what Opaque is for.
        return Expr::opaque(quoteString(form.stringValue()));

    case SExpr::Kind::Symbol: {
        const std::string &name = form.symbolName();
        // Maxima's booleans are the Lisp ones, not $TRUE/$FALSE.
        if (name == "T") {
            return Expr::symbol("true");
        }
        if (name == "NIL") {
            return Expr::symbol("false");
        }
        std::string_view bare = name;
        if (!bare.empty() && bare.front() == '$') {
            bare.remove_prefix(1);
        }
        return Expr::symbol(decodeMaximaName(bare));
    }

    case SExpr::Kind::List:
        break;
    }

    if (form.empty()) {
        throw ParseError("Maxima term has no head");
    }

    // The head is normally a list of the operator followed by simplification
    // flags — and there can be more than SIMP, as in (MEXPT SIMP RATSIMP) — so
    // only its first element names the operator.
    const SExpr &head = form.at(0);
    const SExpr &operatorName = head.isList() ? head.at(0) : head;
    if (!operatorName.isSymbol()) {
        throw ParseError("Maxima term head is not a symbol");
    }
    const std::string &raw = operatorName.symbolName();

    if (raw == "MPLUS") {
        return Expr::add(mapArguments(form));
    }
    if (raw == "MTIMES") {
        return Expr::mul(mapArguments(form));
    }
    if (raw == "MEXPT") {
        return Expr::pow(fromMaxima(form.at(1)), fromMaxima(form.at(2)));
    }
    if (raw == "RAT") {
        return mapRational(form);
    }
    if (raw == "BIGFLOAT") {
        return mapBigfloat(form);
    }

    if (raw == "MEQUAL") {
        return Expr::relation(RelOp::Equal, fromMaxima(form.at(1)),
                              fromMaxima(form.at(2)));
    }
    if (raw == "MNOTEQUAL") {
        return Expr::relation(RelOp::NotEqual, fromMaxima(form.at(1)),
                              fromMaxima(form.at(2)));
    }
    if (raw == "MLESSP") {
        return Expr::relation(RelOp::Less, fromMaxima(form.at(1)),
                              fromMaxima(form.at(2)));
    }
    if (raw == "MLEQP") {
        return Expr::relation(RelOp::LessEqual, fromMaxima(form.at(1)),
                              fromMaxima(form.at(2)));
    }
    if (raw == "MGREATERP") {
        return Expr::relation(RelOp::Greater, fromMaxima(form.at(1)),
                              fromMaxima(form.at(2)));
    }
    if (raw == "MGEQP") {
        return Expr::relation(RelOp::GreaterEqual, fromMaxima(form.at(1)),
                              fromMaxima(form.at(2)));
    }

    // Operators whose structure maps onto a typed node rather than a name.
    // Maxima's simplifier normally rewrites these away — negation becomes
    // MTIMES with -1, division becomes MEXPT with -1 — but unsimplified input
    // can still carry them.
    if (raw == "MMINUS" && form.size() == 2) {
        return -fromMaxima(form.at(1));
    }
    if (raw == "MQUOTIENT" && form.size() == 3) {
        return fromMaxima(form.at(1)) / fromMaxima(form.at(2));
    }

    if (raw == "MLIST") {
        // Maxima has no textual `list(...)` constructor — `[a, b]` is the only
        // spelling — so the printer knows this one head by name.
        return Expr::function("list", mapArguments(form));
    }

    // Everything else is an uninterpreted application. This is the escape hatch
    // that keeps the typed node set small: sin, bessel_j, matrix, a user's f,
    // and Maxima operators with no typed node here (MABS, MFACTORIAL) all land
    // here and print as `head(args...)`.
    //
    // A head whose Maxima syntax is not `name(args...)` — an infix operator
    // like MAND, say — will map faithfully but print as a function call that
    // Maxima would not read back the same way. Maxima's simplifier does not
    // produce those in the expressions this library asks for; if one turns up,
    // it needs an entry beside 'diff in displayNameFor or a typed node here.
    return Expr::function(decodeHead(raw), mapArguments(form));
}

} // namespace mx::detail
