#include "wire/to_maxima.hpp"

#include "wire/from_maxima.hpp"

#include <proxima/errors.hpp>

#include <cctype>
#include <charconv>
#include <cmath>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace proxima::detail {
namespace {

/// Heads whose Maxima internal spelling is not `$NAME`.
///
/// The reverse of from_maxima's display-name table, plus `list`: Maxima's
/// operator heads carry no sigil (`MABS`, not `$ABS`), a derivative noun is
/// `%DERIVATIVE`, and a list is `MLIST`. Everything else is a `$`-prefixed
/// verb, which is exactly what Maxima's own parser produces for `sin(x)`,
/// and which evaluation turns into the `%SIN` noun.
constexpr std::pair<std::string_view, std::string_view> kInternalHeads[] = {
    {"'diff", "%DERIVATIVE"}, {"abs", "MABS"}, {"factorial", "MFACTORIAL"},
    {"not", "MNOT"},          {"list", "MLIST"},
};

constexpr std::string_view relation_head(RelOp op) {
    switch (op) {
    case RelOp::Equal:
        return "MEQUAL";
    case RelOp::NotEqual:
        return "MNOTEQUAL";
    case RelOp::Less:
        return "MLESSP";
    case RelOp::LessEqual:
        return "MLEQP";
    case RelOp::Greater:
        return "MGREATERP";
    case RelOp::GreaterEqual:
        return "MGEQP";
    }
    return "MEQUAL";
}

/// True when the Lisp reader would hand `token` back unchanged: no lowercase
/// (the reader upcases), nothing it treats as syntax. The token always begins
/// with a sigil, so it can never read as a number.
bool reads_plainly(std::string_view token) {
    for (const char c : token) {
        const auto uc = static_cast<unsigned char>(c);
        if (!(std::isupper(uc) != 0 || std::isdigit(uc) != 0 || c == '_'
              || c == '%' || c == '$')) {
            return false;
        }
    }
    return !token.empty();
}

/// `sigil` + the name, case-inverted as Maxima stores it, bar-quoted if the
/// reader would otherwise alter it.
std::string encode_with_sigil(char sigil, std::string_view name) {
    // decode_maxima_name is its own inverse: the same inversion encodes.
    std::string token(1, sigil);
    token += decode_maxima_name(name);
    if (reads_plainly(token)) {
        return token;
    }
    std::string quoted = "|";
    for (const char c : token) {
        if (c == '|' || c == '\\') {
            quoted.push_back('\\');
        }
        quoted.push_back(c);
    }
    quoted.push_back('|');
    return quoted;
}

std::string encode_real(double value) {
    // Always finite: Expr::real refuses NaN and turns an infinity into the
    // symbol inf or minf, which is encoded as any symbol is.
    char buffer[40];
    const auto [stopped, error]
        = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (error != std::errc{}) {
        throw Error("cannot render a real for Maxima");
    }
    std::string text(buffer, stopped);
    // The Lisp reader needs a `d` marker to make a double regardless of
    // *read-default-float-format*; `e` would give whatever that is set to.
    if (const auto e = text.find('e'); e != std::string::npos) {
        text[e] = 'd';
    } else {
        if (text.find('.') == std::string::npos) {
            text += ".0";
        }
        text += "d0";
    }
    return text;
}

/// True when `text` is exactly one string literal: an opening quote, a
/// closing quote, and no unescaped quote between them.
bool is_string_literal(std::string_view text) {
    if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
        return false;
    }
    for (std::size_t i = 1; i + 1 < text.size(); ++i) {
        if (text[i] == '\\') {
            ++i; // Whatever follows is escaped, including a quote.
        } else if (text[i] == '"') {
            return false;
        }
    }
    // The closing quote must not itself be escaped.
    std::size_t backslashes = 0;
    for (std::size_t i = text.size() - 1; i > 1 && text[i - 1] == '\\'; --i) {
        ++backslashes;
    }
    return backslashes % 2 == 0;
}

void render(const Expr &expr, std::string &out);

void render_application(std::string_view head, std::span<const Expr> args,
                       std::string &out) {
    out += "((";
    out += head;
    out += ')';
    for (const Expr &arg : args) {
        out.push_back(' ');
        render(arg, out);
    }
    out.push_back(')');
}

/// The same for the two-operand kinds, which match hands over as a pair of
/// references rather than a span.
void render_pair(std::string_view head, const Expr &first, const Expr &second,
                 std::string &out) {
    out += "((";
    out += head;
    out += ") ";
    render(first, out);
    out.push_back(' ');
    render(second, out);
    out.push_back(')');
}

std::string_view function_head(const std::string &name, std::string &storage) {
    for (const auto &[display, internal] : kInternalHeads) {
        if (name == display) {
            return internal;
        }
    }
    // A quoted head is a noun: 'integrate is %INTEGRATE. Anything else is a
    // verb, which Maxima will evaluate — and, for its own functions, turn
    // into the noun itself.
    if (!name.empty() && name.front() == '\'') {
        storage = encode_with_sigil('%', std::string_view(name).substr(1));
    } else {
        storage = encode_with_sigil('$', name);
    }
    return storage;
}

void render(const Expr &expr, std::string &out) {
    // A match rather than a switch on kind(): the compiler checks every kind
    // is written, and the views read the node in place, where the accessors
    // copied each Integer out of it.
    expr.match(
        [&](const node::Integer &n) { out += n.value.to_string(); },
        [&](const node::Rational &q) {
            // MQUOTIENT rather than RAT: RAT is what the simplifier *produces*,
            // and handing it an unsimplified one is asking for trouble. A
            // quotient is what Maxima's own parser emits for `1/3`, and it
            // simplifies to the RAT form on arrival.
            out += "((MQUOTIENT) " + q.numerator.to_string() + " "
                   + q.denominator.to_string() + ")";
        },
        [&](const node::Real &r) { out += encode_real(r.value); },
        [&](const node::Symbol &s) {
            // Maxima's booleans are the Lisp ones, as from_maxima already knows.
            if (s.name == "true") {
                out += 'T';
            } else if (s.name == "false") {
                out += "NIL";
            } else {
                out += encode_maxima_name(s.name);
            }
        },
        [&](const node::Sum &s) { render_application("MPLUS", s.terms, out); },
        [&](const node::Product &p) { render_application("MTIMES", p.factors, out); },
        [&](const node::Power &p) { render_pair("MEXPT", p.base, p.exponent, out); },
        [&](const node::Call &c) {
            std::string storage;
            render_application(function_head(c.head, storage), c.args, out);
        },
        [&](const node::Relation &r) { render_pair(relation_head(r.op), r.lhs, r.rhs, out); },
        [&](const node::Opaque &o) {
            // A string is a string: from_maxima wraps Maxima strings as quoted
            // Opaque text, and this sends them back as Lisp strings, which use
            // the same escapes. Anything else is Maxima source this library
            // never interpreted, so Maxima parses it — inside errcatch, where a
            // read error is an ordinary failure rather than a silence.
            if (is_string_literal(o.text)) {
                out += o.text;
            } else {
                out += "(($EVAL_STRING) " + string_literal(o.text) + ")";
            }
        });
}

} // namespace

std::string encode_maxima_name(std::string_view name) {
    return encode_with_sigil('$', name);
}

std::string string_literal(std::string_view text) {
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

std::string to_maxima(const Expr &expr) {
    std::string out;
    render(expr, out);
    return out;
}

} // namespace proxima::detail
