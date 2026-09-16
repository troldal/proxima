// Protocol tests driven by a scripted transport. No Maxima installation, no
// child process — these are the tests step 4 exists to make possible, now
// covering the framed protocol introduced in step 6.

#include <doctest/doctest.h>

#include "kernel/session.hpp"
#include "transport/fake_transport.hpp"

#include <proxima/errors.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using proxima::detail::FakeTransport;
using proxima::detail::MaximaSession;
using proxima::detail::Payload;

namespace {

/// The frame key every scripted session uses, so replies can be written before
/// the session exists. A real session draws its own at random.
constexpr const char *kKey = "test";

/// A complete reply frame, exactly as the Lisp helper formats one.
std::string frame(std::uint64_t id, bool ok, const std::string &value,
                  const std::string &reason = "") {
    return MaximaSession::frameBegin(kKey, id) + (ok ? "T" : "NIL")
           + MaximaSession::frameSeparator(kKey, id) + value
           + MaximaSession::frameSeparator(kKey, id) + reason
           + MaximaSession::frameEnd(kKey, id) + "\n";
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
        session = std::make_unique<MaximaSession>(std::move(owned), proxima::Config{}, kKey);
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
    CHECK(sent.find(MaximaSession::requestFor(kKey, 1, Payload::form("T")))
          != std::string::npos);
}

TEST_CASE("a request carries its own correlation id") {
    ScriptedSession scripted({frame(2, true, "((MPLUS SIMP) 1 $X)")});
    scripted.session->eval(Payload::text("x+1"));

    // Request 1 was the handshake probe, so the first real request is 2.
    CHECK(scripted.transport->sent().back()
          == MaximaSession::requestFor(kKey, 2, Payload::text("x+1")) + "\n");
}

TEST_CASE("a successful reply yields the internal s-expression") {
    ScriptedSession scripted(
        {frame(2, true, "((MTIMES SIMP) 2 $X ((%SIN SIMP) $X))")});

    const proxima::Reply reply = scripted.session->eval(Payload::text("2*x*sin(x)"));
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

    const proxima::Reply reply = scripted.session->eval(Payload::text("integrate(x, 5)"));
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

TEST_CASE("a value containing its own request's id is not truncated") {
    // The delimiters used to be the id alone, so a Maxima string holding
    // "@@E2@@" closed frame 2 from inside its own value. The frame key, which
    // no value is ever computed from, is what rules that out.
    const std::string tricky = R"("would end early at @@E2@@ and @@S2@@")";
    ScriptedSession scripted({frame(2, true, tricky)});
    CHECK(scripted.session->eval(Payload::text("\"...\"")).value == tricky);
}

TEST_CASE("each session draws a frame key of its own") {
    const std::string first = proxima::detail::randomFrameKey();
    const std::string second = proxima::detail::randomFrameKey();
    CHECK(first.size() == 16);
    CHECK(first.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(first != second);
}

TEST_CASE("a reply that never completes is abandoned at the size limit") {
    // Two halves that together pass the limit and carry no closing delimiter.
    // Without the limit this read would continue until Config::timeout.
    const std::string half(MaximaSession::kMaxFrameBytes / 2 + 1, 'x');
    ScriptedSession scripted({frame(2, true, "1").substr(0, 20) + half, half});
    CHECK_THROWS_AS(scripted.session->eval(Payload::text("x")), proxima::KernelError);
}

TEST_CASE("a closing delimiter with no opening one is a protocol error") {
    ScriptedSession scripted({MaximaSession::frameEnd(kKey, 2) + "\n"});
    CHECK_THROWS_AS(scripted.session->eval(Payload::text("x")), proxima::KernelError);
}

TEST_CASE("a frame missing its field separators is a protocol error") {
    ScriptedSession scripted({MaximaSession::frameBegin(kKey, 2) + "T"
                              + MaximaSession::frameEnd(kKey, 2)});
    CHECK_THROWS_AS(scripted.session->eval(Payload::text("x")), proxima::KernelError);
}

TEST_CASE("a session whose child has died reports a KernelError") {
    // The script runs out, so FakeTransport reports itself dead — the same
    // signal a real transport gives when the child exits.
    ScriptedSession scripted({});
    REQUIRE(scripted.transport->scriptExhausted());
    CHECK_THROWS_AS(scripted.session->eval(Payload::text("1+1")), proxima::KernelError);
}

TEST_CASE("a null transport is rejected rather than dereferenced") {
    CHECK_THROWS_AS(
        MaximaSession(std::unique_ptr<proxima::detail::ITransport>(), proxima::Config{}, kKey),
        proxima::KernelError);
    CHECK_THROWS_AS(MaximaSession(MaximaSession::TransportFactory{}, proxima::Config{}, kKey),
                    proxima::KernelError);
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

    auto factory = [&]() -> std::unique_ptr<proxima::detail::ITransport> {
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

    MaximaSession session(factory, proxima::Config{}, kKey);
    REQUIRE(built == 1);

    const std::uint64_t handle = session.remember(Payload::text("assume(x > 0)"));
    static_cast<void>(handle);

    // The first transport's script is exhausted, so this call finds a dead
    // child and fails — but triggers recovery on the way out.
    CHECK_THROWS_AS(session.eval(Payload::text("1+1")), proxima::KernelError);
    CHECK(built == 2);

    // And the session works again, with the remembered statement replayed.
    CHECK(session.eval(Payload::text("something")).value == "$RECOVERED");
}

/// Forwards to a FakeTransport the test owns, so the test can still inspect it
/// after the session has discarded the transport.
class Borrowed final : public proxima::detail::ITransport {
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
    auto factory = [&]() -> std::unique_ptr<proxima::detail::ITransport> {
        ++built;
        if (built == 1) {
            return std::make_unique<Borrowed>(busy);
        }
        std::vector<std::string> script = handshakeScript();
        script.push_back(frame(2, true, "$AFTER"));
        return std::make_unique<FakeTransport>(std::move(script));
    };

    proxima::Config config;
    config.timeout = std::chrono::milliseconds(100);
    MaximaSession session(factory, config, kKey);

    CHECK_THROWS_AS(session.eval(Payload::text("expand((x+y+z)^200)")),
                    proxima::TimeoutError);
    CHECK(built == 2);
    CHECK(busy.terminated());
    CHECK_FALSE(busy.killedGracefully());

    // And only that call was lost.
    CHECK(session.eval(Payload::text("again")).value == "$AFTER");
}

TEST_CASE("bookkeeping does not wait behind a call in progress") {
    // One lock used to guard everything, so cacheStats() and setTimeout()
    // waited for a running computation — for as long as Config::timeout — and
    // setTimeout could never shorten the call it was waiting behind.
    using namespace std::chrono_literals;

    auto owned = std::make_unique<FakeTransport>(handshakeScript());
    owned->staySilentWhenExhausted();
    proxima::Config config;
    config.timeout = 30s;
    MaximaSession session(std::move(owned), config, kKey);

    auto running = std::async(std::launch::async, [&session] {
        return session.eval(Payload::text("something slow"));
    });
    std::this_thread::sleep_for(200ms); // Well into its wait for a reply.

    auto stats = std::async(std::launch::async, [&session] {
        return session.cacheStats();
    });
    CHECK(stats.wait_for(2s) == std::future_status::ready);

    session.setTimeout(50ms);
    const bool finished = running.wait_for(5s) == std::future_status::ready;
    CHECK(finished);
    if (!finished) {
        // Let the call end so the test does not hang on the old behaviour.
        session.setTimeout(1ms);
    }
    CHECK_THROWS_AS(running.get(), proxima::TimeoutError);
}

/// A transport that answers the handshake at once, then holds back the next
/// reply until the test releases it — a computation the test can pause.
class GatedTransport final : public proxima::detail::ITransport {
public:
    GatedTransport(std::vector<std::string> script, std::string gatedReply)
        : script_(std::move(script)), gatedReply_(std::move(gatedReply)) {}

    void send(std::string_view) override {}
    std::string receive(std::chrono::milliseconds timeout) override {
        if (next_ < script_.size()) {
            return script_[next_++];
        }
        if (!gatedSent_) {
            waiting = true;
            if (release) {
                gatedSent_ = true;
                return gatedReply_;
            }
        }
        std::this_thread::sleep_for(timeout);
        return {};
    }
    bool alive() const override { return true; }
    void kill() override {}
    void terminate() override {}

    std::atomic<bool> waiting{false};
    std::atomic<bool> release{false};

private:
    std::vector<std::string> script_;
    std::size_t next_ = 0;
    std::string gatedReply_;
    bool gatedSent_ = false;
};

TEST_CASE("an answer computed across a change of state is not cached") {
    // remember() no longer waits for the pipe, so the journal — and with it
    // the key an answer is filed under — can change while Maxima is working on
    // a question. The answer belongs to the state before the change.
    using namespace std::chrono_literals;

    auto owned = std::make_unique<GatedTransport>(handshakeScript(),
                                                  frame(2, true, "$BEFORE"));
    GatedTransport *gate = owned.get();
    MaximaSession session(std::move(owned), proxima::Config{}, kKey);

    auto asking = std::async(std::launch::async, [&session] {
        return session.evalPure(Payload::text("question"));
    });
    while (!gate->waiting) {
        std::this_thread::sleep_for(1ms);
    }

    auto remembering = std::async(std::launch::async, [&session] {
        return session.remember(Payload::text("assume(x > 0)"));
    });
    // Does not wait for the computation, which is still held back.
    CHECK(remembering.wait_for(2s) == std::future_status::ready);

    gate->release = true;
    CHECK(asking.get().value == "$BEFORE");
    static_cast<void>(remembering.get());
    CHECK(session.cacheStats().entries == 0);
}

TEST_CASE("a statement and its record are one conversation") {
    // A Context's statement and the journal's record of it used to be two
    // separate calls. A question from another thread could be answered in
    // between — under Maxima's new state, but cached under the journal's old
    // one. Inside converseAtomically it has to wait until the record is made.
    using namespace std::chrono_literals;

    ScriptedSession scripted({frame(2, true, "$DONE"), frame(3, true, "$ANSWER")});
    MaximaSession &session = *scripted.session;

    std::future<proxima::Reply> asking;
    session.converseAtomically([&](MaximaSession::Conversation &conversation) {
        conversation.evalTracked(Payload::text("assume(x > 0)"));

        asking = std::async(std::launch::async, [&session] {
            return session.evalPure(Payload::text("question"));
        });
        // Between the statement and its record: the question must wait.
        CHECK(asking.wait_for(200ms) == std::future_status::timeout);

        conversation.remember(Payload::text("assume(x > 0)"));
    });

    CHECK(asking.get().value == "$ANSWER");
    // Asked once the record existed, so its answer describes the recorded
    // state, and is kept.
    CHECK(session.cacheStats().entries == 1);
    // And the statement went out before the question did.
    const std::string sent = scripted.transport->sentText();
    CHECK(sent.find("assume(x > 0)") < sent.find("question"));
}

TEST_CASE("a session with no way to build another transport does not restart") {
    ScriptedSession scripted({});
    REQUIRE(scripted.transport->scriptExhausted());
    CHECK_THROWS_AS(scripted.session->eval(Payload::text("1+1")), proxima::KernelError);
    // Still dead, and honestly so, rather than pretending to recover.
    CHECK_THROWS_AS(scripted.session->eval(Payload::text("1+1")), proxima::KernelError);
}

TEST_CASE("restart() replaces the process and replays the journal") {
    int built = 0;
    auto factory = [&]() -> std::unique_ptr<proxima::detail::ITransport> {
        ++built;
        std::vector<std::string> script = handshakeScript();
        if (built > 1) {
            // The replayed statement, then a question.
            script.push_back(frame(2, true, "$DONE"));
            script.push_back(frame(3, true, "$FRESH"));
        }
        return std::make_unique<FakeTransport>(std::move(script));
    };

    MaximaSession session(factory, proxima::Config{}, kKey);
    session.remember(Payload::text("assume(x > 0)"));

    session.restart();
    CHECK(built == 2);
    CHECK(session.eval(Payload::text("question")).value == "$FRESH");

    SUBCASE("unless there is no way to start another") {
        ScriptedSession scripted({});
        CHECK_THROWS_AS(scripted.session->restart(), proxima::KernelError);
    }
}

TEST_CASE("forgetting a statement stops it being replayed") {
    int built = 0;
    auto factory = [&]() -> std::unique_ptr<proxima::detail::ITransport> {
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

    MaximaSession session(factory, proxima::Config{}, kKey);
    const std::uint64_t handle = session.remember(Payload::text("assume(x > 0)"));
    session.forget(handle);

    CHECK_THROWS_AS(session.eval(Payload::text("1+1")), proxima::KernelError);
    CHECK(session.eval(Payload::text("again")).value == "$CLEAN");
}

TEST_CASE("a failed handshake is reported at construction") {
    // If the session cannot be made machine-readable there is no point letting
    // the caller discover that one query later.
    auto transport = std::make_unique<FakeTransport>(
        std::vector<std::string>{frame(1, false, "NIL", "something went wrong")});
    CHECK_THROWS_AS(MaximaSession(std::move(transport), proxima::Config{}, kKey),
                    proxima::KernelError);
}
