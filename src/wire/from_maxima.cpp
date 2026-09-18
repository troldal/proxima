#include "wire/from_maxima.hpp"

#include <proxima/errors.hpp>

#include <bit>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace proxima::detail {
namespace {

bool is_upper(unsigned char c) {
    return std::isupper(c) != 0;
}
bool is_lower(unsigned char c) {
    return std::islower(c) != 0;
}

std::string to_lower(std::string_view text) {
    std::string out(text);
    for (char &c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string to_upper(std::string_view text) {
    std::string out(text);
    for (char &c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

/// Renders a Maxima string literal, escaping what Maxima's reader would
/// otherwise interpret.
std::string quote_string(std::string_view text) {
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
/// the fallback in from_maxima.
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
std::string decode_head(std::string_view raw) {
    for (const auto &[internal, display] : kDisplayNames) {
        if (raw == internal) {
            return std::string(display);
        }
    }
    if (!raw.empty() && (raw.front() == '%' || raw.front() == '$')) {
        raw.remove_prefix(1);
    }
    return decode_maxima_name(raw);
}

/// The value of an Integer atom, of any size: proxima::Integer is unbounded, so a
/// factorial arrives as a number rather than as a blob of text.
Integer integer_of(const SExpr &form) {
    // The reader makes an Integer only of an optional sign and digits, so this
    // parses. It throws rather than falling back to Opaque text, which is what
    // it once did, so that a broken reader could never pass for an answer.
    auto value = Integer::parse(form.digits());
    if (!value) {
        throw ParseError("unreadable integer '" + form.digits()
                         + "' in a Maxima reply");
    }
    return std::move(*value);
}

Expr map_integer(const SExpr &form) {
    return Expr::integer(integer_of(form));
}

/// Maxima's `(RAT numerator denominator)`: two integers, the denominator not
/// zero. Anything else is not a term Maxima produces, and is refused rather
/// than kept as Opaque text such as "(1/0)" that would look like an answer.
Expr map_rational(const SExpr &form) {
    if (form.size() != 3 || !form.at(1).is_integer() || !form.at(2).is_integer()) {
        throw ParseError("malformed rational " + form.to_string()
                         + " in a Maxima reply");
    }
    Integer denominator = integer_of(form.at(2));
    if (denominator.is_zero()) {
        throw ParseError("rational with a zero denominator " + form.to_string()
                         + " in a Maxima reply");
    }
    return Expr::rational(integer_of(form.at(1)), std::move(denominator));
}

/// A Maxima bigfloat is mantissa * 2^(exponent - bits(mantissa)).
///
/// There is no arbitrary-precision float here to map it onto, so it becomes the
/// exact rational it equals, as Opaque text. The *value* is preserved exactly —
/// nothing is rounded — but its bigfloat-ness is not: sending it back gives an
/// exact rational rather than a bfloat. That is the honest trade for not
/// carrying a multiprecision float type.
Expr map_bigfloat(const SExpr &form) {
    const auto mantissa = form.at(1).as_int64();
    const auto exponent = form.at(2).as_int64();
    if (!mantissa || !exponent) {
        // Mantissa past int64 means an fpprec far above the default. Kept
        // structurally; this one does not round-trip.
        return Expr::function("bigfloat",
                              {from_maxima(form.at(1)), from_maxima(form.at(2))});
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

std::vector<Expr> map_arguments(const SExpr &form) {
    std::vector<Expr> args;
    args.reserve(form.size() - 1);
    for (std::size_t i = 1; i < form.size(); ++i) {
        args.push_back(from_maxima(form.at(i)));
    }
    return args;
}

} // namespace

std::string decode_maxima_name(std::string_view raw) {
    bool saw_upper = false;
    bool saw_lower = false;
    for (const char c : raw) {
        const auto uc = static_cast<unsigned char>(c);
        saw_upper = saw_upper || is_upper(uc);
        saw_lower = saw_lower || is_lower(uc);
    }

    // Uniformly cased names were inverted on the way in; mixed-case names
    // reached Maxima through bar quoting and were left alone.
    if (saw_upper && !saw_lower) {
        return to_lower(raw);
    }
    if (saw_lower && !saw_upper) {
        return to_upper(raw);
    }
    return std::string(raw);
}

Expr from_maxima(const SExpr &form) {
    switch (form.kind()) {
    case SExpr::Kind::Integer:
        return map_integer(form);

    case SExpr::Kind::Real:
        return Expr::real(form.real_value());

    case SExpr::Kind::String:
        // Maxima strings have no typed node; they become source text, which is
        // exactly what Opaque is for.
        return Expr::opaque(quote_string(form.string_value()));

    case SExpr::Kind::Symbol: {
        const std::string &name = form.symbol_name();
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
        return Expr::symbol(decode_maxima_name(bare));
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
    const SExpr &operator_name = head.is_list() ? head.at(0) : head;
    if (!operator_name.is_symbol()) {
        throw ParseError("Maxima term head is not a symbol");
    }
    const std::string &raw = operator_name.symbol_name();

    if (raw == "MPLUS") {
        return Expr::add(map_arguments(form));
    }
    if (raw == "MTIMES") {
        return Expr::mul(map_arguments(form));
    }
    if (raw == "MEXPT") {
        return Expr::pow(from_maxima(form.at(1)), from_maxima(form.at(2)));
    }
    if (raw == "RAT") {
        return map_rational(form);
    }
    if (raw == "BIGFLOAT") {
        return map_bigfloat(form);
    }

    if (raw == "MEQUAL") {
        return Expr::relation(RelOp::Equal, from_maxima(form.at(1)),
                              from_maxima(form.at(2)));
    }
    if (raw == "MNOTEQUAL") {
        return Expr::relation(RelOp::NotEqual, from_maxima(form.at(1)),
                              from_maxima(form.at(2)));
    }
    if (raw == "MLESSP") {
        return Expr::relation(RelOp::Less, from_maxima(form.at(1)),
                              from_maxima(form.at(2)));
    }
    if (raw == "MLEQP") {
        return Expr::relation(RelOp::LessEqual, from_maxima(form.at(1)),
                              from_maxima(form.at(2)));
    }
    if (raw == "MGREATERP") {
        return Expr::relation(RelOp::Greater, from_maxima(form.at(1)),
                              from_maxima(form.at(2)));
    }
    if (raw == "MGEQP") {
        return Expr::relation(RelOp::GreaterEqual, from_maxima(form.at(1)),
                              from_maxima(form.at(2)));
    }

    // Operators whose structure maps onto a typed node rather than a name.
    // Maxima's simplifier normally rewrites these away — negation becomes
    // MTIMES with -1, division becomes MEXPT with -1 — but unsimplified input
    // can still carry them.
    if (raw == "MMINUS" && form.size() == 2) {
        return -from_maxima(form.at(1));
    }
    if (raw == "MQUOTIENT" && form.size() == 3) {
        return from_maxima(form.at(1)) / from_maxima(form.at(2));
    }

    if (raw == "MLIST") {
        // Maxima has no textual `list(...)` constructor — `[a, b]` is the only
        // spelling — so the printer knows this one head by name.
        return Expr::function("list", map_arguments(form));
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
    // it needs an entry beside 'diff in display_name_for or a typed node here.
    return Expr::function(decode_head(raw), map_arguments(form));
}

} // namespace proxima::detail
