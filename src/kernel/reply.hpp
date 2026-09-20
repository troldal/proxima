#pragma once

#include <proxima/result.hpp>

#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace proxima::detail {

/// The outcome of one Maxima evaluation, as the session records it: a value,
/// or a failure carrying the cause it was.
///
/// Internal: the public Kernel hands out a proxima::result<std::string>. This
/// is the shape the reply cache and the persistent cache store, which is why
/// it is a value type rather than an expected.
///
/// The two are alternatives of a variant, so a success carrying an error
/// message — or a failure carrying an answer — cannot be built. It used to be
/// `{bool ok, string value, string reason}`, with both of those representable;
/// and the *cause* was not carried at all but recovered downstream by matching
/// the reason against literals that had to be kept in step across three files,
/// so a Maxima error that happened to begin with those words was misread. Now
/// the protocol says which kind of failure it was, in the frame, and the cause
/// travels with the reply from there.
class Reply {
public:
    /// Why there is no value: the cause a program branches on, and Maxima's
    /// own wording for a person.
    struct Failed {
        Cause cause = Cause::Unknown;
        std::string message;
    };

    /// Maxima's answer: the text of its internal s-expression, e.g.
    /// "((MTIMES SIMP) 2 $X ((%SIN SIMP) $X))".
    static Reply value(std::string wire) { return Reply(std::move(wire)); }

    static Reply failure(Cause cause, std::string message) {
        return Reply(Failed{cause, std::move(message)});
    }

    bool ok() const { return std::holds_alternative<std::string>(outcome_); }

    /// The answer; empty for a failure.
    const std::string &value() const {
        static const std::string none;
        const auto *wire = std::get_if<std::string>(&outcome_);
        return wire != nullptr ? *wire : none;
    }

    /// Maxima's wording for the failure; empty for a value.
    const std::string &reason() const {
        static const std::string none;
        const auto *failed = std::get_if<Failed>(&outcome_);
        return failed != nullptr ? failed->message : none;
    }

    /// Why there is no value; Cause::Unknown for a value.
    Cause cause() const {
        const auto *failed = std::get_if<Failed>(&outcome_);
        return failed != nullptr ? failed->cause : Cause::Unknown;
    }

    /// The outcome itself, for a caller about to consume it.
    std::variant<std::string, Failed> take() && { return std::move(outcome_); }

private:
    explicit Reply(std::variant<std::string, Failed> outcome)
        : outcome_(std::move(outcome)) {}

    std::variant<std::string, Failed> outcome_;
};

/// The cause `number` stands for, or nothing if this build has no such cause:
/// what the persistent cache reads back, where a file may have been written
/// by another version of this library.
///
/// A switch over every enumerator, so that adding a Cause is a compile error
/// here — with warnings as errors — rather than a value that silently reads
/// back as something else.
inline std::optional<Cause> cause_from_number(int number) {
    const auto cause = static_cast<Cause>(number);
    switch (cause) {
    case Cause::Unknown:
    case Cause::MaximaError:
    case Cause::NeedsAssumption:
    case Cause::Inconsistent:
    case Cause::NoClosedForm:
    case Cause::NotSolved:
    case Cause::NoLimit:
    case Cause::UnexpectedAnswer:
    case Cause::Parse:
    case Cause::Eval:
    case Cause::Argument:
    case Cause::Overflow:
        return cause;
    }
    return std::nullopt;
}

/// The reply as the public result: the wire text, or Maxima's message with
/// the cause the protocol gave it.
inline result<std::string> to_result(Reply reply) {
    auto outcome = std::move(reply).take();
    if (auto *value = std::get_if<std::string>(&outcome)) {
        return std::move(*value);
    }
    auto &failed = std::get<Reply::Failed>(outcome);
    return fxt::unexpected(fail(failed.cause, std::move(failed.message)));
}

} // namespace proxima::detail
