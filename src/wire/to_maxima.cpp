#include "wire/to_maxima.hpp"

#include "wire/from_maxima.hpp"

#include <mx/errors.hpp>

#include <cctype>
#include <charconv>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>

namespace mx::detail {
namespace {

/// Heads whose Maxima internal spelling is not `$NAME`.
///
/// The reverse of fromMaxima's display-name table, plus `list`: Maxima's
/// operator heads carry no sigil (`MABS`, not `$ABS`), a derivative noun is
/// `%DERIVATIVE`, and a list is `MLIST`. Everything else is a `$`-prefixed
/// verb, which is exactly what Maxima's own parser produces for `sin(x)`,
/// and which evaluation turns into the `%SIN` noun.
constexpr std::pair<std::string_view, std::string_view> kInternalHeads[] = {
    {"'diff", "%DERIVATIVE"}, {"abs", "MABS"}, {"factorial", "MFACTORIAL"},
    {"not", "MNOT"},          {"list", "MLIST"},
};

constexpr std::string_view relationHead(RelOp op) {
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
bool readsPlainly(std::string_view token) {
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
std::string encodeWithSigil(char sigil, std::string_view name) {
    // decodeMaximaName is its own inverse: the same inversion encodes.
    std::string token(1, sigil);
    token += decodeMaximaName(name);
    if (readsPlainly(token)) {
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

std::string encodeReal(double value) {
    // No NaN case: Expr::real refuses one, so no Real can carry it this far.
    if (std::isinf(value)) {
        // Maxima's own names for the two infinities.
        return value > 0 ? "$INF" : "$MINF";
    }
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
bool isStringLiteral(std::string_view text) {
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
    for (std::size_t i = text.size() - 1; i-- > 1 && text[i] == '\\';) {
        ++backslashes;
    }
    return backslashes % 2 == 0;
}

void render(const Expr &expr, std::string &out);

void renderApplication(std::string_view head, const std::vector<Expr> &args,
                       std::string &out) {
    out += "((";
    out += head;
    out += ")";
    for (const Expr &arg : args) {
        out.push_back(' ');
        render(arg, out);
    }
    out.push_back(')');
}

std::string_view functionHead(const std::string &name, std::string &storage) {
    for (const auto &[display, internal] : kInternalHeads) {
        if (name == display) {
            return internal;
        }
    }
    // A quoted head is a noun: 'integrate is %INTEGRATE. Anything else is a
    // verb, which Maxima will evaluate — and, for its own functions, turn
    // into the noun itself.
    if (!name.empty() && name.front() == '\'') {
        storage = encodeWithSigil('%', std::string_view(name).substr(1));
    } else {
        storage = encodeWithSigil('$', name);
    }
    return storage;
}

void render(const Expr &expr, std::string &out) {
    switch (expr.kind()) {
    case Kind::Integer:
        out += expr.integerValue().toString();
        return;

    case Kind::Rational:
        // MQUOTIENT rather than RAT: RAT is what the simplifier *produces*,
        // and handing it an unsimplified one is asking for trouble. A quotient
        // is what Maxima's own parser emits for `1/3`, and it simplifies to
        // the RAT form on arrival.
        out += "((MQUOTIENT) " + expr.numerator().toString() + " "
               + expr.denominator().toString() + ")";
        return;

    case Kind::Real:
        out += encodeReal(expr.realValue());
        return;

    case Kind::Symbol: {
        const std::string &name = expr.name();
        // Maxima's booleans are the Lisp ones, as fromMaxima already knows.
        if (name == "true") {
            out += "T";
        } else if (name == "false") {
            out += "NIL";
        } else {
            out += encodeMaximaName(name);
        }
        return;
    }

    case Kind::Add:
        renderApplication("MPLUS", expr.args(), out);
        return;
    case Kind::Mul:
        renderApplication("MTIMES", expr.args(), out);
        return;
    case Kind::Pow:
        renderApplication("MEXPT", expr.args(), out);
        return;

    case Kind::Function: {
        std::string storage;
        renderApplication(functionHead(expr.name(), storage), expr.args(), out);
        return;
    }

    case Kind::Relation:
        renderApplication(relationHead(expr.relationOp()), expr.args(), out);
        return;

    case Kind::Opaque: {
        const std::string &text = expr.opaqueText();
        // A string is a string: fromMaxima wraps Maxima strings as quoted
        // Opaque text, and this sends them back as Lisp strings, which use
        // the same escapes. Anything else is Maxima source this library never
        // interpreted, so Maxima parses it — inside errcatch, where a read
        // error is an ordinary failure rather than a silence.
        if (isStringLiteral(text)) {
            out += text;
        } else {
            out += "(($EVAL_STRING) " + stringLiteral(text) + ")";
        }
        return;
    }
    }
    throw Error("cannot render expression for Maxima");
}

} // namespace

std::string encodeMaximaName(std::string_view name) {
    return encodeWithSigil('$', name);
}

std::string stringLiteral(std::string_view text) {
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

std::string toMaxima(const Expr &expr) {
    std::string out;
    render(expr, out);
    return out;
}

} // namespace mx::detail
