// Protocol tests driven by a scripted transport. No Maxima installation, no
// child process — these are the tests step 4 exists to make possible, now
// covering the framed protocol introduced in step 6.

#include <doctest/doctest.h>

#include "kernel/session.hpp"
#include "transport/fake_transport.hpp"

#include <mx/errors.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using mx::detail::FakeTransport;
using mx::detail::MaximaSession;
using mx::detail::Payload;

namespace {

/// A complete reply frame, exactly as the Lisp helper formats one.
std::string frame(std::uint64_t id, bool ok, const std::string &value,
                  const std::string &reason = "") {
    return MaximaSession::frameBegin(id) + (ok ? "T" : "NIL")
           + MaximaSession::frameSeparator(id) + value
           + MaximaSession::frameSeparator(id) + reason
           + MaximaSession::frameEnd(id) + "\n";
}

/// The banner Maxima prints before anything else, plus the prompts that appear
/// between statements. All of it is noise the frame delimiters let us discard.
std::string noise(int promptNumber) {
    return "(%i" + std::to_string(promptNumber) + ") ";
}

/// The one response a session consumes during construction: the banner, then
/// the frame answering the handshake probe (request 1).
std::vector<std::string> handshakeScript() {
    return {"Maxima 5.50.0 https://maxima.sourceforge.io\n"
            "using Lisp SBCL 2.6.8\n"
            + noise(1) + noise(2) + noise(3) + noise(4)
            + frame(1, true, "$TRUE")};
}

/// Builds a session over a scripted transport, keeping a borrowed pointer to
/// the transport so tests can inspect what was sent.
struct ScriptedSession {
    explicit ScriptedSession(std::vector<std::string> afterHandshake) {
        std::vector<std::string> script = handshakeScript();
        script.insert(script.end(), afterHandshake.begin(), afterHandshake.end());

        auto owned = std::make_unique<FakeTransport>(std::move(script));
        transport = owned.get();
        session = std::make_unique<MaximaSession>(std::move(owned), mx::Config{});
    }

    FakeTransport *transport = nullptr;
    std::unique_ptr<MaximaSession> session;
};

} // namespace

TEST_CASE("the handshake makes the session machine-readable and deterministic") {
    const ScriptedSession scripted({});

    const std::string sent = scripted.transport->sentText();
    // Without these three the protocol does not work at all: display2d turns
    // off ASCII art, nolabels stops an unbounded leak of %i/%o labels, and
    // errormsg keeps error text out of the stream.
    CHECK(sent.find("display2d:false$") != std::string::npos);
    CHECK(sent.find("nolabels:true$") != std::string::npos);
    CHECK(sent.find("errormsg:false$") != std::string::npos);

    // And a framed probe, which is what synchronises the stream. A form, so
    // that the handshake needs nothing but the helper installed at launch.
    CHECK(sent.find(MaximaSession::requestFor(1, Payload::form("T")))
          != std::string::npos);
}

TEST_CASE("a request carries its own correlation id") {
    ScriptedSession scripted({frame(2, true, "((MPLUS SIMP) 1 $X)")});
    scripted.session->eval(Payload::text("x+1"));

    // Request 1 was the handshake probe, so the first real request is 2.
    CHECK(scripted.transport->sent().back()
          == MaximaSession::requestFor(2, Payload::text("x+1")) + "\n");
}

TEST_CASE("a successful reply yields the internal s-expression") {
    ScriptedSession scripted(
        {frame(2, true, "((MTIMES SIMP) 2 $X ((%SIN SIMP) $X))")});

    const mx::Reply reply = scripted.session->eval(Payload::text("2*x*sin(x)"));
    CHECK(reply.ok);
    CHECK(reply.value == "((MTIMES SIMP) 2 $X ((%SIN SIMP) $X))");
    CHECK(reply.reason.empty());
}

TEST_CASE("a Maxima error is a value, not an exception") {
    // Failing to integrate something is an ordinary outcome. Only
    // infrastructure failures throw.
    ScriptedSession scripted(
        {frame(2, false, "NIL",
               "integrate: variable must not be a number; found: 5")});

    const mx::Reply reply = scripted.session->eval(Payload::text("integrate(x, 5)"));
    CHECK_FALSE(reply.ok);
    CHECK(reply.reason == "integrate: variable must not be a number; found: 5");
    CHECK(reply.value.empty());
}

TEST_CASE("exact rationals survive the round trip") {
    // The whole reason for using the internal form rather than display output:
    // 1/3 stays a rational instead of becoming 0.333...
    ScriptedSession scripted({frame(2, true, "((RAT SIMP) 11 15)")});
    CHECK(scripted.session->eval(Payload::text("1/3 + 2/5")).value == "((RAT SIMP) 11 15)");
}

TEST_CASE("prompts and banner text between frames are discarded") {
    ScriptedSession scripted({noise(5) + "\n" + frame(2, true, "42")});
    CHECK(scripted.session->eval(Payload::text("6*7")).value == "42");
}

TEST_CASE("a reply split across several reads is reassembled") {
    // The transport hands back short reads; the session must accumulate until
    // the closing delimiter arrives rather than assume one read per reply.
    const std::string whole = frame(2, true, "((MPLUS SIMP) 1 $X)");
    const size_t third = whole.size() / 3;

    ScriptedSession scripted({whole.substr(0, third),
                              whole.substr(third, third),
                              whole.substr(2 * third)});
    CHECK(scripted.session->eval(Payload::text("x+1")).value == "((MPLUS SIMP) 1 $X)");
}

TEST_CASE("a delimiter split across two reads is still recognised") {
    // The nastiest reassembly case: the closing delimiter itself straddles a
    // read boundary, so neither half contains it.
    const std::string whole = frame(2, true, "7");
    const size_t cut = whole.size() - 4;

    ScriptedSession scripted({whole.substr(0, cut), whole.substr(cut)});
    CHECK(scripted.session->eval(Payload::text("7")).value == "7");
}

TEST_CASE("a stale frame from an earlier request is skipped") {
    // This is what the correlation id buys. Without it a leftover reply would
    // be returned as the answer to the wrong question — silently, and with a
    // perfectly plausible-looking value.
    ScriptedSession scripted({frame(1, true, "$STALE_ANSWER")
                              + frame(2, true, "$CORRECT_ANSWER")});

    CHECK(scripted.session->eval(Payload::text("something")).value == "$CORRECT_ANSWER");
}

TEST_CASE("a value containing delimiter-like text is not truncated") {
    // Maxima strings are printed readably, so a result can legitimately contain
    // text resembling a delimiter for *another* id. Only this request's own id
    // may terminate its frame.
    const std::string tricky = R"("contains @@E99@@ and @@B3@@ inside")";
    ScriptedSession scripted({frame(2, true, tricky)});
    CHECK(scripted.session->eval(Payload::text("\"...\"")).value == tricky);
}

TEST_CASE("a closing delimiter with no opening one is a protocol error") {
    ScriptedSession scripted({MaximaSession::frameEnd(2) + "\n"});
    CHECK_THROWS_AS(scripted.session->eval(Payload::text("x")), mx::KernelError);
}

TEST_CASE("a frame missing its field separators is a protocol error") {
    ScriptedSession scripted({MaximaSession::frameBegin(2) + "T"
                              + MaximaSession::frameEnd(2)});
    CHECK_THROWS_AS(scripted.session->eval(Payload::text("x")), mx::KernelError);
}

TEST_CASE("a session whose child has died reports a KernelError") {
    // The script runs out, so FakeTransport reports itself dead — the same
    // signal a real transport gives when the child exits.
    ScriptedSession scripted({});
    REQUIRE(scripted.transport->scriptExhausted());
    CHECK_THROWS_AS(scripted.session->eval(Payload::text("1+1")), mx::KernelError);
}

TEST_CASE("a null transport is rejected rather than dereferenced") {
    CHECK_THROWS_AS(
        MaximaSession(std::unique_ptr<mx::detail::ITransport>(), mx::Config{}),
        mx::KernelError);
    CHECK_THROWS_AS(MaximaSession(MaximaSession::TransportFactory{}, mx::Config{}),
                    mx::KernelError);
}

TEST_CASE("a session that cannot answer restarts and replays its state") {
    // Two scripted transports: the first dies mid-request, the second answers
    // the handshake, the replayed statement, and then the retry.
    //
    // This is what stops one hung computation costing a whole session. Without
    // the replay, the restarted kernel would come back *working but wrong* —
    // missing every assumption the caller had established, which is worse than
    // an outright failure because nothing announces it.
    int built = 0;
    std::vector<std::string> sentToSecond;

    auto factory = [&]() -> std::unique_ptr<mx::detail::ITransport> {
        ++built;
        if (built == 1) {
            // Answers the handshake, then nothing: the child has gone.
            return std::make_unique<FakeTransport>(handshakeScript());
        }
        std::vector<std::string> script = handshakeScript();
        // The replayed statement, then the caller's retry.
        script.push_back(frame(2, true, "$DONE"));
        script.push_back(frame(3, true, "$RECOVERED"));
        return std::make_unique<FakeTransport>(std::move(script));
    };

    MaximaSession session(factory, mx::Config{});
    REQUIRE(built == 1);

    const std::uint64_t handle = session.remember(Payload::text("assume(x > 0)"));
    static_cast<void>(handle);

    // The first transport's script is exhausted, so this call finds a dead
    // child and fails — but triggers recovery on the way out.
    CHECK_THROWS_AS(session.eval(Payload::text("1+1")), mx::KernelError);
    CHECK(built == 2);

    // And the session works again, with the remembered statement replayed.
    CHECK(session.eval(Payload::text("something")).value == "$RECOVERED");
}

/// Forwards to a FakeTransport the test owns, so the test can still inspect it
/// after the session has discarded the transport.
class Borrowed final : public mx::detail::ITransport {
public:
    explicit Borrowed(FakeTransport &inner) : inner_(inner) {}
    void send(std::string_view bytes) override { inner_.send(bytes); }
    std::string receive(std::chrono::milliseconds timeout) override {
        return inner_.receive(timeout);
    }
    bool alive() const override { return inner_.alive(); }
    void kill() override { inner_.kill(); }
    void terminate() override { inner_.terminate(); }

private:
    FakeTransport &inner_;
};

TEST_CASE("a timeout ends the busy child at once instead of waiting for it") {
    // Recovery used to call kill(), which gives a child that was asked to quit
    // two seconds to leave. A child that timed out was not asked: it is still
    // computing and never leaves, so every timeout cost two seconds more than
    // it needed to.
    FakeTransport busy(handshakeScript());
    busy.staySilentWhenExhausted();

    int built = 0;
    auto factory = [&]() -> std::unique_ptr<mx::detail::ITransport> {
        ++built;
        if (built == 1) {
            return std::make_unique<Borrowed>(busy);
        }
        std::vector<std::string> script = handshakeScript();
        script.push_back(frame(2, true, "$AFTER"));
        return std::make_unique<FakeTransport>(std::move(script));
    };

    mx::Config config;
    config.timeout = std::chrono::milliseconds(100);
    MaximaSession session(factory, config);

    CHECK_THROWS_AS(session.eval(Payload::text("expand((x+y+z)^200)")),
                    mx::TimeoutError);
    CHECK(built == 2);
    CHECK(busy.terminated());
    CHECK_FALSE(busy.killedGracefully());

    // And only that call was lost.
    CHECK(session.eval(Payload::text("again")).value == "$AFTER");
}

TEST_CASE("a session with no way to build another transport does not restart") {
    ScriptedSession scripted({});
    REQUIRE(scripted.transport->scriptExhausted());
    CHECK_THROWS_AS(scripted.session->eval(Payload::text("1+1")), mx::KernelError);
    // Still dead, and honestly so, rather than pretending to recover.
    CHECK_THROWS_AS(scripted.session->eval(Payload::text("1+1")), mx::KernelError);
}

TEST_CASE("restart() replaces the process and replays the journal") {
    int built = 0;
    auto factory = [&]() -> std::unique_ptr<mx::detail::ITransport> {
        ++built;
        std::vector<std::string> script = handshakeScript();
        if (built > 1) {
            // The replayed statement, then a question.
            script.push_back(frame(2, true, "$DONE"));
            script.push_back(frame(3, true, "$FRESH"));
        }
        return std::make_unique<FakeTransport>(std::move(script));
    };

    MaximaSession session(factory, mx::Config{});
    session.remember(Payload::text("assume(x > 0)"));

    session.restart();
    CHECK(built == 2);
    CHECK(session.eval(Payload::text("question")).value == "$FRESH");

    SUBCASE("unless there is no way to start another") {
        ScriptedSession scripted({});
        CHECK_THROWS_AS(scripted.session->restart(), mx::KernelError);
    }
}

TEST_CASE("forgetting a statement stops it being replayed") {
    int built = 0;
    auto factory = [&]() -> std::unique_ptr<mx::detail::ITransport> {
        ++built;
        std::vector<std::string> script = handshakeScript();
        if (built > 1) {
            // Only the retry, with no replayed statement before it: if the
            // forgotten entry were still in the journal it would consume this
            // frame and the assertion below would see the wrong value.
            script.push_back(frame(2, true, "$CLEAN"));
        }
        return std::make_unique<FakeTransport>(std::move(script));
    };

    MaximaSession session(factory, mx::Config{});
    const std::uint64_t handle = session.remember(Payload::text("assume(x > 0)"));
    session.forget(handle);

    CHECK_THROWS_AS(session.eval(Payload::text("1+1")), mx::KernelError);
    CHECK(session.eval(Payload::text("again")).value == "$CLEAN");
}

TEST_CASE("a failed handshake is reported at construction") {
    // If the session cannot be made machine-readable there is no point letting
    // the caller discover that one query later.
    auto transport = std::make_unique<FakeTransport>(
        std::vector<std::string>{frame(1, false, "NIL", "something went wrong")});
    CHECK_THROWS_AS(MaximaSession(std::move(transport), mx::Config{}),
                    mx::KernelError);
}
