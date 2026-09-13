// The arbitrary-precision integer. Written by hand rather than taken from a
// multiprecision library, so it is tested harder than anything else here — and
// against Maxima, which is as good an oracle as exists.

#include <doctest/doctest.h>

#include <mx/errors.hpp>
#include <mx/integer.hpp>
#include <mx/kernel.hpp>
#include <mx/ops.hpp>

#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

using mx::Integer;

// bool and the character types are integral to C++ but not numbers. Integer
// already refused bool and char, but not the wide character types, so
// Integer(u'7') compiled and quietly meant 55. The fixed-width types spelled
// with signed char and unsigned char are numbers, and must stay convertible.
static_assert(!std::is_constructible_v<Integer, bool>);
static_assert(!std::is_constructible_v<Integer, char>);
static_assert(!std::is_constructible_v<Integer, wchar_t>);
static_assert(!std::is_constructible_v<Integer, char8_t>);
static_assert(!std::is_constructible_v<Integer, char16_t>);
static_assert(!std::is_constructible_v<Integer, char32_t>);
static_assert(std::is_convertible_v<std::int8_t, Integer>);
static_assert(std::is_convertible_v<std::uint8_t, Integer>);
static_assert(std::is_convertible_v<long long, Integer>);
static_assert(std::is_convertible_v<unsigned long long, Integer>);

namespace {

const std::string kFactorial30 = "265252859812191058636308480000000";
const std::string kMaxInt64 = "9223372036854775807";
const std::string kMinInt64 = "-9223372036854775808";

} // namespace

TEST_CASE("small values do not allocate") {
    // The fast path matters: every expression holds integers, and an
    // allocation per literal would slow the whole library down.
    CHECK(Integer(0).isSmall());
    CHECK(Integer(-1).isSmall());
    CHECK(Integer(kMaxInt64).isSmall());
    CHECK(Integer(kMinInt64).isSmall());
    CHECK_FALSE(Integer(kFactorial30).isSmall());

    SUBCASE("and a value that shrinks back returns to the fast path") {
        const Integer big(kFactorial30);
        CHECK((big - big).isSmall());
        CHECK((big / big).isSmall());
    }
}

TEST_CASE("round-tripping through decimal") {
    for (const std::string &text : {std::string("0"), std::string("1"),
                                    std::string("-1"), kMaxInt64, kMinInt64,
                                    kFactorial30, std::string("-") + kFactorial30}) {
        CAPTURE(text);
        CHECK(Integer(text).toString() == text);
    }

    SUBCASE("a leading plus is accepted and not echoed") {
        CHECK(Integer("+42").toString() == "42");
    }
    SUBCASE("malformed text is refused") {
        CHECK_FALSE(Integer::parse("").has_value());
        CHECK_FALSE(Integer::parse("-").has_value());
        CHECK_FALSE(Integer::parse("12a").has_value());
        CHECK_FALSE(Integer::parse(" 1").has_value());
        CHECK_THROWS_AS(Integer("nonsense"), mx::ParseError);
    }
}

TEST_CASE("arithmetic that stays small") {
    CHECK((Integer(2) + Integer(3)) == Integer(5));
    CHECK((Integer(2) - Integer(5)) == Integer(-3));
    CHECK((Integer(6) * Integer(7)) == Integer(42));
    CHECK((Integer(7) / Integer(2)) == Integer(3));
    CHECK((Integer(-7) / Integer(2)) == Integer(-3)); // Truncating, as C++.
    CHECK((Integer(7) % Integer(2)) == Integer(1));
    CHECK((Integer(-7) % Integer(2)) == Integer(-1)); // Sign of the dividend.
    CHECK(-Integer(5) == Integer(-5));
}

TEST_CASE("arithmetic that grows past 64 bits") {
    const Integer max(kMaxInt64);

    CHECK((max + max).toString() == "18446744073709551614");
    CHECK((max * max).toString() == "85070591730234615847396907784232501249");
    CHECK((Integer(kFactorial30) * Integer(2)).toString()
          == "530505719624382117272616960000000");

    SUBCASE("and shrinks back again exactly") {
        CHECK(((max + max) - max) == max);
        CHECK(((max * max) / max) == max);
        CHECK(((max * max) % max) == Integer(0));
    }
}

TEST_CASE("the extreme negative value needs no special case") {
    // Negating it overflows in 64 bits, which used to force a fallback.
    const Integer min(kMinInt64);
    CHECK((-min).toString() == "9223372036854775808");
    CHECK((min / Integer(-1)).toString() == "9223372036854775808");
    CHECK(mx::abs(min).toString() == "9223372036854775808");
    CHECK((min - Integer(1)).toString() == "-9223372036854775809");
}

TEST_CASE("signs follow through every combination") {
    const std::vector<long long> values{-97, -12, -1, 0, 1, 12, 97};
    for (const long long a : values) {
        for (const long long b : values) {
            CAPTURE(a);
            CAPTURE(b);
            CHECK((Integer(a) + Integer(b)) == Integer(a + b));
            CHECK((Integer(a) - Integer(b)) == Integer(a - b));
            CHECK((Integer(a) * Integer(b)) == Integer(a * b));
            if (b != 0) {
                CHECK((Integer(a) / Integer(b)) == Integer(a / b));
                CHECK((Integer(a) % Integer(b)) == Integer(a % b));
            }
        }
    }
}

TEST_CASE("comparison orders correctly across the boundary") {
    const Integer big(kFactorial30);
    const Integer max(kMaxInt64);

    CHECK(max < big);
    CHECK(-big < max);
    CHECK(-big < -max);
    CHECK(big > Integer(0));
    CHECK(-big < Integer(0));
    CHECK(Integer(kMaxInt64) == Integer(kMaxInt64));
    CHECK(big != max);

    SUBCASE("and a promoted value equals its small self") {
        // (big - big) takes the general path and lands back on zero.
        CHECK((big - big) == Integer(0));
        CHECK((big - big).hash() == Integer(0).hash());
    }
}

TEST_CASE("division by zero is refused") {
    CHECK_THROWS_AS(Integer(1) / Integer(0), mx::Error);
    CHECK_THROWS_AS(Integer(kFactorial30) % Integer(0), mx::Error);
}

TEST_CASE("gcd") {
    CHECK(gcd(Integer(12), Integer(18)) == Integer(6));
    CHECK(gcd(Integer(-12), Integer(18)) == Integer(6)); // Always non-negative.
    CHECK(gcd(Integer(0), Integer(5)) == Integer(5));
    CHECK(gcd(Integer(5), Integer(0)) == Integer(5));
    CHECK(gcd(Integer(0), Integer(0)) == Integer(0));
    CHECK(gcd(Integer(17), Integer(5)) == Integer(1));

    SUBCASE("on values past 64 bits") {
        const Integer big(kFactorial30);
        CHECK(gcd(big, big) == big);
        CHECK(gcd(big * Integer(7), big * Integer(5)) == big);
        // 30! is divisible by every integer up to 30.
        CHECK(gcd(big, Integer(29)) == Integer(29));
        // 31 is prime and larger, so it is not.
        CHECK(gcd(big, Integer(31)) == Integer(1));
    }
}

TEST_CASE("conversion out") {
    CHECK(Integer(42).toInt64() == 42);
    CHECK(Integer(kMinInt64).toInt64() == std::numeric_limits<std::int64_t>::min());
    CHECK_FALSE(Integer(kFactorial30).toInt64().has_value());

    CHECK(Integer(kFactorial30).toDouble()
          == doctest::Approx(2.6525285981219107e32));
    CHECK(Integer(-5).toDouble() == doctest::Approx(-5.0));
}

TEST_CASE("equal values hash equally however they are represented") {
    const Integer viaText(kMaxInt64);
    const Integer viaArithmetic
        = Integer(kFactorial30) / Integer(kFactorial30) * Integer(kMaxInt64);
    CHECK(viaText == viaArithmetic);
    CHECK(viaText.hash() == viaArithmetic.hash());
}

TEST_SUITE("maxima") {

TEST_CASE("arithmetic agrees with Maxima on large random values") {
    // Maxima has arbitrary-precision integers of its own and decades of use
    // behind them. Checking against it is far stronger than checking against
    // values written out by the same person who wrote the code.
    mx::Kernel kernel;
    std::mt19937_64 generator(20240117);

    const auto randomDigits = [&generator](int count) {
        std::uniform_int_distribution<int> digit(0, 9);
        std::string text(1, static_cast<char>('1' + digit(generator) % 9));
        for (int i = 1; i < count; ++i) {
            text.push_back(static_cast<char>('0' + digit(generator)));
        }
        return text;
    };

    const auto maximaSays = [&kernel](const std::string &expression) {
        const mx::Reply reply = kernel.evalPure(expression);
        REQUIRE(reply.ok);
        return reply.value;
    };

    for (int trial = 0; trial < 25; ++trial) {
        const std::string left = randomDigits(1 + trial * 2);
        const std::string right = randomDigits(1 + trial);
        const bool negateLeft = (trial % 3) == 0;
        const bool negateRight = (trial % 4) == 0;

        const std::string a = (negateLeft ? "-" : "") + left;
        const std::string b = (negateRight ? "-" : "") + right;
        CAPTURE(a);
        CAPTURE(b);

        const Integer x(a);
        const Integer y(b);

        CHECK((x + y).toString() == maximaSays(a + " + (" + b + ")"));
        CHECK((x - y).toString() == maximaSays(a + " - (" + b + ")"));
        CHECK((x * y).toString() == maximaSays(a + " * (" + b + ")"));
        // Maxima's quotient and remainder truncate toward zero, as these do.
        CHECK((x / y).toString() == maximaSays("truncate((" + a + ")/(" + b + "))"));
        CHECK((x % y).toString()
              == maximaSays("(" + a + ") - (" + b + ")*truncate((" + a + ")/("
                            + b + "))"));
        CHECK(gcd(x, y).toString() == maximaSays("gcd(" + a + ", " + b + ")"));
    }
}

TEST_CASE("a factorial from Maxima is a number here") {
    // The whole point of the widening: this used to come back as Opaque text
    // that could be printed and nothing else.
    mx::Kernel kernel;
    const auto value = mx::parse("30!", kernel);
    REQUIRE(value.has_value());
    REQUIRE(value->kind() == mx::Kind::Integer);

    // Arithmetic on it happens here, with no further round trip.
    const mx::Expr doubled = *value * mx::Expr(2);
    CHECK(doubled.str() == "530505719624382117272616960000000");

    // And Maxima agrees.
    const auto confirmed = kernel.evalPure("is(" + doubled.str() + " = 2*30!)");
    CHECK(confirmed.value == "T");
}

} // TEST_SUITE("maxima")
