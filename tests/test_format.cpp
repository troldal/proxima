// Printing expressions: operator<< and std::format, including the format spec
// that picks a notation.

#include <doctest/doctest.h>

#include <proxima/errors.hpp>
#include <proxima/expr.hpp>
#include <proxima/integer.hpp>
#include <proxima/mathml.hpp>
#include <proxima/symbol.hpp>
#include <proxima/tex.hpp>

#include <format>
#include <sstream>
#include <string>

using proxima::Expr;
using proxima::Symbol;

TEST_CASE("an expression prints with << and with std::format") {
    const Symbol x("x");
    const Expr e = (Expr(x) + 1) / (Expr(x) - 1);

    std::ostringstream out;
    out << e;
    CHECK(out.str() == e.str());
    CHECK(std::format("{}", e) == e.str());

    SUBCASE("a format spec picks the notation") {
        CHECK(std::format("{:tex}", e) == proxima::to_tex(e));
        CHECK(std::format("{:mathml}", e) == proxima::to_mathml(e));
    }

    SUBCASE("and the usual width and alignment still apply, with or without one") {
        const std::string plain = e.str();
        REQUIRE(plain.size() < 40);
        CHECK(std::format("{:>40}", e)
              == std::string(40 - plain.size(), ' ') + plain);

        const std::string tex = proxima::to_tex(e);
        REQUIRE(tex.size() < 40);
        CHECK(std::format("{:tex:*<40}", e)
              == tex + std::string(40 - tex.size(), '*'));
    }

    SUBCASE("an unknown notation is an error, not plain text") {
        // A runtime format string: a constant one would not compile at all.
        const std::string spec = "{:latex}";
        CHECK_THROWS_AS(
            static_cast<void>(std::vformat(spec, std::make_format_args(e))),
            std::format_error);
    }
}

TEST_CASE("a kind prints by name") {
    using proxima::Kind;
    CHECK(proxima::kind_name(Kind::Integer) == "Integer");
    CHECK(proxima::kind_name(Kind::Rational) == "Rational");
    CHECK(proxima::kind_name(Kind::Real) == "Real");
    CHECK(proxima::kind_name(Kind::Symbol) == "Symbol");
    CHECK(proxima::kind_name(Kind::Add) == "Add");
    CHECK(proxima::kind_name(Kind::Mul) == "Mul");
    CHECK(proxima::kind_name(Kind::Pow) == "Pow");
    CHECK(proxima::kind_name(Kind::Function) == "Function");
    CHECK(proxima::kind_name(Kind::Relation) == "Relation");
    CHECK(proxima::kind_name(Kind::Opaque) == "Opaque");

    CHECK(std::format("{}", Kind::Pow) == "Pow");
    CHECK(std::format("{:>8}", Kind::Pow) == "     Pow");
    std::ostringstream out;
    out << Kind::Relation;
    CHECK(out.str() == "Relation");

    SUBCASE("which is what an accessor on the wrong kind reports") {
        // It used to say "(kind 3)".
        CHECK_THROWS_WITH_AS(static_cast<void>(Expr::symbol("x").integer_value()),
                             "expression is not an integer (its kind is Symbol)",
                             proxima::Error);
    }
}

TEST_CASE("symbols and integers print too") {
    const Symbol pi("%pi");
    std::ostringstream out;
    out << pi;
    CHECK(out.str() == Expr(pi).str());
    CHECK(std::format("{:tex}", pi) == proxima::to_tex(pi));

    const proxima::Integer factorial30("265252859812191058636308480000000");
    std::ostringstream digits;
    digits << factorial30;
    CHECK(digits.str() == "265252859812191058636308480000000");
    CHECK(std::format("[{:>36}]", factorial30)
          == "[   265252859812191058636308480000000]");
}
