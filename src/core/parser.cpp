#include <proxima/errors.hpp>
#include <proxima/expr.hpp>

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h> // _AddressOfReturnAddress, for the parser's stack budget.
#endif

// A Pratt parser for infix expressions, with no kernel behind it.
//
// Deliberately a *subset* of Maxima's syntax: arithmetic, comparisons, function
// application, lists. Statements — assignment, definition, quoting,
// non-commutative multiplication, blocks — are not here, because this parses
// expressions rather than programs. Anything beyond the subset goes to
// proxima::parse, which hands the text to Maxima's own parser and so cannot drift
// from it.
//
// Precedences are Maxima's, which matters for the two that surprise people:
// `^` is right-associative, so `x^2^3` is `x^(2^3)`; and unary minus binds
// *looser* than `^`, so `-x^2` is `-(x^2)` and not `(-x)^2`.
//
// So is the lexing of `!`. Maxima reads `!!` as one token, the double
// factorial, and reads it greedily: `x!!` is `double_factorial(x)`, `x!!!` is
// `factorial(double_factorial(x))`, and only `x! !`, with a space, is a
// factorial taken twice.

namespace proxima {
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
        BangBang,
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
    bool is_integer = false;
    std::size_t at = 0;
};

bool is_symbol_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'
           || c == '%';
}

bool is_symbol_part(char c) {
    return is_symbol_start(c) || (c >= '0' && c <= '9');
}

bool is_digit(char c) {
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

        if (is_digit(c) || (c == '.' && at_ + 1 < source_.size()
                           && is_digit(source_[at_ + 1]))) {
            return number(start);
        }
        if (is_symbol_start(c)) {
            while (at_ < source_.size() && is_symbol_part(source_[at_])) {
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
        case '*':
            // Maxima reads ** as ^, when the two stars are adjacent.
            if (at_ < source_.size() && source_[at_] == '*') {
                ++at_;
                return {Token::Kind::Caret, "**", false, start};
            }
            return {Token::Kind::Star, "*", false, start};
        case '/': return {Token::Kind::Slash, "/", false, start};
        case '^': return {Token::Kind::Caret, "^", false, start};
        case '!':
            // Greedy, as Maxima's lexer is: "!!!" is "!!" then "!".
            if (at_ < source_.size() && source_[at_] == '!') {
                ++at_;
                return {Token::Kind::BangBang, "!!", false, start};
            }
            return {Token::Kind::Bang, "!", false, start};
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
        bool is_integer = true;
        while (at_ < source_.size() && is_digit(source_[at_])) {
            ++at_;
        }
        if (at_ < source_.size() && source_[at_] == '.') {
            is_integer = false;
            ++at_;
            while (at_ < source_.size() && is_digit(source_[at_])) {
                ++at_;
            }
        }
        if (at_ < source_.size() && (source_[at_] == 'e' || source_[at_] == 'E')) {
            const std::size_t mark = at_;
            ++at_;
            if (at_ < source_.size() && (source_[at_] == '+' || source_[at_] == '-')) {
                ++at_;
            }
            if (at_ < source_.size() && is_digit(source_[at_])) {
                is_integer = false;
                while (at_ < source_.size() && is_digit(source_[at_])) {
                    ++at_;
                }
            } else {
                // Not an exponent after all — `2e` is the number 2 followed by
                // the symbol e, which is how Maxima reads it too.
                at_ = mark;
            }
        }
        return {Token::Kind::Number,
                std::string(source_.substr(start, at_ - start)), is_integer, start};
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

int left_binding_power(Token::Kind kind) {
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
    case Token::Kind::BangBang:
        return kPostfix;
    default:
        return kNone;
    }
}

RelOp relation_for(Token::Kind kind) {
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

/// How deeply an expression may nest, whatever the stack allows. Far beyond
/// anything written by hand, and the same as the reader of Maxima's replies.
constexpr std::size_t kMaxParseDepth = 1000;

/// How much stack the parser may use before refusing to nest further.
///
/// A count of levels alone was not enough: a Debug build on Windows spends
/// several times the stack per level a Release build does, on a 1 MB stack,
/// and overflowed well before 1000 levels. So the stack actually used is
/// measured too. This much leaves room below the parser on any main thread;
/// a caller parsing on a thread with a smaller stack than that should say so
/// by not doing it.
constexpr std::uintptr_t kParseStackBudget = std::uintptr_t{256} * 1024;

/// An address inside the current stack frame: the real stack, even under
/// AddressSanitizer, which can move locals off it.
inline std::uintptr_t stack_address() {
#if defined(_MSC_VER)
    return reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress());
#else
    return reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
#endif
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
    std::size_t depth_ = 0;

    /// Where the stack stood when parsing began, to measure its use from.
    std::uintptr_t stack_base_ = stack_address();

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
    Expr expression(int minimum_power) {
        // Every way to nest — parentheses, a unary sign, the right of `^` or a
        // relation, a function's arguments — comes back through here, so this
        // is the one place to guard. Unguarded, 200,000 opening parentheses
        // overflowed the stack and killed the process. The stack grows down on
        // every platform this builds for, but the distance is taken either way.
        const std::uintptr_t here = stack_address();
        const std::uintptr_t used = here < stack_base_ ? stack_base_ - here : here - stack_base_;
        if (depth_ >= kMaxParseDepth || used > kParseStackBudget) {
            throw ParseError("expression nested too deep at offset "
                             + std::to_string(current_.at));
        }
        ++depth_;
        struct Leave {
            std::size_t &depth;
            ~Leave() { --depth; }
        } leave{depth_};

        Expr left = prefix();
        while (left_binding_power(current_.kind) > minimum_power) {
            switch (current_.kind) {
            case Token::Kind::Plus:
            case Token::Kind::Minus:
                left = sum(std::move(left));
                break;
            case Token::Kind::Star:
            case Token::Kind::Slash:
                left = product(std::move(left));
                break;
            default:
                left = infix(std::move(left));
                break;
            }
        }
        return left;
    }

    // A run of `+` and `-`, or of `*` and `/`, is collected and built once.
    // Folding it one operator at a time, as `left + right` did, normalised the
    // whole growing sum again at every step: a 4000-term sum took nine seconds
    // to parse, where one Expr::add over the same terms takes milliseconds.
    // The result is the same expression, since canonical form does not depend
    // on how the operands were grouped.

    Expr sum(Expr first) {
        std::vector<Expr> terms;
        terms.push_back(std::move(first));
        while (current_.kind == Token::Kind::Plus
               || current_.kind == Token::Kind::Minus) {
            const bool subtract = current_.kind == Token::Kind::Minus;
            advance();
            Expr term = expression(kAddSub);
            terms.push_back(subtract ? -term : std::move(term));
        }
        return Expr::add(std::move(terms));
    }

    Expr product(Expr first) {
        std::vector<Expr> factors;
        factors.push_back(std::move(first));
        while (current_.kind == Token::Kind::Star
               || current_.kind == Token::Kind::Slash) {
            const bool divide = current_.kind == Token::Kind::Slash;
            advance();
            Expr factor = expression(kMulDiv);
            // 1 / factor is exactly what operator/ multiplies by: a reciprocal
            // for a number, a negative power for anything else.
            factors.push_back(divide ? Expr::integer(1) / factor : std::move(factor));
        }
        return Expr::mul(std::move(factors));
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
            return Expr::opaque(quote_string(token.text));

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

        // `+ - * /` never arrive here: expression() hands their runs to sum()
        // and product().
        switch (token.kind) {
        case Token::Kind::Caret:
            // Right-associative, so the right operand is parsed at one below
            // this operator's own power: x^2^3 is x^(2^3).
            return pow(left, expression(kPower - 1));
        case Token::Kind::Bang:
            // Postfix, so nothing follows it to parse.
            return Expr::function("factorial", {std::move(left)});
        case Token::Kind::BangBang:
            // Not factorial(factorial(x)), which is a different number: 5!! is
            // 15, (5!)! has 199 digits. `double_factorial` is also the name
            // Maxima reads to the same noun, so it prints back faithfully.
            return Expr::function("double_factorial", {std::move(left)});
        default:
            break;
        }
        Expr right = expression(kRelation);
        if (left_binding_power(current_.kind) == kRelation) {
            // a < b < c. Maxima refuses it — a relation is not something to
            // compare — and accepting it as (a < b) < c would give text a
            // meaning Maxima never gives it. Parenthesised, it is legal in both,
            // and that path does not come through here.
            throw ParseError("chained relation '" + current_.text
                             + "' at offset " + std::to_string(current_.at)
                             + ": parenthesise one side");
        }
        return Expr::relation(relation_for(token.kind), std::move(left),
                              std::move(right));
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
        if (token.is_integer) {
            // Any size: proxima::Integer is unbounded, so a literal factorial reads
            // as a number rather than as opaque text.
            if (auto value = Integer::parse(token.text)) {
                return Expr::integer(std::move(*value));
            }
            throw ParseError("malformed integer " + token.text + " at offset "
                             + std::to_string(token.at));
        }

        double value = 0.0;
        const char *first = token.text.data();
        const char *last = first + token.text.size();
        const auto [stopped, error] = std::from_chars(first, last, value);
        if (error == std::errc::result_out_of_range && stopped == last) {
            // Well-formed, but beyond a double. Not quietly inf: 1e400 is a
            // finite number, and an infinity would be a different value.
            throw ParseError("number '" + token.text + "' at offset "
                             + std::to_string(token.at)
                             + " is out of the range of a double");
        }
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

result<Expr> Expr::parse(std::string_view source) {
    // The parser reports through ParseError internally, where an exception
    // is the simplest way out of a recursive descent; at the boundary it is
    // an ordinary outcome, and becomes one.
    try {
        return Parser(source).parse();
    } catch (const ParseError &error) {
        return fxt::unexpected(fail(Cause::Parse, error.what()));
    } catch (const OverflowError &error) {
        // `1e308 * 10.0` parses; building it does not. Still no answer, and
        // still the text's fault, so still a value.
        return fxt::unexpected(fail(Cause::Overflow, error.what()));
    }
}

} // namespace proxima
