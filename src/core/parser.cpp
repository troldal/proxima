#include <mx/errors.hpp>
#include <mx/expr.hpp>

#include <charconv>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// A Pratt parser for infix expressions, with no kernel behind it.
//
// Deliberately a *subset* of Maxima's syntax: arithmetic, comparisons, function
// application, lists. Statements — assignment, definition, quoting,
// non-commutative multiplication, blocks — are not here, because this parses
// expressions rather than programs. Anything beyond the subset goes to
// mx::parse, which hands the text to Maxima's own parser and so cannot drift
// from it.
//
// Precedences are Maxima's, which matters for the two that surprise people:
// `^` is right-associative, so `x^2^3` is `x^(2^3)`; and unary minus binds
// *looser* than `^`, so `-x^2` is `-(x^2)` and not `(-x)^2`.

namespace mx {
namespace {

enum Precedence {
    kNone = 0,
    kRelation = 80,
    kAddSub = 100,
    kUnary = 100, // Same as addition, which is what makes -x^2 mean -(x^2).
    kMulDiv = 120,
    kPower = 140,
    kPostfix = 160,
};

struct Token {
    enum class Kind {
        End,
        Number,
        Symbol,
        String,
        Plus,
        Minus,
        Star,
        Slash,
        Caret,
        Bang,
        Equal,
        NotEqual,
        Less,
        LessEqual,
        Greater,
        GreaterEqual,
        LeftParen,
        RightParen,
        LeftBracket,
        RightBracket,
        Comma,
    };

    Kind kind = Kind::End;
    std::string text;
    bool isInteger = false;
    std::size_t at = 0;
};

bool isSymbolStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'
           || c == '%';
}

bool isSymbolPart(char c) {
    return isSymbolStart(c) || (c >= '0' && c <= '9');
}

bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

class Lexer {
public:
    explicit Lexer(std::string_view source) : source_(source) {}

    Token next() {
        while (at_ < source_.size()
               && static_cast<unsigned char>(source_[at_]) <= ' ') {
            ++at_;
        }
        const std::size_t start = at_;
        if (at_ >= source_.size()) {
            return {Token::Kind::End, {}, false, start};
        }

        const char c = source_[at_];

        if (isDigit(c) || (c == '.' && at_ + 1 < source_.size()
                           && isDigit(source_[at_ + 1]))) {
            return number(start);
        }
        if (isSymbolStart(c)) {
            while (at_ < source_.size() && isSymbolPart(source_[at_])) {
                ++at_;
            }
            return {Token::Kind::Symbol,
                    std::string(source_.substr(start, at_ - start)), false, start};
        }
        if (c == '"') {
            return string(start);
        }

        ++at_;
        switch (c) {
        case '+': return {Token::Kind::Plus, "+", false, start};
        case '-': return {Token::Kind::Minus, "-", false, start};
        case '*': return {Token::Kind::Star, "*", false, start};
        case '/': return {Token::Kind::Slash, "/", false, start};
        case '^': return {Token::Kind::Caret, "^", false, start};
        case '!': return {Token::Kind::Bang, "!", false, start};
        case '=': return {Token::Kind::Equal, "=", false, start};
        case '#': return {Token::Kind::NotEqual, "#", false, start};
        case '(': return {Token::Kind::LeftParen, "(", false, start};
        case ')': return {Token::Kind::RightParen, ")", false, start};
        case '[': return {Token::Kind::LeftBracket, "[", false, start};
        case ']': return {Token::Kind::RightBracket, "]", false, start};
        case ',': return {Token::Kind::Comma, ",", false, start};
        case '<':
            if (at_ < source_.size() && source_[at_] == '=') {
                ++at_;
                return {Token::Kind::LessEqual, "<=", false, start};
            }
            return {Token::Kind::Less, "<", false, start};
        case '>':
            if (at_ < source_.size() && source_[at_] == '=') {
                ++at_;
                return {Token::Kind::GreaterEqual, ">=", false, start};
            }
            return {Token::Kind::Greater, ">", false, start};
        default:
            break;
        }
        throw ParseError("unexpected character '" + std::string(1, c)
                         + "' at offset " + std::to_string(start));
    }

private:
    Token number(std::size_t start) {
        bool isInteger = true;
        while (at_ < source_.size() && isDigit(source_[at_])) {
            ++at_;
        }
        if (at_ < source_.size() && source_[at_] == '.') {
            isInteger = false;
            ++at_;
            while (at_ < source_.size() && isDigit(source_[at_])) {
                ++at_;
            }
        }
        if (at_ < source_.size() && (source_[at_] == 'e' || source_[at_] == 'E')) {
            const std::size_t mark = at_;
            ++at_;
            if (at_ < source_.size() && (source_[at_] == '+' || source_[at_] == '-')) {
                ++at_;
            }
            if (at_ < source_.size() && isDigit(source_[at_])) {
                isInteger = false;
                while (at_ < source_.size() && isDigit(source_[at_])) {
                    ++at_;
                }
            } else {
                // Not an exponent after all — `2e` is the number 2 followed by
                // the symbol e, which is how Maxima reads it too.
                at_ = mark;
            }
        }
        return {Token::Kind::Number,
                std::string(source_.substr(start, at_ - start)), isInteger, start};
    }

    Token string(std::size_t start) {
        ++at_; // Opening quote.
        std::string value;
        while (at_ < source_.size() && source_[at_] != '"') {
            if (source_[at_] == '\\' && at_ + 1 < source_.size()) {
                ++at_;
            }
            value.push_back(source_[at_++]);
        }
        if (at_ >= source_.size()) {
            throw ParseError("unterminated string starting at offset "
                             + std::to_string(start));
        }
        ++at_; // Closing quote.
        return {Token::Kind::String, std::move(value), false, start};
    }

    std::string_view source_;
    std::size_t at_ = 0;
};

int leftBindingPower(Token::Kind kind) {
    switch (kind) {
    case Token::Kind::Equal:
    case Token::Kind::NotEqual:
    case Token::Kind::Less:
    case Token::Kind::LessEqual:
    case Token::Kind::Greater:
    case Token::Kind::GreaterEqual:
        return kRelation;
    case Token::Kind::Plus:
    case Token::Kind::Minus:
        return kAddSub;
    case Token::Kind::Star:
    case Token::Kind::Slash:
        return kMulDiv;
    case Token::Kind::Caret:
        return kPower;
    case Token::Kind::Bang:
        return kPostfix;
    default:
        return kNone;
    }
}

RelOp relationFor(Token::Kind kind) {
    switch (kind) {
    case Token::Kind::NotEqual:
        return RelOp::NotEqual;
    case Token::Kind::Less:
        return RelOp::Less;
    case Token::Kind::LessEqual:
        return RelOp::LessEqual;
    case Token::Kind::Greater:
        return RelOp::Greater;
    case Token::Kind::GreaterEqual:
        return RelOp::GreaterEqual;
    default:
        return RelOp::Equal;
    }
}

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

class Parser {
public:
    explicit Parser(std::string_view source) : lexer_(source) {
        current_ = lexer_.next();
    }

    Expr parse() {
        Expr result = expression(kNone);
        expect(Token::Kind::End, "end of input");
        return result;
    }

private:
    void advance() {
        current_ = lexer_.next();
    }

    [[noreturn]] void unexpected(std::string_view wanted) const {
        const std::string found
            = current_.kind == Token::Kind::End ? "end of input"
                                                : "'" + current_.text + "'";
        throw ParseError("expected " + std::string(wanted) + " but found "
                         + found + " at offset "
                         + std::to_string(current_.at));
    }

    void expect(Token::Kind kind, std::string_view wanted) {
        if (current_.kind != kind) {
            unexpected(wanted);
        }
        if (kind != Token::Kind::End) {
            advance();
        }
    }

    /// The Pratt loop: parse a prefix, then keep absorbing infix and postfix
    /// operators that bind more tightly than the caller allows.
    Expr expression(int minimumPower) {
        Expr left = prefix();
        while (leftBindingPower(current_.kind) > minimumPower) {
            left = infix(std::move(left));
        }
        return left;
    }

    Expr prefix() {
        const Token token = current_;
        switch (token.kind) {
        case Token::Kind::Number:
            advance();
            return number(token);

        case Token::Kind::String:
            // No string node, so it becomes source text — the same choice the
            // Maxima mapping makes.
            advance();
            return Expr::opaque(quoteString(token.text));

        case Token::Kind::Symbol: {
            advance();
            if (current_.kind == Token::Kind::LeftParen) {
                advance();
                return Expr::function(token.text,
                                      arguments(Token::Kind::RightParen, ")"));
            }
            return Expr::symbol(token.text);
        }

        case Token::Kind::Minus:
            advance();
            // Binds looser than '^', so -x^2 is -(x^2).
            return -expression(kUnary);

        case Token::Kind::Plus:
            advance();
            return expression(kUnary);

        case Token::Kind::LeftParen: {
            advance();
            Expr inner = expression(kNone);
            expect(Token::Kind::RightParen, "')'");
            return inner;
        }

        case Token::Kind::LeftBracket:
            advance();
            // Maxima writes lists in brackets and has no textual `list(...)`
            // constructor; the printer renders this head back as brackets.
            return Expr::function("list",
                                  arguments(Token::Kind::RightBracket, "']'"));

        default:
            break;
        }
        unexpected("an expression");
    }

    Expr infix(Expr left) {
        const Token token = current_;
        advance();

        switch (token.kind) {
        case Token::Kind::Plus:
            return std::move(left) + expression(kAddSub);
        case Token::Kind::Minus:
            return std::move(left) - expression(kAddSub);
        case Token::Kind::Star:
            return std::move(left) * expression(kMulDiv);
        case Token::Kind::Slash:
            return std::move(left) / expression(kMulDiv);
        case Token::Kind::Caret:
            // Right-associative, so the right operand is parsed at one below
            // this operator's own power: x^2^3 is x^(2^3).
            return pow(left, expression(kPower - 1));
        case Token::Kind::Bang:
            // Postfix, so nothing follows it to parse.
            return Expr::function("factorial", {std::move(left)});
        default:
            break;
        }
        return Expr::relation(relationFor(token.kind), std::move(left),
                              expression(kRelation));
    }

    std::vector<Expr> arguments(Token::Kind closer, std::string_view wanted) {
        std::vector<Expr> args;
        if (current_.kind == closer) {
            advance();
            return args;
        }
        for (;;) {
            args.push_back(expression(kNone));
            if (current_.kind == Token::Kind::Comma) {
                advance();
                continue;
            }
            expect(closer, wanted);
            return args;
        }
    }

    static Expr number(const Token &token) {
        if (token.isInteger) {
            Integer value = 0;
            const char *first = token.text.data();
            const char *last = first + token.text.size();
            const auto [stopped, error] = std::from_chars(first, last, value);
            if (error == std::errc{} && stopped == last) {
                return Expr::integer(value);
            }
            // Beyond mx::Integer. Kept exactly, as text — the same fallback the
            // Maxima mapping uses for a bignum.
            return Expr::opaque(token.text);
        }

        double value = 0.0;
        const char *first = token.text.data();
        const char *last = first + token.text.size();
        const auto [stopped, error] = std::from_chars(first, last, value);
        if (error != std::errc{} || stopped != last) {
            throw ParseError("malformed number '" + token.text + "' at offset "
                             + std::to_string(token.at));
        }
        return Expr::real(value);
    }

    Lexer lexer_;
    Token current_;
};

} // namespace

Expr Expr::parse(std::string_view source) {
    return Parser(source).parse();
}

} // namespace mx
