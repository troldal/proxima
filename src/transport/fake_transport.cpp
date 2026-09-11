#include "transport/fake_transport.hpp"

namespace mx::detail {

FakeTransport::FakeTransport(std::vector<std::string> responses)
    : responses_(std::move(responses)) {}

void FakeTransport::send(std::string_view bytes) {
    sent_.emplace_back(bytes);
}

std::string FakeTransport::receive(std::chrono::milliseconds) {
    if (killed_ || next_ >= responses_.size()) {
        return {};
    }
    return responses_[next_++];
}

bool FakeTransport::alive() const {
    return !killed_ && next_ < responses_.size();
}

void FakeTransport::kill() {
    killed_ = true;
}

std::string FakeTransport::sentText() const {
    std::string all;
    for (const std::string &chunk : sent_) {
        all += chunk;
    }
    return all;
}

} // namespace mx::detail
