#pragma once

#include <proxima/expr.hpp>
#include <proxima/symbol.hpp>

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

namespace proxima {

/// A property a symbol can be declared to have.
///
/// Maxima's `declare`. Distinct from an assumption: an assumption is a relation
/// that happens to hold (`x > 0`), a declaration is a standing property of the
/// symbol itself (`n` is an integer).
///
/// Exactly the entries of Maxima's `features` list, and no others: anything
/// else is refused by Maxima ("declare: unknown property"). There is no
/// `Prime`, because Maxima has no such feature — `primep` tests a number, it
/// is not something a symbol can be declared to be.
enum class Feature {
    // Of a value.
    Integer,
    NonInteger,
    Even,
    Odd,
    Rational,
    Irrational,
    Real,
    Imaginary,
    Complex,
    Constant,
    // Of a function.
    Analytic,
    Increasing,
    Decreasing,
    OddFun,
    EvenFun,
    PosFun,
    IntegerValued,
    // Of an operator.
    Commutative,
    LAssociative,
    RAssociative,
    Symmetric,
    AntiSymmetric, // keep last: the tests walk every enumerator up to this one
};

/// Maxima's spelling of `feature`.
std::string_view name_of(Feature feature);

/// `symbol` declared to have `feature`.
struct Declaration {
    Symbol symbol;
    Feature feature;

    bool operator==(const Declaration &) const = default;
};

/// What an operation may take for granted: facts that hold, and properties
/// symbols are declared to have.
///
/// A value, not a scope. It is passed to the operations that need it —
///
///     const auto positive = proxima::assuming(gt(n, 0));
///     proxima::integrate(pow(x, n), x, positive);        // x^(n + 1)/(n + 1)
///     proxima::integrate(pow(x, n), x);                  // needs an assumption
///
/// — so the meaning of a call is a function of its arguments alone, on every
/// thread, whatever else is alive. The same value names the same Maxima
/// context each time it is used, and keys the caches, so asking again under
/// the same assumptions is answered from memory.
///
/// Immutable and canonical: the facts and declarations are kept sorted and
/// without duplicates, so two values that say the same things in a different
/// order are equal, hash alike, and share their Maxima context and cached
/// answers. Adding to one makes another: see with().
///
/// Whether the facts are consistent is Maxima's to say, when the assumptions
/// are first used: an operation under contradictory assumptions fails with
/// Cause::Inconsistent. A redundant fact is accepted quietly.
class Assumptions {
public:
    /// Nothing assumed.
    Assumptions() = default;

    /// These assumptions and one more fact, a relation such as `gt(x, 0)`.
    [[nodiscard]] Assumptions with(Expr fact) const;

    /// These assumptions and one more declaration.
    [[nodiscard]] Assumptions with(const Symbol &symbol, Feature feature) const;

    /// These assumptions and all of `more`'s.
    [[nodiscard]] Assumptions with(const Assumptions &more) const;

    /// The facts, in canonical order.
    std::span<const Expr> facts() const { return facts_; }

    /// The declarations, ordered by symbol name and then feature.
    std::span<const Declaration> declarations() const { return declarations_; }

    bool empty() const { return facts_.empty() && declarations_.empty(); }

    /// Consistent with operator==.
    std::size_t hash() const { return hash_; }

    bool operator==(const Assumptions &other) const {
        return hash_ == other.hash_ && facts_ == other.facts_
               && declarations_ == other.declarations_;
    }

private:
    /// Sorts, removes duplicates and rehashes: the one place the canonical
    /// form is made.
    void normalise();

    std::vector<Expr> facts_;
    std::vector<Declaration> declarations_;
    std::size_t hash_ = 0;
};

/// Assumptions holding `fact`: `assuming(gt(x, 0))`.
Assumptions assuming(Expr fact);

/// Assumptions holding every one of `facts`: `assuming({gt(x, 0), lt(x, 1)})`.
Assumptions assuming(std::initializer_list<Expr> facts);

/// Assumptions declaring `symbol` to have `feature`: `declaring(n,
/// Feature::Integer)`.
Assumptions declaring(const Symbol &symbol, Feature feature);

} // namespace proxima

template <>
struct std::hash<proxima::Assumptions> {
    std::size_t operator()(const proxima::Assumptions &value) const noexcept {
        return value.hash();
    }
};
