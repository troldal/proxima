#include <proxima/assumptions.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <tuple>
#include <utility>

namespace proxima {
namespace {

std::size_t mix(std::size_t seed, std::size_t value) {
    // boost::hash_combine's mixing, spelled out to keep Boost out of here.
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

bool declaration_before(const Declaration &a, const Declaration &b) {
    return std::tie(a.symbol.name(), a.feature) < std::tie(b.symbol.name(), b.feature);
}

} // namespace

std::string_view name_of(Feature feature) {
    switch (feature) {
    case Feature::Integer:
        return "integer";
    case Feature::NonInteger:
        return "noninteger";
    case Feature::Even:
        return "even";
    case Feature::Odd:
        return "odd";
    case Feature::Rational:
        return "rational";
    case Feature::Irrational:
        return "irrational";
    case Feature::Real:
        return "real";
    case Feature::Imaginary:
        return "imaginary";
    case Feature::Complex:
        return "complex";
    case Feature::Constant:
        return "constant";
    case Feature::Analytic:
        return "analytic";
    case Feature::Increasing:
        return "increasing";
    case Feature::Decreasing:
        return "decreasing";
    case Feature::OddFun:
        return "oddfun";
    case Feature::EvenFun:
        return "evenfun";
    case Feature::PosFun:
        return "posfun";
    case Feature::IntegerValued:
        return "integervalued";
    case Feature::Commutative:
        return "commutative";
    case Feature::LAssociative:
        return "lassociative";
    case Feature::RAssociative:
        return "rassociative";
    case Feature::Symmetric:
        return "symmetric";
    case Feature::AntiSymmetric:
        return "antisymmetric";
    }
    return "real";
}

void Assumptions::normalise() {
    std::ranges::sort(facts_, CanonicalLess{});
    const auto [first, last] = std::ranges::unique(facts_);
    facts_.erase(first, last);

    std::ranges::sort(declarations_, declaration_before);
    const auto [dfirst, dlast] = std::ranges::unique(declarations_);
    declarations_.erase(dfirst, dlast);

    std::size_t hash = facts_.size();
    for (const Expr &fact : facts_) {
        hash = mix(hash, fact.hash());
    }
    hash = mix(hash, declarations_.size());
    for (const Declaration &declaration : declarations_) {
        hash = mix(hash, declaration.symbol.expr().hash());
        hash = mix(hash, static_cast<std::size_t>(declaration.feature));
    }
    hash_ = hash;
}

Assumptions Assumptions::with(Expr fact) const {
    Assumptions more = *this;
    more.facts_.push_back(std::move(fact));
    more.normalise();
    return more;
}

Assumptions Assumptions::with(const Symbol &symbol, Feature feature) const {
    Assumptions more = *this;
    more.declarations_.push_back({symbol, feature});
    more.normalise();
    return more;
}

Assumptions Assumptions::with(const Assumptions &more) const {
    Assumptions both = *this;
    both.facts_.insert(both.facts_.end(), more.facts_.begin(), more.facts_.end());
    both.declarations_.insert(both.declarations_.end(), more.declarations_.begin(),
                              more.declarations_.end());
    both.normalise();
    return both;
}

Assumptions assuming(Expr fact) {
    return Assumptions().with(std::move(fact));
}

Assumptions assuming(std::initializer_list<Expr> facts) {
    Assumptions all;
    for (const Expr &fact : facts) {
        all = all.with(fact);
    }
    return all;
}

Assumptions declaring(const Symbol &symbol, Feature feature) {
    return Assumptions().with(symbol, feature);
}

} // namespace proxima
