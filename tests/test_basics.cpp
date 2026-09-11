// Tests that need no Maxima installation. Thin for now — there is very little
// to assert until the term layer lands (PLAN.md steps 7-9) — but they pin down
// two contracts that later steps rely on.

#include <doctest/doctest.h>

#include <mx/config.hpp>
#include <mx/errors.hpp>

#include <exception>
#include <string>

TEST_CASE("Config carries a usable default Maxima root") {
    const mx::Config config;
    CHECK_FALSE(config.maximaRoot.empty());
}

TEST_CASE("KernelError is catchable at every level of the hierarchy") {
    // PLAN.md step 11 splits failures two ways: infrastructure failures throw,
    // mathematical ones ("no closed form") come back as std::expected. Callers
    // therefore need to be able to catch mx::Error without knowing which
    // concrete kernel failure occurred.
    SUBCASE("as its own type") {
        CHECK_THROWS_AS(throw mx::KernelError("boom"), mx::KernelError);
    }
    SUBCASE("as the library base") {
        CHECK_THROWS_AS(throw mx::KernelError("boom"), mx::Error);
    }
    SUBCASE("as std::exception") {
        CHECK_THROWS_AS(throw mx::KernelError("boom"), std::exception);
    }
    SUBCASE("message survives") {
        try {
            throw mx::KernelError("boom");
        } catch (const mx::Error &e) {
            CHECK(std::string(e.what()) == "boom");
        }
    }
}
