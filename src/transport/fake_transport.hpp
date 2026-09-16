#pragma once

#include "transport/itransport.hpp"

#include <string>
#include <vector>

namespace proxima::detail {

/// A scripted ITransport for tests: no process, no pipes, no Maxima.
///
/// Each receive() hands back the next scripted response in full. Once the
/// script is exhausted the transport reports itself dead, which models a child
/// that has exited and lets tests exercise the session's failure paths.
///
/// Everything sent is recorded, so tests can assert on what the protocol layer
/// actually put on the wire — the half that is otherwise invisible.
class FakeTransport final : public ITransport {
public:
    explicit FakeTransport(std::vector<std::string> responses);

    void send(std::string_view bytes) override;
    std::string receive(std::chrono::milliseconds timeout) override;
    bool alive() const override;
    void kill() override;
    void terminate() override;

    /// Everything written to the transport, one entry per send() call.
    const std::vector<std::string> &sent() const { return sent_; }

    /// Every send() concatenated, for assertions that span calls.
    std::string sent_text() const;

    /// True once every scripted response has been consumed.
    bool script_exhausted() const { return next_ >= responses_.size(); }

    /// After the script runs out, behave like a child that is still busy
    /// computing rather than one that has exited: stay alive, and let each
    /// receive() wait out its timeout with nothing to show for it.
    void stay_silent_when_exhausted() { silent_when_exhausted_ = true; }

    /// Which way the transport was ended, if it was.
    bool killed_gracefully() const { return killed_gracefully_; }
    bool terminated() const { return terminated_; }

private:
    std::vector<std::string> responses_;
    std::vector<std::string> sent_;
    std::size_t next_ = 0;
    bool killed_ = false;
    bool killed_gracefully_ = false;
    bool terminated_ = false;
    bool silent_when_exhausted_ = false;
};

} // namespace proxima::detail
