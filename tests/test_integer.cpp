// The arbitrary-precision integer. Written by hand rather than taken from a
// multiprecision library, so it is tested harder than anything else here — and
// against Maxima, which is as good an oracle as exists.

#include <doctest/doctest.h>

#include <proxima/errors.hpp>
#include <proxima/integer.hpp>
#include <proxima/kernel.hpp>
#include <proxima/ops.hpp>

#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

using proxima::Integer;

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
    CHECK(Integer(0).is_small());
    CHECK(Integer(-1).is_small());
    CHECK(Integer(kMaxInt64).is_small());
    CHECK(Integer(kMinInt64).is_small());
    CHECK_FALSE(Integer(kFactorial30).is_small());

    SUBCASE("and a value that shrinks back returns to the fast path") {
        const Integer big(kFactorial30);
        CHECK((big - big).is_small());
        CHECK((big / big).is_small());
    }
}

TEST_CASE("round-tripping through decimal") {
    for (const std::string &text : {std::string("0"), std::string("1"),
                                    std::string("-1"), kMaxInt64, kMinInt64,
                                    kFactorial30, std::string("-") + kFactorial30}) {
        CAPTURE(text);
        CHECK(Integer(text).to_string() == text);
    }

    SUBCASE("a leading plus is accepted and not echoed") {
        CHECK(Integer("+42").to_string() == "42");
    }
    SUBCASE("malformed text is refused") {
        CHECK_FALSE(Integer::parse("").has_value());
        CHECK_FALSE(Integer::parse("-").has_value());
        CHECK_FALSE(Integer::parse("12a").has_value());
        CHECK_FALSE(Integer::parse(" 1").has_value());
        CHECK_THROWS_AS(Integer("nonsense"), proxima::ParseError);
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

    CHECK((max + max).to_string() == "18446744073709551614");
    CHECK((max * max).to_string() == "85070591730234615847396907784232501249");
    CHECK((Integer(kFactorial30) * Integer(2)).to_string()
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
    CHECK((-min).to_string() == "9223372036854775808");
    CHECK((min / Integer(-1)).to_string() == "9223372036854775808");
    CHECK(proxima::abs(min).to_string() == "9223372036854775808");
    CHECK((min - Integer(1)).to_string() == "-9223372036854775809");
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
    CHECK_THROWS_AS(Integer(1) / Integer(0), proxima::Error);
    CHECK_THROWS_AS(Integer(kFactorial30) % Integer(0), proxima::Error);
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

TEST_CASE("leading zeros are decimal, however long the literal") {
    // Found by fuzzing: past eighteen digits the text went to cpp_int, which
    // takes a leading 0 for an octal prefix. With an 8 or 9 in it that threw
    // an exception nothing here expected; without, the value was silently wrong.
    CHECK(Integer::parse("000000000000000000017") == Integer(17));
    CHECK(Integer::parse("-000000000000000000017") == Integer(-17));
    CHECK(Integer::parse("000000000000000000000") == Integer(0));
    REQUIRE(Integer::parse("00525285981219105863630848000000").has_value());
    CHECK(Integer::parse("00525285981219105863630848000000")->to_string()
          == "525285981219105863630848000000");
    REQUIRE(Integer::parse("0012345670123456701234567").has_value());
    CHECK(Integer::parse("0012345670123456701234567")->to_string()
          == "12345670123456701234567");
}

TEST_CASE("conversion out") {
    CHECK(Integer(42).to_int64() == 42);
    CHECK(Integer(kMinInt64).to_int64() == std::numeric_limits<std::int64_t>::min());
    CHECK_FALSE(Integer(kFactorial30).to_int64().has_value());

    CHECK(Integer(kFactorial30).to_double()
          == doctest::Approx(2.6525285981219107e32));
    CHECK(Integer(-5).to_double() == doctest::Approx(-5.0));
}

TEST_CASE("equal values hash equally however they are represented") {
    const Integer via_text(kMaxInt64);
    const Integer via_arithmetic
        = Integer(kFactorial30) / Integer(kFactorial30) * Integer(kMaxInt64);
    CHECK(via_text == via_arithmetic);
    CHECK(via_text.hash() == via_arithmetic.hash());
}

TEST_SUITE("maxima") {

TEST_CASE("arithmetic agrees with Maxima on large random values") {
    // Maxima has arbitrary-precision integers of its own and decades of use
    // behind them. Checking against it is far stronger than checking against
    // values written out by the same person who wrote the code.
    proxima::Kernel kernel;
    std::mt19937_64 generator(20240117);

    const auto random_digits = [&generator](int count) {
        std::uniform_int_distribution<int> digit(0, 9);
        std::string text(1, static_cast<char>('1' + digit(generator) % 9));
        for (int i = 1; i < count; ++i) {
            text.push_back(static_cast<char>('0' + digit(generator)));
        }
        return text;
    };

    const auto maxima_says = [&kernel](const std::string &expression) {
        const proxima::Reply reply = kernel.eval_pure(expression);
        REQUIRE(reply.ok);
        return reply.value;
    };

    for (int trial = 0; trial < 25; ++trial) {
        const std::string left = random_digits(1 + trial * 2);
        const std::string right = random_digits(1 + trial);
        const bool negate_left = (trial % 3) == 0;
        const bool negate_right = (trial % 4) == 0;

        const std::string a = (negate_left ? "-" : "") + left;
        const std::string b = (negate_right ? "-" : "") + right;
        CAPTURE(a);
        CAPTURE(b);

        const Integer x(a);
        const Integer y(b);

        CHECK((x + y).to_string() == maxima_says(a + " + (" + b + ")"));
        CHECK((x - y).to_string() == maxima_says(a + " - (" + b + ")"));
        CHECK((x * y).to_string() == maxima_says(a + " * (" + b + ")"));
        // Maxima's quotient and remainder truncate toward zero, as these do.
        CHECK((x / y).to_string() == maxima_says("truncate((" + a + ")/(" + b + "))"));
        CHECK((x % y).to_string()
              == maxima_says("(" + a + ") - (" + b + ")*truncate((" + a + ")/("
                            + b + "))"));
        CHECK(gcd(x, y).to_string() == maxima_says("gcd(" + a + ", " + b + ")"));
    }
}

TEST_CASE("a factorial from Maxima is a number here") {
    // The whole point of the widening: this used to come back as Opaque text
    // that could be printed and nothing else.
    proxima::Kernel kernel;
    const auto value = proxima::parse("30!", kernel);
    REQUIRE(value.has_value());
    REQUIRE(value->kind() == proxima::Kind::Integer);

    // Arithmetic on it happens here, with no further round trip.
    const proxima::Expr doubled = *value * proxima::Expr(2);
    CHECK(doubled.str() == "530505719624382117272616960000000");

    // And Maxima agrees.
    const auto confirmed = kernel.eval_pure("is(" + doubled.str() + " = 2*30!)");
    CHECK(confirmed.value == "T");
}

} // TEST_SUITE("maxima")
