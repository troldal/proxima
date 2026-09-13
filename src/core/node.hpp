#pragma once

#include <mx/expr.hpp>

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

namespace mx::detail {

/// A Rational's value: reduced, with the sign on the numerator and a
/// denominator that is always positive and coprime with it.
struct Fraction {
    Integer numerator;
    Integer denominator;
};

/// A Function's head and arguments.
struct Application {
    std::string head;
    std::vector<Expr> args;
};

/// The shared, immutable representation behind an Expr.
///
/// A kind tag beside a std::variant holding only what that kind needs:
///
///     Integer                      Integer
///     Rational                     Fraction
///     Real                         double
///     Symbol, Opaque               std::string   (the name, or the text)
///     Add, Mul, Pow, Relation      std::vector<Expr>
///     Function                     Application
///
/// It used to be one struct with a field for every kind, so every node — every
/// symbol, every sin(x) — carried two Integers, a double, a string and an
/// operand vector: 160 bytes. Now a node is the kind, the relation operator, the
/// hash and the largest alternative.
///
/// Reading a tree is still a switch on the kind, which is the thing this library
/// does constantly. The variant is only reached through the accessors below,
/// each of which knows which alternative the kind implies; nothing visits it.
/// None of this is visible through the public API, which never exposes Node.
struct Node {
    Kind kind = Kind::Integer;

    /// Meaningful for a Relation only.
    RelOp relOp = RelOp::Equal;

    /// Computed once at construction. Consistent with Expr::operator==.
    std::size_t hash = 0;

    std::variant<Integer, Fraction, double, std::string, std::vector<Expr>, Application>
        payload;

    const Integer &integer() const { return std::get<Integer>(payload); }
    const Fraction &fraction() const { return std::get<Fraction>(payload); }
    double real() const { return std::get<double>(payload); }

    /// A Symbol's name, an Opaque node's text, or a Function's head.
    const std::string &text() const {
        if (const auto *application = std::get_if<Application>(&payload)) {
            return application->head;
        }
        return std::get<std::string>(payload);
    }

    /// Operands, in the order the kind implies; empty for a leaf.
    const std::vector<Expr> &args() const {
        if (const auto *operands = std::get_if<std::vector<Expr>>(&payload)) {
            return *operands;
        }
        if (const auto *application = std::get_if<Application>(&payload)) {
            return application->args;
        }
        static const std::vector<Expr> none;
        return none;
    }
};

} // namespace mx::detail
