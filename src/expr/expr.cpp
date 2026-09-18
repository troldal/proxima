#include "expr/node.hpp"

#include <proxima/symbol.hpp>

#include "expr/normalize.hpp"

#include <proxima/errors.hpp>

#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace proxima {

namespace detail {
Expr make_expr(std::shared_ptr<const Node> node) {
    return Expr(std::move(node));
}
} // namespace detail

namespace {

using detail::Application;
using detail::Node;

void hash_combine(std::size_t &seed, std::size_t value) {
    // The usual mixing constant; adequate for a hash table key, and cheap.
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
}

std::size_t hash_of(const Node &node) {
    std::size_t seed = std::hash<int>{}(static_cast<int>(node.kind));

    switch (node.kind) {
    case Kind::Integer:
        hash_combine(seed, std::hash<Integer>{}(node.integer()));
        break;
    case Kind::Rational:
        hash_combine(seed, std::hash<Integer>{}(node.fraction().numerator));
        hash_combine(seed, std::hash<Integer>{}(node.fraction().denominator));
        break;
    case Kind::Real:
        // 0.0 == -0.0, so the two must hash alike, and MSVC's std::hash<double>
        // hashes the bit pattern, which differs. (NaN, the other value whose
        // equality and bits disagree, is refused by Expr::real.)
        hash_combine(seed,
                     std::hash<double>{}(node.real() == 0.0 ? 0.0 : node.real()));
        break;
    case Kind::Symbol:
    case Kind::Opaque:
    case Kind::Function:
        hash_combine(seed, std::hash<std::string>{}(node.text()));
        break;
    case Kind::Relation:
        hash_combine(seed, std::hash<int>{}(static_cast<int>(node.rel_op)));
        break;
    case Kind::Add:
    case Kind::Mul:
    case Kind::Pow:
        break;
    }

    // Order-sensitive, which is correct: these are structural hashes, and the
    // normaliser (docs/design.md step 10) is what makes x+1 and 1+x agree by putting
    // their operands in a canonical order first.
    for (const Expr &arg : node.args()) {
        hash_combine(seed, arg.hash());
    }
    return seed;
}

Expr finish(Node node) {
    node.hash = hash_of(node);
    return detail::make_expr(std::make_shared<const Node>(std::move(node)));
}

[[noreturn]] void wrong_kind(const char *wanted, Kind actual) {
    throw Error(std::string("expression is not ") + wanted + " (its kind is "
                + std::string(kind_name(actual)) + ")");
}

} // namespace

// --- construction ---------------------------------------------------------

Expr Expr::make_integer(Integer value) {
    Node node;
    node.kind = Kind::Integer;
    node.payload = std::move(value);
    return finish(std::move(node));
}

Expr::Expr() : Expr(make_integer(0)) {}

Expr Expr::integer(Integer value) {
    return make_integer(std::move(value));
}

Expr Expr::rational(Integer numerator, Integer denominator) {
    if (denominator.is_zero()) {
        throw Error("rational with zero denominator");
    }

    // No overflow cases to guard: proxima::Integer is unbounded, so reduction is
    // simply reduction. This used to need three special cases for the extreme
    // negative value alone.
    const Integer divisor = gcd(numerator, denominator);
    if (!divisor.is_zero()) {
        numerator = numerator / divisor;
        denominator = denominator / divisor;
    }
    if (denominator.is_negative()) {
        numerator = -numerator;
        denominator = -denominator;
    }
    if (denominator == Integer(1)) {
        return make_integer(std::move(numerator));
    }

    Node node;
    node.kind = Kind::Rational;
    node.payload = Fraction{std::move(numerator), std::move(denominator)};
    return finish(std::move(node));
}

Expr Expr::real(double value) {
    if (std::isnan(value)) {
        // Refused here, once, rather than handled everywhere a Real is looked
        // at: a NaN is not equal to itself, which broke both the equality/hash
        // contract and the strict weak ordering the normaliser sorts by —
        // undefined behaviour in std::sort, not merely a wrong order. It also
        // has no meaning in Maxima and printed as `nan`, which reads back as a
        // symbol.
        throw Error("a Real cannot be NaN: it has no value in Maxima and no place "
                    "in an ordering");
    }
    if (std::isinf(value)) {
        // Maxima has no floating-point infinity, only the symbols inf and minf,
        // which is what a Real infinity was sent as and printed as. Found by
        // fuzzing: printed, it read back as the symbol, a different expression.
        // So it is the symbol from the start.
        return symbol(value > 0 ? "inf" : "minf");
    }
    Node node;
    node.kind = Kind::Real;
    node.payload = value;
    return finish(std::move(node));
}

Expr Expr::symbol(std::string name) {
    Node node;
    node.kind = Kind::Symbol;
    node.payload = std::move(name);
    return finish(std::move(node));
}

Expr Expr::function(std::string head, std::vector<Expr> args) {
    Node node;
    node.kind = Kind::Function;
    node.payload = Application{std::move(head), std::move(args)};
    return finish(std::move(node));
}

Expr Expr::relation(RelOp op, Expr lhs, Expr rhs) {
    Node node;
    node.kind = Kind::Relation;
    node.rel_op = op;
    node.payload = std::vector<Expr>{std::move(lhs), std::move(rhs)};
    return finish(std::move(node));
}

Expr Expr::opaque(std::string text) {
    Node node;
    node.kind = Kind::Opaque;
    node.payload = std::move(text);
    return finish(std::move(node));
}

Expr Expr::add(std::vector<Expr> terms) {
    // Normalised at construction, so every Expr in existence is in canonical
    // form and equality never has to re-derive it.
    terms = detail::normalize_sum(std::move(terms));
    if (terms.empty()) {
        return integer(0);
    }
    if (terms.size() == 1) {
        return std::move(terms.front());
    }
    Node node;
    node.kind = Kind::Add;
    node.payload = std::move(terms);
    return finish(std::move(node));
}

Expr Expr::mul(std::vector<Expr> factors) {
    factors = detail::normalize_product(std::move(factors));
    if (factors.empty()) {
        return integer(1);
    }
    if (factors.size() == 1) {
        return std::move(factors.front());
    }
    Node node;
    node.kind = Kind::Mul;
    node.payload = std::move(factors);
    return finish(std::move(node));
}

Expr Expr::pow(Expr base, Expr exponent) {
    if (auto simplified = detail::normalize_power(base, exponent)) {
        return *simplified;
    }
    Node node;
    node.kind = Kind::Pow;
    node.payload = std::vector<Expr>{std::move(base), std::move(exponent)};
    return finish(std::move(node));
}

// --- inspection -----------------------------------------------------------

Kind Expr::kind() const {
    return node_->kind;
}

bool Expr::is_number() const {
    const Kind k = node_->kind;
    return k == Kind::Integer || k == Kind::Rational || k == Kind::Real;
}

bool Expr::is_negative_number() const {
    switch (node_->kind) {
    case Kind::Integer:
        return node_->integer().is_negative();
    case Kind::Rational:
        return node_->fraction().numerator.is_negative(); // Denominator is positive.
    case Kind::Real:
        return node_->real() < 0.0;
    default:
        return false;
    }
}

const Integer &Expr::integer_ref() const {
    return node_->integer();
}
const Fraction &Expr::fraction_ref() const {
    return node_->fraction();
}
double Expr::real_ref() const {
    return node_->real();
}
const std::string &Expr::text_ref() const {
    return node_->text();
}
RelOp Expr::relation_ref() const {
    return node_->rel_op;
}

std::optional<Integer> Expr::as_integer() const {
    if (node_->kind != Kind::Integer) {
        return std::nullopt;
    }
    return node_->integer();
}

std::optional<Fraction> Expr::as_fraction() const {
    if (node_->kind == Kind::Integer) {
        return Fraction{node_->integer(), Integer(1)};
    }
    if (node_->kind == Kind::Rational) {
        return node_->fraction();
    }
    return std::nullopt;
}

std::optional<double> Expr::as_real() const {
    if (node_->kind != Kind::Real) {
        return std::nullopt;
    }
    return node_->real();
}

std::optional<Symbol> Expr::as_symbol() const {
    if (node_->kind != Kind::Symbol) {
        return std::nullopt;
    }
    return Symbol(node_->text());
}

Integer Expr::integer_value() const {
    if (node_->kind != Kind::Integer) {
        wrong_kind("an integer", node_->kind);
    }
    return node_->integer();
}

Integer Expr::numerator() const {
    if (node_->kind == Kind::Integer) {
        return node_->integer();
    }
    if (node_->kind != Kind::Rational) {
        wrong_kind("a rational", node_->kind);
    }
    return node_->fraction().numerator;
}

Integer Expr::denominator() const {
    if (node_->kind == Kind::Integer) {
        return Integer(1);
    }
    if (node_->kind != Kind::Rational) {
        wrong_kind("a rational", node_->kind);
    }
    return node_->fraction().denominator;
}

double Expr::real_value() const {
    if (node_->kind != Kind::Real) {
        wrong_kind("a real", node_->kind);
    }
    return node_->real();
}

const std::string &Expr::name() const {
    if (node_->kind != Kind::Symbol && node_->kind != Kind::Function) {
        wrong_kind("a symbol or function", node_->kind);
    }
    return node_->text();
}

RelOp Expr::relation_op() const {
    if (node_->kind != Kind::Relation) {
        wrong_kind("a relation", node_->kind);
    }
    return node_->rel_op;
}

const std::string &Expr::opaque_text() const {
    if (node_->kind != Kind::Opaque) {
        wrong_kind("opaque", node_->kind);
    }
    return node_->text();
}

const std::vector<Expr> &Expr::args() const {
    return node_->args();
}

std::size_t Expr::arity() const {
    return node_->args().size();
}

const Expr &Expr::arg(std::size_t index) const {
    const std::vector<Expr> &operands = node_->args();
    if (index >= operands.size()) {
        throw Error("expression has no operand " + std::to_string(index)
                    + " (it has " + std::to_string(operands.size()) + ")");
    }
    return operands[index];
}

std::size_t Expr::hash() const {
    return node_->hash;
}

bool detail::same_representation(const Expr &lhs, const Expr &rhs) noexcept {
    return lhs.node_ == rhs.node_;
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
        return a.integer() == b.integer();
    case Kind::Rational:
        return a.fraction().numerator == b.fraction().numerator
               && a.fraction().denominator == b.fraction().denominator;
    case Kind::Real:
        return a.real() == b.real();
    case Kind::Symbol:
    case Kind::Opaque:
        return a.text() == b.text();
    case Kind::Function:
        return a.text() == b.text() && a.args() == b.args();
    case Kind::Relation:
        return a.rel_op == b.rel_op && a.args() == b.args();
    case Kind::Add:
    case Kind::Mul:
    case Kind::Pow:
        return a.args() == b.args();
    }
    return false;
}

std::weak_ordering canonical_order(const Expr &lhs, const Expr &rhs) {
    const int order = detail::compare_expr(lhs, rhs);
    if (order < 0) {
        return std::weak_ordering::less;
    }
    return order > 0 ? std::weak_ordering::greater : std::weak_ordering::equivalent;
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
    // Dividing by a number multiplies by its reciprocal, which keeps exact
    // division exact (`1/3` is a Rational, not `1*3^-1`) and puts the result in
    // the same shape Maxima uses: it returns x^3/3 as (1/3)*x^3, a numeric
    // coefficient, not a negative power. Without this the two spell the same
    // value differently and never compare equal.
    if (rhs.is(Kind::Integer) || rhs.is(Kind::Rational)) {
        if (!rhs.numerator().is_zero()) {
            return lhs * Expr::rational(rhs.denominator(), rhs.numerator());
        }
    } else if (rhs.is(Kind::Real) && rhs.real_value() != 0.0
               && std::isfinite(1.0 / rhs.real_value())) {
        // Not when the reciprocal overflows, as dividing by 1e-320 does: that
        // stays a negative power below, rather than becoming infinity.
        return lhs * Expr::real(1.0 / rhs.real_value());
    }
    // Anything else, including division by zero, becomes a negative power —
    // which is also how Maxima represents it, and lets Maxima be the one to
    // object to dividing by zero.
    return Expr::mul({lhs, Expr::pow(rhs, Expr::integer(-1))});
}

Expr operator-(const Expr &operand) {
    // Negating a literal gives a literal, so `-x + 1` does not print as
    // `-1*x + 1`. Everything else becomes a product with -1, which is how
    // Maxima represents negation internally too.
    switch (operand.kind()) {
    case Kind::Integer:
        return Expr::integer(-operand.integer_value());
    case Kind::Rational:
        return Expr::rational(-operand.numerator(), operand.denominator());
    case Kind::Real:
        return Expr::real(-operand.real_value());
    default:
        break;
    }
    return Expr::mul({Expr::integer(-1), operand});
}

Expr operator+(const Expr &operand) {
    return operand;
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

Expr lhs(const Expr &relation) {
    if (!relation.is(Kind::Relation)) {
        throw Error("lhs: " + relation.str() + " is not a relation");
    }
    return relation.arg(0);
}

Expr rhs(const Expr &relation) {
    if (!relation.is(Kind::Relation)) {
        throw Error("rhs: " + relation.str() + " is not a relation");
    }
    return relation.arg(1);
}

std::string_view kind_name(Kind kind) {
    switch (kind) {
    case Kind::Integer:
        return "Integer";
    case Kind::Rational:
        return "Rational";
    case Kind::Real:
        return "Real";
    case Kind::Symbol:
        return "Symbol";
    case Kind::Add:
        return "Add";
    case Kind::Mul:
        return "Mul";
    case Kind::Pow:
        return "Pow";
    case Kind::Function:
        return "Function";
    case Kind::Relation:
        return "Relation";
    case Kind::Opaque:
        return "Opaque";
    }
    // Not a Kind this library defines: a value cast in from outside.
    return "?";
}

std::string_view symbol_for(RelOp op) {
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

} // namespace proxima
