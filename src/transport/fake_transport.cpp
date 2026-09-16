#include "transport/fake_transport.hpp"

#include <thread>

namespace proxima::detail {

FakeTransport::FakeTransport(std::vector<std::string> responses)
    : responses_(std::move(responses)) {}

void FakeTransport::send(std::string_view bytes) {
    sent_.emplace_back(bytes);
}

std::string FakeTransport::receive(std::chrono::milliseconds timeout) {
    if (killed_) {
        return {};
    }
    if (next_ >= responses_.size()) {
        if (silent_when_exhausted_) {
            // A real transport waits for output that does not come; so does
            // this, so a session's deadline passes in real time.
            std::this_thread::sleep_for(timeout);
        }
        return {};
    }
    return responses_[next_++];
}

bool FakeTransport::alive() const {
    return !killed_ && (next_ < responses_.size() || silent_when_exhausted_);
}

void FakeTransport::kill() {
    killed_ = true;
    killed_gracefully_ = true;
}

void FakeTransport::terminate() {
    killed_ = true;
    terminated_ = true;
}

std::string FakeTransport::sent_text() const {
    std::string all;
    for (const std::string &chunk : sent_) {
        all += chunk;
    }
    return all;
}

} // namespace proxima::detail
