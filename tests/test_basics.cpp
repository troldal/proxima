// Tests that need no Maxima installation. Thin for now — there is very little
// to assert until the term layer lands (docs/design.md steps 7-9) — but they pin
// down two contracts that later steps rely on.

#include <doctest/doctest.h>

#include <proxima/config.hpp>
#include <proxima/errors.hpp>

#include <exception>
#include <string>

TEST_CASE("a default Config asks for discovery rather than a fixed path") {
    const proxima::Config config;
    // Empty is meaningful: it means "find Maxima yourself". A hard-coded
    // default would quietly work on the machine it was written on and nowhere
    // else.
    CHECK(config.maxima_root.empty());

    // A library must compute the same answer on every machine, so the user's
    // own maxima-init.mac is out of the picture unless asked for.
    CHECK_FALSE(config.load_user_init);
}

TEST_CASE("KernelError is catchable at every level of the hierarchy") {
    // docs/design.md step 11 splits failures two ways: infrastructure failures
    // throw, mathematical ones ("no closed form") come back as std::expected.
    // Callers therefore need to be able to catch proxima::Error without knowing
    // which concrete kernel failure occurred.
    SUBCASE("as its own type") {
        CHECK_THROWS_AS(throw proxima::KernelError("boom"), proxima::KernelError);
    }
    SUBCASE("as the library base") {
        CHECK_THROWS_AS(throw proxima::KernelError("boom"), proxima::Error);
    }
    SUBCASE("as std::exception") {
        CHECK_THROWS_AS(throw proxima::KernelError("boom"), std::exception);
    }
    SUBCASE("message survives") {
        try {
            throw proxima::KernelError("boom");
        } catch (const proxima::Error &e) {
            CHECK(std::string(e.what()) == "boom");
        }
    }
}
