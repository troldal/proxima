// Printing expressions: operator<< and std::format, including the format spec
// that picks a notation.

#include <doctest/doctest.h>

#include <mx/expr.hpp>
#include <mx/integer.hpp>
#include <mx/mathml.hpp>
#include <mx/symbol.hpp>
#include <mx/tex.hpp>

#include <format>
#include <sstream>
#include <string>

using mx::Expr;
using mx::Symbol;

TEST_CASE("an expression prints with << and with std::format") {
    const Symbol x("x");
    const Expr e = (Expr(x) + 1) / (Expr(x) - 1);

    std::ostringstream out;
    out << e;
    CHECK(out.str() == e.str());
    CHECK(std::format("{}", e) == e.str());

    SUBCASE("a format spec picks the notation") {
        CHECK(std::format("{:tex}", e) == mx::toTeX(e));
        CHECK(std::format("{:mathml}", e) == mx::toMathML(e));
    }

    SUBCASE("and the usual width and alignment still apply, with or without one") {
        const std::string plain = e.str();
        REQUIRE(plain.size() < 40);
        CHECK(std::format("{:>40}", e) == std::string(40 - plain.size(), ' ') + plain);

        const std::string tex = mx::toTeX(e);
        REQUIRE(tex.size() < 40);
        CHECK(std::format("{:tex:*<40}", e) == tex + std::string(40 - tex.size(), '*'));
    }

    SUBCASE("an unknown notation is an error, not plain text") {
        // A runtime format string: a constant one would not compile at all.
        const std::string spec = "{:latex}";
        CHECK_THROWS_AS(static_cast<void>(std::vformat(spec, std::make_format_args(e))),
                        std::format_error);
    }
}

TEST_CASE("symbols and integers print too") {
    const Symbol pi("%pi");
    std::ostringstream out;
    out << pi;
    CHECK(out.str() == Expr(pi).str());
    CHECK(std::format("{:tex}", pi) == mx::toTeX(pi));

    const mx::Integer factorial30("265252859812191058636308480000000");
    std::ostringstream digits;
    digits << factorial30;
    CHECK(digits.str() == "265252859812191058636308480000000");
    CHECK(std::format("[{:>36}]", factorial30) == "[   265252859812191058636308480000000]");
}
