#include "kernel/protocol.hpp"

#include "wire/to_maxima.hpp"

#include <proxima/errors.hpp>

#include <format>
#include <random>

namespace proxima::detail {
namespace {

// The Lisp helpers installed at startup: one that reads a form and evaluates
// it, one that does the framing, and one that stops Maxima asking questions.
//
// `$cppread` is the outbound half of the s-expression protocol. It reads one
// Lisp form from the string it is given — with *read-eval* off, so `#.` cannot
// run code, and with the Maxima package current, so `MPLUS` and `$X` land on
// the symbols Maxima uses — and hands it to meval. Because it is called from
// inside errcatch, a form that fails to read or to evaluate is an ordinary
// Maxima error with a message, not a silence.
//
// One thing Maxima's parser does that reading a form does not: resolve
// aliases. `subst` is an alias for `substitute`, and a form headed `$SUBST`
// would evaluate to itself, unrecognised, where the text `subst(...)` would
// have been rewritten on the way in. `cppresolve` walks the form applying
// Maxima's own `getalias` to every symbol, which is exactly what the parser
// does — including `$true` and `$false` to their Lisp spellings.
//
// Maxima interrogates the user when it needs a fact it has not been told —
// `integrate(x^n, x)` asks "Is n equal to -1?" — by printing a prompt and
// reading a line from standard input. Over a pipe that is fatal twice over: the
// read blocks until the timeout, and then Maxima consumes the *next request* as
// the answer, leaving every subsequent reply attached to the wrong question.
//
// Overriding `retrieve`, the single point all of that goes through, turns a
// question into an ordinary Maxima error. errcatch then reports it as a Failure
// carrying the question text, the session stays synchronised, and the caller is
// told exactly which assumption to supply.
//
// `errcatch` hands `x` back as a Maxima list: empty on failure, one element on
// success. On failure the message is rendered by calling errormsg() with
// *standard-output* bound to a string, which is what keeps it inside the frame
// instead of loose in the stream. The request id appears in all three
// delimiters, so a frame can only ever be matched to the request that asked
// for it.
//
// The reply is printed with Lisp's print limits switched off. Maxima itself
// runs with *print-length* at 100 and *print-level* at 15, and under those a
// sum of more than a hundred terms printed as its first hundred and `...`, and
// anything nested deeper than fifteen levels as `#`. The reader took both for
// symbols, so a large result came back as a smaller, wrong one that looked
// right. *print-base* and *print-radix* are pinned for the same reason: what is
// printed here is data for a reader, not text for a person.
//
// Keep the delimiters here in step with frame_begin/frame_separator/frame_end
// below; a test asserts that they agree.
constexpr const char *kHelperLisp = R"LISP((progn
 (defun maxima::cppresolve (form)
  (cond ((symbolp form) (maxima::getalias form))
        ((atom form) form)
        (t (mapcar (function maxima::cppresolve) form))))
 (defun maxima::$cppread (text)
  (let ((*package* (find-package :maxima))
        (*read-eval* nil)
        (*read-base* 10)
        (*read-default-float-format* 'double-float)
        (*readtable* (copy-readtable nil)))
   (maxima::meval (maxima::cppresolve (read-from-string text)))))
 (defun maxima::retrieve (msg flag &rest more)
  (declare (ignore flag more))
  (maxima::merror
   "this computation needs an assumption that was not supplied. Maxima asked: ~a"
   (with-output-to-string (s)
    (dolist (part (cond ((not (listp msg)) (list msg))
                        ((consp (car msg)) (cdr msg))
                        (t msg)))
     (princ (if (stringp part) part (maxima::$sconcat part)) s)))))
 (defun maxima::$cppsend (id x)
  (let ((ok (and (consp x) (cdr x)))
        (reason "")
        (*print-circle* nil)
        (*print-pretty* nil)
        (*print-readably* nil)
        (*print-length* nil)
        (*print-level* nil)
        (*print-lines* nil)
        (*print-base* 10)
        (*print-radix* nil))
   (unless ok
    (let ((sink (make-string-output-stream)))
     (let ((*standard-output* sink)) (ignore-errors (maxima::$errormsg)))
     (setf reason (string-trim (list #\Space #\Newline #\Tab)
                               (get-output-stream-string sink)))))
   (format t "~&@@B~a@@~a@@S~a@@~s@@S~a@@~a@@E~a@@~%"
           id (if ok "T" "NIL") id (if ok (cadr x) nil) id reason id))
  (quote maxima::$done))
 (cl-user::run)))LISP";

/// What the helper formats with ~a in every delimiter.
std::string frame_tag(std::string_view key, std::uint64_t id) {
    return std::format("{}-{}", key, id);
}

} // namespace

Payload Payload::form(std::string_view sexpr) {
    return Payload("cppread(" + string_literal(sexpr) + ")");
}

Payload Payload::text(std::string_view source) {
    return Payload("eval_string(" + string_literal(source) + ")");
}

std::string random_frame_key() {
    std::random_device device;
    const std::uint64_t bits
        = (std::uint64_t{device()} << 32) ^ std::uint64_t{device()};
    return std::format("{:016x}", bits);
}

std::string_view helper_lisp() {
    return kHelperLisp;
}

std::vector<std::string> setup_statements() {
    return {
        // Results as one-dimensional text rather than ASCII art. Irrelevant to
        // the framed values themselves, but it keeps anything Maxima prints
        // outside a frame from becoming a wall of layout.
        "display2d:false$",
        // Maxima otherwise retains every %i/%o label for the life of the
        // session, which for a long-lived kernel is an unbounded leak.
        "nolabels:true$",
        // Errors are rendered into the frame by the helper instead; without
        // this they would also be printed loose in the stream.
        "errormsg:false$",
    };
}

std::string frame_begin(std::string_view key, std::uint64_t id) {
    return "@@B" + frame_tag(key, id) + "@@";
}

std::string frame_separator(std::string_view key, std::uint64_t id) {
    return "@@S" + frame_tag(key, id) + "@@";
}

std::string frame_end(std::string_view key, std::uint64_t id) {
    return "@@E" + frame_tag(key, id) + "@@";
}

std::string request_for(std::string_view key, std::uint64_t id,
                        const Payload &payload) {
    // errcatch turns a Maxima error into an empty list rather than an error
    // prompt; ratdisrep keeps canonical rational (MRAT) forms from coming back
    // in place of general ones. The payload is a call on a string literal, so
    // this text is well-formed whatever the caller asked. The tag travels as a
    // Maxima string, which is a Lisp string by the time the helper prints it
    // with ~a — without quotes, exactly as frame_begin spells it.
    return "cppsend(" + string_literal(frame_tag(key, id)) + ", errcatch(ratdisrep("
           + payload.str() + ")))$";
}

Reply parse_frame(std::string_view buffer, std::size_t end_at, std::string_view key,
                  std::uint64_t id) {
    const std::string begin = frame_begin(key, id);
    const std::string separator = frame_separator(key, id);

    const std::size_t begin_at = buffer.rfind(begin, end_at);
    if (begin_at == std::string_view::npos) {
        throw KernelError("Maxima produced a malformed reply: the closing "
                          "delimiter for request "
                          + frame_tag(key, id) + " arrived without its opening "
                          + "delimiter");
    }

    const std::size_t body_at = begin_at + begin.size();
    const std::string_view body = buffer.substr(body_at, end_at - body_at);

    const std::size_t first_separator = body.find(separator);
    if (first_separator == std::string_view::npos) {
        throw KernelError("Maxima produced a malformed reply for request "
                          + std::to_string(id) + ": missing field separator");
    }
    const std::size_t second_separator
        = body.find(separator, first_separator + separator.size());
    if (second_separator == std::string_view::npos) {
        throw KernelError("Maxima produced a malformed reply for request "
                          + std::to_string(id) + ": missing second field "
                          + "separator");
    }

    Reply reply;
    reply.ok = body.substr(0, first_separator) == "T";

    const std::size_t value_at = first_separator + separator.size();
    const std::size_t reason_at = second_separator + separator.size();
    if (reply.ok) {
        reply.value
            = std::string(body.substr(value_at, second_separator - value_at));
    } else {
        reply.reason = std::string(body.substr(reason_at));
    }
    return reply;
}

} // namespace proxima::detail
