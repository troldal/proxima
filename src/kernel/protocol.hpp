#pragma once

// The wire protocol between this library and the Lisp helper it installs in
// Maxima: what a request looks like, how a reply is framed, and how a frame
// is read back into a Reply. Everything here is a pure function of its
// arguments; nothing talks to a process. MaximaSession does the talking.
//
// ## The protocol
//
// A Lisp helper installed at startup wraps every reply in delimiters carrying
// the request's own id:
//
//     @@B<tag>@@<status>@@S<tag>@@<value>@@S<tag>@@<reason>@@E<tag>@@
//
// where the tag is `<key>-<id>`: the request's id, behind a key drawn at
// random for each session. The id alone was enough to keep one reply from
// being mistaken for another, but not to keep a *value* from ending its own
// frame — a Maxima string containing `@@E7@@`, printed inside frame 7, did
// exactly that, and ids are sequential and so easy to guess. The key is never
// part of any value Maxima is asked to compute.
//
// Each request is sent as
//
//     cppsend(<tag>, errcatch(ratdisrep(<payload>)))$
//
// where the payload is one of exactly two calls, each taking a single string
// literal that this layer escaped itself:
//
//     cppread("((MPLUS) 1 $X)")        a Maxima internal form, read and evaluated
//     eval_string("integrate(x^2, x)") Maxima source, parsed and evaluated
//
// Five things fall out of that shape:
//
// - **The id makes desynchronisation detectable.** A reply is only accepted
//   for the request that asked for it; a stale or duplicated frame is skipped
//   rather than silently returned as the answer to the wrong question.
// - **errcatch turns errors into values.** It yields `[]` on failure and
//   `[result]` on success, so Maxima never drops into an error prompt that
//   leaves the stream off by one. Success and failure are read from the frame
//   rather than guessed at from the shape of some text — and so is *which*
//   failure: the status is `T` for a value, `Q` for a question Maxima needed
//   to ask and could not, and `NIL` for an error of its own.
// - **Nothing variable ever reaches Maxima's reader as syntax.** The only
//   text Maxima parses is the fixed wrapper plus a string literal. Reading
//   the string's *contents* happens inside errcatch, so a malformed
//   expression is an ordinary failure with a message. Before this, the
//   expression was spliced in raw, and a stray `$` failed in the reader —
//   before errcatch — which produced no frame at all and cost the caller the
//   full Config::timeout and a restart.
// - **The value is Maxima's internal s-expression, not its display output.**
//   `(%oN)` text is a display format: it is ambiguous, it line-wraps, and it
//   loses exact rationals. The internal form has no precedence to re-derive
//   and keeps `((RAT SIMP) 1 3)` as a rational. `ratdisrep` prevents canonical
//   rational (`MRAT`) forms coming back in place of general ones. And with
//   `cppread` the *outbound* direction is the same form, so an expression
//   goes out as structure and comes back as structure: the infix printer is
//   no longer part of the protocol at all.
// - **Errors no longer leak into the stream.** With `errormsg:false` Maxima
//   stops printing them, and the helper renders the message into the frame, so
//   everything between frames is noise that can simply be discarded.

#include "kernel/reply.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace proxima::detail {

/// The variable part of a request: a Maxima call taking one string literal.
///
/// Only the two factories can make one, and both escape the string they are
/// given, so there is no way to hand the session text that Maxima's reader
/// will see as syntax. That is the whole point of the type; see the protocol
/// notes above.
class Payload {
public:
    /// `cppread("<sexpr>")` — a Maxima internal form, to be read by the Lisp
    /// helper and evaluated. What to_maxima produces.
    static Payload form(std::string_view sexpr);

    /// `eval_string("<source>")` — Maxima source text, parsed and evaluated
    /// by Maxima's own parser. The escape hatch for anything an Expr cannot
    /// say.
    static Payload text(std::string_view source);

    /// The call, exactly as it is substituted into the request wrapper. Also
    /// the reply-cache key, since it is the whole question.
    const std::string &str() const { return call_; }

    bool operator==(const Payload &other) const = default;

private:
    explicit Payload(std::string call) : call_(std::move(call)) {}
    std::string call_;
};

/// A fresh frame key: sixteen hex digits from std::random_device.
std::string random_frame_key();

/// The Lisp installed at startup: the helper that reads forms, frames replies
/// and turns Maxima's questions into errors. Passed to SBCL as `--eval`.
std::string_view helper_lisp();

/// The statements sent once at startup to make the session machine-readable
/// and deterministic.
std::vector<std::string> setup_statements();

/// Frame delimiters for request `id` under frame key `key`. Kept in step with
/// the helper, which prints them; a test asserts that they agree.
std::string frame_begin(std::string_view key, std::uint64_t id);
std::string frame_separator(std::string_view key, std::uint64_t id);
std::string frame_end(std::string_view key, std::uint64_t id);

/// Wraps `payload` in the framed, error-trapping call sent to Maxima, for
/// request `id` of the session whose frame key is `key`.
std::string request_for(std::string_view key, std::uint64_t id,
                        const Payload &payload);

/// The most a single reply may occupy before it is abandoned as a broken
/// conversation. Without a limit, a child that streamed without ever closing
/// its frame was bounded only by Config::timeout — at pipe speed, gigabytes.
/// The largest reply measured in practice is under a megabyte.
inline constexpr std::size_t kMaxFrameBytes = std::size_t{256} * 1024 * 1024;

/// The status a frame's first field carries: `T` for a value, `Q` for a
/// question Maxima could not ask — the caller has an assumption to supply —
/// and anything else for an error of Maxima's own.
inline constexpr std::string_view kStatusValue = "T";
inline constexpr std::string_view kStatusQuestion = "Q";

/// Reads request `id`'s frame out of `buffer`, given where its closing
/// delimiter was found. Everything before the opening delimiter is banner
/// text, prompts, or a frame belonging to some earlier request, and is
/// ignored. Throws KernelError if the frame is malformed: a closing delimiter
/// with no opening one, or a field separator missing.
Reply parse_frame(std::string_view buffer, std::size_t end_at, std::string_view key,
                  std::uint64_t id);

} // namespace proxima::detail
