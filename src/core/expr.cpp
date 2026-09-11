#include "core/node.hpp"

#include "core/normalize.hpp"

#include <mx/errors.hpp>

#include <limits>
#include <numeric>
#include <utility>

namespace mx {

namespace detail {
Expr makeExpr(std::shared_ptr<const Node> node) {
    return Expr(std::move(node));
}
} // namespace detail

namespace {

using detail::Node;

void hashCombine(std::size_t &seed, std::size_t value) {
    // The usual mixing constant; adequate for a hash table key, and cheap.
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

std::size_t hashOf(const Node &node) {
    std::size_t seed = std::hash<int>{}(static_cast<int>(node.kind));

    switch (node.kind) {
    case Kind::Integer:
        hashCombine(seed, std::hash<Integer>{}(node.integer));
        break;
    case Kind::Rational:
        hashCombine(seed, std::hash<Integer>{}(node.integer));
        hashCombine(seed, std::hash<Integer>{}(node.denominator));
        break;
    case Kind::Real:
        hashCombine(seed, std::hash<double>{}(node.real));
        break;
    case Kind::Symbol:
    case Kind::Opaque:
        hashCombine(seed, std::hash<std::string>{}(node.text));
        break;
    case Kind::Function:
        hashCombine(seed, std::hash<std::string>{}(node.text));
        break;
    case Kind::Relation:
        hashCombine(seed, std::hash<int>{}(static_cast<int>(node.relOp)));
        break;
    case Kind::Add:
    case Kind::Mul:
    case Kind::Pow:
        break;
    }

    // Order-sensitive, which is correct: these are structural hashes, and the
    // normaliser (PLAN.md step 10) is what makes x+1 and 1+x agree by putting
    // their operands in a canonical order first.
    for (const Expr &arg : node.args) {
        hashCombine(seed, arg.hash());
    }
    return seed;
}

Expr finish(Node node) {
    node.hash = hashOf(node);
    return detail::makeExpr(std::make_shared<const Node>(std::move(node)));
}

[[noreturn]] void wrongKind(const char *wanted, Kind actual) {
    throw Error(std::string("expression is not ") + wanted + " (kind "
                + std::to_string(static_cast<int>(actual)) + ")");
}

} // namespace

// --- construction ---------------------------------------------------------

Expr Expr::makeInteger(Integer value) {
    Node node;
    node.kind = Kind::Integer;
    node.integer = value;
    return finish(std::move(node));
}

Expr::Expr() : Expr(makeInteger(0)) {}

Expr::Expr(double value) : Expr(real(value)) {}

Expr Expr::integer(Integer value) {
    return makeInteger(value);
}

Expr Expr::rational(Integer numerator, Integer denominator) {
    if (denominator == 0) {
        throw Error("rational with zero denominator");
    }

    // Negating the extreme negative value overflows, so the one case that
    // cannot be normalised in mx::Integer becomes Opaque rather than silently
    // wrapping. The same escape hatch bignums use.
    constexpr Integer kMin = std::numeric_limits<Integer>::min();
    if ((numerator == kMin && denominator == -1)
        || (denominator == kMin && numerator == -1)) {
        return opaque("9223372036854775808");
    }
    if (denominator == kMin || numerator == kMin) {
        // Can still be reduced safely only if the gcd removes the extreme.
        const Integer divisor = static_cast<Integer>(
            std::gcd(static_cast<std::uint64_t>(numerator < 0 ? -(numerator + 1) + 1
                                                              : numerator),
                     static_cast<std::uint64_t>(denominator < 0
                                                    ? -(denominator + 1) + 1
                                                    : denominator)));
        if (divisor <= 1) {
            return opaque(std::to_string(numerator) + "/"
                          + std::to_string(denominator));
        }
        numerator /= divisor;
        denominator /= divisor;
    }

    const Integer divisor = std::gcd(numerator, denominator);
    if (divisor != 0) {
        numerator /= divisor;
        denominator /= divisor;
    }
    if (denominator < 0) {
        numerator = -numerator;
        denominator = -denominator;
    }
    if (denominator == 1) {
        return makeInteger(numerator);
    }

    Node node;
    node.kind = Kind::Rational;
    node.integer = numerator;
    node.denominator = denominator;
    return finish(std::move(node));
}

Expr Expr::real(double value) {
    Node node;
    node.kind = Kind::Real;
    node.real = value;
    return finish(std::move(node));
}

Expr Expr::symbol(std::string name) {
    Node node;
    node.kind = Kind::Symbol;
    node.text = std::move(name);
    return finish(std::move(node));
}

Expr Expr::function(std::string head, std::vector<Expr> args) {
    Node node;
    node.kind = Kind::Function;
    node.text = std::move(head);
    node.args = std::move(args);
    return finish(std::move(node));
}

Expr Expr::relation(RelOp op, Expr lhs, Expr rhs) {
    Node node;
    node.kind = Kind::Relation;
    node.relOp = op;
    node.args = {std::move(lhs), std::move(rhs)};
    return finish(std::move(node));
}

Expr Expr::opaque(std::string text) {
    Node node;
    node.kind = Kind::Opaque;
    node.text = std::move(text);
    return finish(std::move(node));
}

Expr Expr::add(std::vector<Expr> terms) {
    // Normalised at construction, so every Expr in existence is in canonical
    // form and equality never has to re-derive it.
    terms = detail::normalizeSum(std::move(terms));
    if (terms.empty()) {
        return integer(0);
    }
    if (terms.size() == 1) {
        return std::move(terms.front());
    }
    Node node;
    node.kind = Kind::Add;
    node.args = std::move(terms);
    return finish(std::move(node));
}

Expr Expr::mul(std::vector<Expr> factors) {
    factors = detail::normalizeProduct(std::move(factors));
    if (factors.empty()) {
        return integer(1);
    }
    if (factors.size() == 1) {
        return std::move(factors.front());
    }
    Node node;
    node.kind = Kind::Mul;
    node.args = std::move(factors);
    return finish(std::move(node));
}

Expr Expr::pow(Expr base, Expr exponent) {
    if (auto simplified = detail::normalizePower(base, exponent)) {
        return *simplified;
    }
    Node node;
    node.kind = Kind::Pow;
    node.args = {std::move(base), std::move(exponent)};
    return finish(std::move(node));
}

// --- inspection -----------------------------------------------------------

Kind Expr::kind() const {
    return node_->kind;
}

bool Expr::isNumber() const {
    const Kind k = node_->kind;
    return k == Kind::Integer || k == Kind::Rational || k == Kind::Real;
}

bool Expr::isNegativeNumber() const {
    switch (node_->kind) {
    case Kind::Integer:
    case Kind::Rational:
        return node_->integer < 0; // Denominator is always positive.
    case Kind::Real:
        return node_->real < 0.0;
    default:
        return false;
    }
}

Integer Expr::integerValue() const {
    if (node_->kind != Kind::Integer) {
        wrongKind("an integer", node_->kind);
    }
    return node_->integer;
}

Integer Expr::numerator() const {
    if (node_->kind == Kind::Integer) {
        return node_->integer;
    }
    if (node_->kind != Kind::Rational) {
        wrongKind("a rational", node_->kind);
    }
    return node_->integer;
}

Integer Expr::denominator() const {
    if (node_->kind == Kind::Integer) {
        return 1;
    }
    if (node_->kind != Kind::Rational) {
        wrongKind("a rational", node_->kind);
    }
    return node_->denominator;
}

double Expr::realValue() const {
    if (node_->kind != Kind::Real) {
        wrongKind("a real", node_->kind);
    }
    return node_->real;
}

const std::string &Expr::name() const {
    if (node_->kind != Kind::Symbol && node_->kind != Kind::Function) {
        wrongKind("a symbol or function", node_->kind);
    }
    return node_->text;
}

RelOp Expr::relationOp() const {
    if (node_->kind != Kind::Relation) {
        wrongKind("a relation", node_->kind);
    }
    return node_->relOp;
}

const std::string &Expr::opaqueText() const {
    if (node_->kind != Kind::Opaque) {
        wrongKind("opaque", node_->kind);
    }
    return node_->text;
}

const std::vector<Expr> &Expr::args() const {
    return node_->args;
}

std::size_t Expr::arity() const {
    return node_->args.size();
}

const Expr &Expr::arg(std::size_t index) const {
    if (index >= node_->args.size()) {
        throw Error("expression has no operand " + std::to_string(index)
                    + " (it has " + std::to_string(node_->args.size()) + ")");
    }
    return node_->args[index];
}

std::size_t Expr::hash() const {
    return node_->hash;
}

bool Expr::operator==(const Expr &other) const {
    if (node_ == other.node_) {
        return true; // Shared representation: the common case after a copy.
    }
    const Node &a = *node_;
    const Node &b = *other.node_;
    if (a.kind != b.kind || a.hash != b.hash) {
        return false;
    }

    switch (a.kind) {
    case Kind::Integer:
        return a.integer == b.integer;
    case Kind::Rational:
        return a.integer == b.integer && a.denominator == b.denominator;
    case Kind::Real:
        return a.real == b.real;
    case Kind::Symbol:
    case Kind::Opaque:
        return a.text == b.text;
    case Kind::Function:
        return a.text == b.text && a.args == b.args;
    case Kind::Relation:
        return a.relOp == b.relOp && a.args == b.args;
    case Kind::Add:
    case Kind::Mul:
    case Kind::Pow:
        return a.args == b.args;
    }
    return false;
}

// --- operators ------------------------------------------------------------

Expr operator+(const Expr &lhs, const Expr &rhs) {
    return Expr::add({lhs, rhs});
}

Expr operator-(const Expr &lhs, const Expr &rhs) {
    return Expr::add({lhs, -rhs});
}

Expr operator*(const Expr &lhs, const Expr &rhs) {
    return Expr::mul({lhs, rhs});
}

Expr operator/(const Expr &lhs, const Expr &rhs) {
    // Exact division of exact integers stays exact. Without this, `Expr(1)/3`
    // would be 1*3^-1, which is correct but a poor thing to hand a user before
    // the normaliser exists.
    if (lhs.is(Kind::Integer) && rhs.is(Kind::Integer)
        && rhs.integerValue() != 0) {
        return Expr::rational(lhs.integerValue(), rhs.integerValue());
    }
    return Expr::mul({lhs, Expr::pow(rhs, Expr::integer(-1))});
}

Expr operator-(const Expr &operand) {
    // Negating a literal gives a literal, so `-x + 1` does not print as
    // `-1*x + 1`. Everything else becomes a product with -1, which is how
    // Maxima represents negation internally too.
    switch (operand.kind()) {
    case Kind::Integer:
        if (operand.integerValue() != std::numeric_limits<Integer>::min()) {
            return Expr::integer(-operand.integerValue());
        }
        break;
    case Kind::Rational:
        return Expr::rational(-operand.numerator(), operand.denominator());
    case Kind::Real:
        return Expr::real(-operand.realValue());
    default:
        break;
    }
    return Expr::mul({Expr::integer(-1), operand});
}

Expr operator+(const Expr &operand) {
    return operand;
}

Expr pow(const Expr &base, const Expr &exponent) {
    return Expr::pow(base, exponent);
}

Expr eq(Expr lhs, Expr rhs) {
    return Expr::relation(RelOp::Equal, std::move(lhs), std::move(rhs));
}
Expr ne(Expr lhs, Expr rhs) {
    return Expr::relation(RelOp::NotEqual, std::move(lhs), std::move(rhs));
}
Expr lt(Expr lhs, Expr rhs) {
    return Expr::relation(RelOp::Less, std::move(lhs), std::move(rhs));
}
Expr le(Expr lhs, Expr rhs) {
    return Expr::relation(RelOp::LessEqual, std::move(lhs), std::move(rhs));
}
Expr gt(Expr lhs, Expr rhs) {
    return Expr::relation(RelOp::Greater, std::move(lhs), std::move(rhs));
}
Expr ge(Expr lhs, Expr rhs) {
    return Expr::relation(RelOp::GreaterEqual, std::move(lhs), std::move(rhs));
}

std::string_view symbolFor(RelOp op) {
    switch (op) {
    case RelOp::Equal:
        return "=";
    case RelOp::NotEqual:
        return "#"; // Maxima's spelling, not "!=".
    case RelOp::Less:
        return "<";
    case RelOp::LessEqual:
        return "<=";
    case RelOp::Greater:
        return ">";
    case RelOp::GreaterEqual:
        return ">=";
    }
    return "=";
}

} // namespace mx
