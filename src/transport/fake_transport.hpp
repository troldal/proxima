#pragma once

#include "transport/itransport.hpp"

#include <string>
#include <vector>

namespace mx::detail {

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

    /// Everything written to the transport, one entry per send() call.
    const std::vector<std::string> &sent() const { return sent_; }

    /// Every send() concatenated, for assertions that span calls.
    std::string sentText() const;

    /// True once every scripted response has been consumed.
    bool scriptExhausted() const { return next_ >= responses_.size(); }

private:
    std::vector<std::string> responses_;
    std::vector<std::string> sent_;
    std::size_t next_ = 0;
    bool killed_ = false;
};

} // namespace mx::detail
