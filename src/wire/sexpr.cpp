#include "wire/sexpr.hpp"

#include <proxima/errors.hpp>

#include <charconv>
#include <cstdlib>
#include <system_error>

namespace proxima::detail {
namespace {

bool is_delimiter(char c) {
    return c == '(' || c == ')' || c == '"' || c == ';'
           || static_cast<unsigned char>(c) <= ' ';
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

/// True for a token that is entirely an optional sign followed by digits.
bool looks_like_integer(std::string_view token) {
    size_t at = (token.starts_with('+') || token.starts_with('-')) ? 1 : 0;
    if (at >= token.size()) {
        return false;
    }
    for (; at < token.size(); ++at) {
        if (!is_digit(token[at])) {
            return false;
        }
    }
    return true;
}

/// Parses a Lisp float token.
///
/// Common Lisp permits several exponent markers — e/s/f/d/l — which select the
/// float format. Maxima sets *read-default-float-format* to double-float, so in
/// practice `e` or none is what arrives, but accepting the others costs one
/// substitution and avoids a mystifying parse failure if the setting ever
/// differs.
std::optional<double> parse_float(std::string_view token) {
    std::string normalised;
    normalised.reserve(token.size());
    bool saw_digit = false;
    bool saw_marker = false;
    bool saw_point = false;

    for (size_t at = 0; at < token.size(); ++at) {
        const char c = token[at];
        if (is_digit(c)) {
            saw_digit = true;
            normalised.push_back(c);
        } else if (c == '.') {
            if (saw_point || saw_marker) {
                return std::nullopt;
            }
            saw_point = true;
            normalised.push_back(c);
        } else if (c == '+' || c == '-') {
            normalised.push_back(c);
        } else if (c == 'e' || c == 'E' || c == 's' || c == 'S' || c == 'f'
                   || c == 'F' || c == 'd' || c == 'D' || c == 'l' || c == 'L') {
            if (saw_marker || !saw_digit) {
                return std::nullopt;
            }
            saw_marker = true;
            normalised.push_back('e');
        } else {
            return std::nullopt;
        }
    }

    if (!saw_digit || (!saw_point && !saw_marker)) {
        return std::nullopt; // An integer, or not a number at all.
    }
    // A trailing marker with no exponent digits ("1.0d") is how Lisp spells a
    // float of that format with exponent zero.
    if (!normalised.empty() && normalised.back() == 'e') {
        normalised.push_back('0');
    }

    double value = 0.0;
    const char *first = normalised.data();
    const char *last = first + normalised.size();
    const auto [stopped, error] = std::from_chars(first, last, value);
    if (error != std::errc{} || stopped != last) {
        return std::nullopt;
    }
    return value;
}

/// Tokeniser over the reply text. Produces one token at a time so the parser
/// can drive it without materialising the whole token stream.
class Lexer {
public:
    explicit Lexer(std::string_view text) : text_(text) {}

    /// QuotedSymbol is kept apart from Atom: Lisp bar-quotes a symbol
    /// precisely because its name would otherwise read as something else — a
    /// number, `...`, `#` — so its text must never be interpreted.
    enum class TokenKind { End, Open, Close, Atom, QuotedSymbol, String };

    struct Token {
        TokenKind kind = TokenKind::End;
        std::string text; // Atom text, or the unescaped string contents.
    };

    Token next() {
        skip_whitespace();
        if (at_ >= text_.size()) {
            return {TokenKind::End, {}};
        }

        const char c = text_[at_];
        if (c == '(') {
            ++at_;
            return {TokenKind::Open, {}};
        }
        if (c == ')') {
            ++at_;
            return {TokenKind::Close, {}};
        }
        if (c == '"') {
            return {TokenKind::String, read_string()};
        }
        if (c == '|') {
            return {TokenKind::QuotedSymbol, read_bar_symbol()};
        }
        if (c == ';') {
            // A Lisp comment, which Maxima never puts in a reply. Found by
            // fuzzing: ';' ends an atom but nothing consumed it, so every call
            // returned an empty atom at the same place, and the list growing
            // from them ran the process out of memory.
            throw ParseError("unexpected ';' in Maxima reply");
        }
        return {TokenKind::Atom, read_atom()};
    }

private:
    void skip_whitespace() {
        while (at_ < text_.size() && static_cast<unsigned char>(text_[at_]) <= ' ') {
            ++at_;
        }
    }

    std::string read_string() {
        ++at_; // Opening quote.
        std::string value;
        while (at_ < text_.size() && text_[at_] != '"') {
            if (text_[at_] == '\\' && at_ + 1 < text_.size()) {
                ++at_; // A backslash escapes the next character verbatim.
            }
            value.push_back(text_[at_++]);
        }
        if (at_ >= text_.size()) {
            throw ParseError("unterminated string in Maxima reply");
        }
        ++at_; // Closing quote.
        return value;
    }

    /// `|Symbol With Spaces|` — how Lisp prints a symbol whose name would
    /// otherwise not read back. The bars are not part of the name.
    std::string read_bar_symbol() {
        ++at_; // Opening bar.
        std::string name;
        while (at_ < text_.size() && text_[at_] != '|') {
            if (text_[at_] == '\\' && at_ + 1 < text_.size()) {
                ++at_;
            }
            name.push_back(text_[at_++]);
        }
        if (at_ >= text_.size()) {
            throw ParseError("unterminated |symbol| in Maxima reply");
        }
        ++at_; // Closing bar.
        return name;
    }

    std::string read_atom() {
        const size_t start = at_;
        while (at_ < text_.size() && !is_delimiter(text_[at_])) {
            ++at_;
        }
        if (at_ == start) {
            // Every delimiter is handled before this is reached, so an atom of
            // nothing means one was not. Refused rather than returned, since an
            // empty atom never advances and the reader would loop.
            throw ParseError("unreadable character in Maxima reply");
        }
        return std::string(text_.substr(start, at_ - start));
    }

    std::string_view text_;
    size_t at_ = 0;
};

SExpr atom_from(std::string text) {
    if (looks_like_integer(text)) {
        return SExpr::integer(std::move(text));
    }
    if (const auto value = parse_float(text)) {
        return SExpr::real(*value);
    }
    if (text == ".") {
        // Maxima's term representation is proper lists throughout. A dotted
        // pair would mean something unmodelled arrived, and silently treating
        // the dot as a symbol would corrupt the tree rather than say so.
        throw ParseError("dotted pairs are not supported in Maxima replies");
    }
    if (text == "..." || text == "#") {
        // What Lisp prints in place of the rest of a list under *print-length*,
        // and of a deeper list under *print-level*. A symbol really named
        // either would have been printed escaped, as |...| or |#|. Read as a
        // symbol, a truncated reply became a smaller expression that looked
        // complete; the helper now prints without limits, and this makes sure
        // a truncation can never pass silently again.
        throw ParseError("the Maxima reply was truncated by Lisp's print limits");
    }
    return SExpr::symbol(std::move(text));
}

void append_escaped(std::string &out, std::string_view text) {
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
}

} // namespace

SExpr SExpr::integer(std::string digits) {
    SExpr value;
    value.kind_ = Kind::Integer;
    value.text_ = std::move(digits);
    return value;
}

SExpr SExpr::real(double number) {
    SExpr value;
    value.kind_ = Kind::Real;
    value.real_ = number;
    return value;
}

SExpr SExpr::symbol(std::string name) {
    SExpr value;
    value.kind_ = Kind::Symbol;
    value.text_ = std::move(name);
    return value;
}

SExpr SExpr::string(std::string text) {
    SExpr value;
    value.kind_ = Kind::String;
    value.text_ = std::move(text);
    return value;
}

SExpr SExpr::list(std::vector<SExpr> items) {
    SExpr value;
    value.kind_ = Kind::List;
    value.items_ = std::move(items);
    return value;
}

std::optional<std::int64_t> SExpr::as_int64() const {
    if (kind_ != Kind::Integer || text_.empty()) {
        return std::nullopt;
    }
    const char *first = text_.data();
    const char *last = first + text_.size();
    if (*first == '+') {
        ++first; // from_chars rejects a leading '+'.
    }
    std::int64_t value = 0;
    const auto [stopped, error] = std::from_chars(first, last, value);
    if (error != std::errc{} || stopped != last) {
        return std::nullopt; // Out of range, which is the interesting case.
    }
    return value;
}

const SExpr &SExpr::at(std::size_t index) const {
    if (index >= items_.size()) {
        throw ParseError("Maxima reply has no element " + std::to_string(index)
                         + " (it has " + std::to_string(items_.size()) + ")");
    }
    return items_[index];
}

bool SExpr::operator==(const SExpr &other) const {
    if (kind_ != other.kind_) {
        return false;
    }
    switch (kind_) {
    case Kind::Integer:
    case Kind::Symbol:
    case Kind::String:
        return text_ == other.text_;
    case Kind::Real:
        return real_ == other.real_;
    case Kind::List:
        return items_ == other.items_;
    }
    return false;
}

std::string SExpr::to_string() const {
    switch (kind_) {
    case Kind::Integer:
        return text_;
    case Kind::Real: {
        char buffer[40];
        const auto [stopped, error]
            = std::to_chars(buffer, buffer + sizeof(buffer), real_);
        std::string rendered(buffer, error == std::errc{} ? stopped : buffer);
        // Keep it readable back as a float rather than an integer.
        if (rendered.find_first_of(".e") == std::string::npos) {
            rendered += ".0";
        }
        return rendered;
    }
    case Kind::Symbol:
        return text_;
    case Kind::String: {
        std::string out = "\"";
        append_escaped(out, text_);
        out.push_back('"');
        return out;
    }
    case Kind::List: {
        std::string out = "(";
        for (size_t i = 0; i < items_.size(); ++i) {
            if (i != 0) {
                out.push_back(' ');
            }
            out += items_[i].to_string();
        }
        out.push_back(')');
        return out;
    }
    }
    return {};
}

SExpr parse_sexpr(std::string_view text) {
    Lexer lexer(text);

    // An explicit stack rather than recursion, so that nesting depth costs heap
    // rather than call frames.
    std::vector<std::vector<SExpr>> open;
    std::optional<SExpr> result;

    for (;;) {
        const Lexer::Token token = lexer.next();

        if (token.kind == Lexer::TokenKind::End) {
            if (!open.empty()) {
                throw ParseError("unbalanced '(' in Maxima reply");
            }
            break;
        }

        if (token.kind == Lexer::TokenKind::Open) {
            if (open.size() >= kMaxSExprDepth) {
                throw ParseError("Maxima reply nested deeper than "
                                 + std::to_string(kMaxSExprDepth) + " levels");
            }
            open.emplace_back();
            continue;
        }

        SExpr value;
        switch (token.kind) {
        case Lexer::TokenKind::Close:
            if (open.empty()) {
                throw ParseError("unbalanced ')' in Maxima reply");
            }
            value = SExpr::list(std::move(open.back()));
            open.pop_back();
            break;
        case Lexer::TokenKind::String:
            value = SExpr::string(token.text);
            break;
        case Lexer::TokenKind::Atom:
            value = atom_from(token.text);
            break;
        case Lexer::TokenKind::QuotedSymbol:
            // Its name is its name. Passed through atom_from, as it used to be,
            // |123| read as the integer 123.
            value = SExpr::symbol(token.text);
            break;
        default:
            break;
        }

        if (open.empty()) {
            if (result) {
                throw ParseError("Maxima reply contained more than one "
                                 "expression");
            }
            result = std::move(value);
        } else {
            open.back().push_back(std::move(value));
        }
    }

    if (!result) {
        throw ParseError("Maxima reply was empty");
    }
    return std::move(*result);
}

} // namespace proxima::detail
