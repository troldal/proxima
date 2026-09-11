// Protocol tests driven by a scripted transport. No Maxima installation, no
// child process — these are the tests step 4 exists to make possible.

#include <doctest/doctest.h>

#include "kernel/session.hpp"
#include "transport/child_process.hpp"
#include "transport/fake_transport.hpp"

#include <mx/errors.hpp>

#include <memory>
#include <string>
#include <vector>

using mx::detail::FakeTransport;
using mx::detail::MaximaSession;

namespace {

/// A wrapped prompt exactly as Maxima's *prompt-prefix*/*prompt-suffix* hooks
/// produce it.
std::string prompt(int n) {
    return std::string(MaximaSession::kPromptPrefix) + "(%i" + std::to_string(n)
           + ") " + MaximaSession::kPromptSuffix;
}

/// A result line followed by the next prompt.
std::string reply(int n, const std::string &text) {
    return "(%o" + std::to_string(n) + ") " + text + "\n" + prompt(n + 1);
}

/// The two responses every session consumes before it is usable: the startup
/// banner up to the first prompt, and the answer to `display2d:false$`.
std::vector<std::string> handshake() {
    return {"Maxima 5.50.0 https://maxima.sourceforge.io\n" + prompt(1),
            prompt(2)};
}

/// Builds a session over a scripted transport, keeping a borrowed pointer to
/// the transport so tests can inspect what was sent.
struct ScriptedSession {
    explicit ScriptedSession(std::vector<std::string> afterHandshake) {
        std::vector<std::string> script = handshake();
        script.insert(script.end(), afterHandshake.begin(), afterHandshake.end());

        auto owned = std::make_unique<FakeTransport>(std::move(script));
        transport = owned.get();
        session = std::make_unique<MaximaSession>(std::move(owned), mx::Config{});
    }

    FakeTransport *transport = nullptr;
    std::unique_ptr<MaximaSession> session;
};

} // namespace

TEST_CASE("handshake consumes the banner and disables 2-D display") {
    const ScriptedSession scripted({});

    // Exactly one statement should have gone out during construction, and it
    // must be the one that makes results machine-readable at all.
    REQUIRE(scripted.transport->sent().size() == 1);
    CHECK(scripted.transport->sent().front() == "display2d:false$\n");
}

TEST_CASE("a result line is stripped of its (%oN) label") {
    ScriptedSession scripted({reply(2, "2*x+3")});
    CHECK(scripted.session->evaluate("diff(x^2+3*x+2, x);") == "2*x+3");
}

TEST_CASE("the statement reaches the transport verbatim, newline-terminated") {
    ScriptedSession scripted({reply(2, "42")});
    scripted.session->evaluate("subst(5, x, x^2+3*x+2);");
    CHECK(scripted.transport->sent().back() == "subst(5, x, x^2+3*x+2);\n");
}

TEST_CASE("a statement producing no output yields an empty string") {
    // A `$`-terminated statement prints nothing between the two prompts.
    ScriptedSession scripted({prompt(3)});
    CHECK(scripted.session->evaluate("a: 7$") == "");
}

TEST_CASE("output split across several reads is reassembled") {
    // The transport hands back short reads; the session must accumulate until
    // it sees the prompt marker rather than assuming one read per reply.
    ScriptedSession scripted({"(%o2) 2*x*sin(x)", "+(2-x^2)*cos(x)\n", prompt(3)});
    CHECK(scripted.session->evaluate("integrate(x^2*sin(x), x);")
          == "2*x*sin(x)+(2-x^2)*cos(x)");
}

TEST_CASE("a multi-line result keeps its interior newlines") {
    ScriptedSession scripted({reply(2, "first\nsecond")});
    CHECK(scripted.session->evaluate("something;") == "first\nsecond");
}

TEST_CASE("text with no (%oN) label is passed through unchanged") {
    // Maxima error text arrives without a result label. Callers need to see it
    // rather than have it silently discarded.
    ScriptedSession scripted(
        {"integrate: variable must not be a number; found: 5\n" + prompt(3)});
    CHECK(scripted.session->evaluate("integrate(x, 5);")
          == "integrate: variable must not be a number; found: 5");
}

TEST_CASE("a session whose child has died reports a KernelError") {
    // The script runs out, so FakeTransport reports itself dead — the same
    // signal a real transport gives when the child exits.
    ScriptedSession scripted({});
    REQUIRE(scripted.transport->scriptExhausted());
    CHECK_THROWS_AS(scripted.session->evaluate("1+1;"), mx::KernelError);
}

TEST_CASE("a null transport is rejected rather than dereferenced") {
    CHECK_THROWS_AS(MaximaSession(nullptr, mx::Config{}), mx::KernelError);
}

