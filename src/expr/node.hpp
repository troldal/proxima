#pragma once

#include <proxima/expr.hpp>

#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace proxima::detail {

using proxima::Fraction;

// The alternatives a node can be, one per Kind. Each carries exactly what
// that kind needs, under the name the kind gives it, and nothing else: a
// symbol has a name, a relation has an operator and two sides, an integer
// has a value. The two-operand kinds take their operands through a
// constructor, so a power with three or a relation with one cannot be made.

/// Kind::Symbol. A separate type from Opaque, which is also a string, so that
/// the variant can tell them apart.
struct SymbolName {
    std::string name;
};

/// Kind::Opaque: Maxima source text this library does not model.
struct OpaqueText {
    std::string text;
};

/// Kind::Add. The terms are in canonical order; see normalize.cpp.
struct Sum {
    std::vector<Expr> terms;
};

/// Kind::Mul. The factors are in canonical order.
struct Product {
    std::vector<Expr> factors;
};

/// Kind::Pow: exactly a base and an exponent. Held as a vector because
/// Expr::args() hands the operands out as one; the constructor is the only
/// way in, which is what keeps them two.
class Power {
public:
    Power(Expr base, Expr exponent)
        : operands_{std::move(base), std::move(exponent)} {}
    const Expr &base() const { return operands_[0]; }
    const Expr &exponent() const { return operands_[1]; }
    const std::vector<Expr> &operands() const { return operands_; }

private:
    std::vector<Expr> operands_;
};

/// Kind::Function: a head and its arguments, however many.
struct Application {
    std::string head;
    std::vector<Expr> args;
};

/// Kind::Relation: an operator and exactly two sides, as Power holds two.
class RelationNode {
public:
    RelationNode(RelOp op, Expr lhs, Expr rhs)
        : op_(op), sides_{std::move(lhs), std::move(rhs)} {}
    RelOp op() const { return op_; }
    const Expr &lhs() const { return sides_[0]; }
    const Expr &rhs() const { return sides_[1]; }
    const std::vector<Expr> &sides() const { return sides_; }

private:
    RelOp op_;
    std::vector<Expr> sides_;
};

/// The shared, immutable representation behind an Expr.
///
/// The variant *is* the kind: there is one alternative per Kind, and kind()
/// is read off which one is held, so a node cannot claim one kind and hold
/// another's data. It used to be a kind tag beside a variant, set separately
/// by every builder, with the accessors doing std::get on trust.
///
/// Reading a tree is still a switch on the kind, which is the thing this
/// library does constantly: kind() is a switch on the index, and the
/// accessors below know which alternative each kind implies. None of this is
/// visible through the public API, which never exposes Node.
struct Node {
    using Payload = std::variant<Integer, Fraction, double, SymbolName, Sum, Product,
                                 Power, Application, RelationNode, OpaqueText>;

    /// The hash is computed by whoever builds the node, once, from the
    /// payload: see finish() in expr.cpp. Consistent with Expr::operator==.
    Node(Payload held, std::size_t hashed)
        : payload(std::move(held)), hash(hashed) {}

    Payload payload;
    std::size_t hash;

    Kind kind() const {
        // One case per alternative, in the variant's order. A missing case
        // is a compile error below, not a wrong answer.
        switch (payload.index()) {
        case 0:
            return Kind::Integer;
        case 1:
            return Kind::Rational;
        case 2:
            return Kind::Real;
        case 3:
            return Kind::Symbol;
        case 4:
            return Kind::Add;
        case 5:
            return Kind::Mul;
        case 6:
            return Kind::Pow;
        case 7:
            return Kind::Function;
        case 8:
            return Kind::Relation;
        case 9:
            return Kind::Opaque;
        default:
            break;
        }
        return Kind::Opaque; // Unreachable: a variant always holds one.
    }

    // Accessors for a node whose kind is already known. Each std::get throws
    // if the kind was not checked, as the old tag-and-variant did.
    const Integer &integer() const { return std::get<Integer>(payload); }
    const Fraction &fraction() const { return std::get<Fraction>(payload); }
    double real() const { return std::get<double>(payload); }
    const RelationNode &relation() const { return std::get<RelationNode>(payload); }

    /// A Symbol's name, an Opaque node's text, or a Function's head.
    const std::string &text() const {
        if (const auto *symbol = std::get_if<SymbolName>(&payload)) {
            return symbol->name;
        }
        if (const auto *opaque = std::get_if<OpaqueText>(&payload)) {
            return opaque->text;
        }
        return std::get<Application>(payload).head;
    }

    /// Operands, in the order the kind implies; empty for a leaf.
    const std::vector<Expr> &args() const {
        static const std::vector<Expr> none;
        return std::visit(
            [](const auto &alternative) -> const std::vector<Expr> & {
                using T = std::decay_t<decltype(alternative)>;
                if constexpr (std::is_same_v<T, Sum>) {
                    return alternative.terms;
                } else if constexpr (std::is_same_v<T, Product>) {
                    return alternative.factors;
                } else if constexpr (std::is_same_v<T, Power>) {
                    return alternative.operands();
                } else if constexpr (std::is_same_v<T, Application>) {
                    return alternative.args;
                } else if constexpr (std::is_same_v<T, RelationNode>) {
                    return alternative.sides();
                } else {
                    return none;
                }
            },
            payload);
    }
};

static_assert(std::variant_size_v<Node::Payload> == 10,
              "one alternative per Kind: update Node::kind() with the variant");

} // namespace proxima::detail
