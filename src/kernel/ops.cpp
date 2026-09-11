#include <mx/ops.hpp>

#include "wire/from_maxima.hpp"
#include "wire/sexpr.hpp"

#include <mx/errors.hpp>

#include <string>
#include <utility>

namespace mx {
namespace {

/// Evaluates Maxima source and maps the reply back into an expression.
///
/// The whole round trip in one place: print, send, read, map. Every operation
/// below is a one-line wrapper around this, which is what keeps the dispatch
/// layer from accumulating protocol knowledge.
std::expected<Expr, Failure> evaluate(Kernel &kernel, const std::string &source) {
    // evalPure, not eval: every operation here is a question rather than an
    // instruction, so the answer can be remembered. The promise that goes with
    // evalPure is exactly what these functions are — nothing below assigns,
    // assumes or defines anything.
    const Reply reply = kernel.evalPure(source);
    if (!reply.ok) {
        return std::unexpected(Failure{reply.reason});
    }
    return detail::fromMaxima(detail::parseSExpr(reply.value));
}

/// For the operations with no ordinary failure mode.
Expr evaluateOrThrow(Kernel &kernel, const std::string &source) {
    auto result = evaluate(kernel, source);
    if (!result) {
        throw MaximaError(result.error().message);
    }
    return std::move(*result);
}

std::string call(std::string_view head, std::initializer_list<std::string> args) {
    std::string source(head);
    source += "(";
    bool first = true;
    for (const std::string &arg : args) {
        if (!first) {
            source += ", ";
        }
        first = false;
        source += arg;
    }
    source += ")";
    return source;
}

/// Renders `text` as a Maxima string literal.
std::string quoteForMaxima(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

/// True when `expr` is `head(...)` left unevaluated by Maxima — its way of
/// saying it could not do the job.
bool isUnevaluated(const Expr &expr, std::string_view head) {
    return expr.is(Kind::Function) && expr.name() == head;
}

std::string_view sideKeyword(Side side) {
    switch (side) {
    case Side::FromAbove:
        return "plus";
    case Side::FromBelow:
        return "minus";
    case Side::Both:
        break;
    }
    return {};
}

} // namespace

Kernel &sharedKernel() {
    // Started on first use. If construction throws — no Maxima installed — the
    // next call retries, which is the behaviour a function-local static gives
    // and the one that makes sense here.
    static Kernel kernel;
    return kernel;
}

std::expected<Expr, Failure> parse(std::string_view source, Kernel &kernel) {
    return evaluate(kernel, call("parse_string", {quoteForMaxima(source)}));
}

Expr diff(const Expr &expr, const Symbol &wrt, unsigned order, Kernel &kernel) {
    return evaluateOrThrow(
        kernel,
        call("diff", {expr.str(), wrt.name(), std::to_string(order)}));
}

Expr expand(const Expr &expr, Kernel &kernel) {
    return evaluateOrThrow(kernel, call("expand", {expr.str()}));
}

Expr factor(const Expr &expr, Kernel &kernel) {
    return evaluateOrThrow(kernel, call("factor", {expr.str()}));
}

Expr simplify(const Expr &expr, Kernel &kernel) {
    return evaluateOrThrow(kernel, call("ratsimp", {expr.str()}));
}

Expr subst(const Expr &expr, const Symbol &symbol, const Expr &value,
           Kernel &kernel) {
    // Maxima's argument order is (replacement, target, expression).
    return evaluateOrThrow(
        kernel, call("subst", {value.str(), symbol.name(), expr.str()}));
}

std::expected<Expr, Failure> integrate(const Expr &expr, const Symbol &wrt,
                                       Kernel &kernel) {
    auto result = evaluate(kernel, call("integrate", {expr.str(), wrt.name()}));
    if (!result) {
        return result;
    }
    // Maxima does not treat "I cannot do this" as an error; it hands the
    // integral back unevaluated. That noun form is the failure signal.
    if (isUnevaluated(*result, "integrate")) {
        return std::unexpected(
            Failure{"no closed form for the integral of " + expr.str()
                    + " with respect to " + wrt.name()});
    }
    return result;
}

std::expected<Expr, Failure> integrate(const Expr &expr, const Symbol &wrt,
                                       const Expr &from, const Expr &to,
                                       Kernel &kernel) {
    auto result = evaluate(
        kernel,
        call("integrate", {expr.str(), wrt.name(), from.str(), to.str()}));
    if (!result) {
        return result;
    }
    if (isUnevaluated(*result, "integrate")) {
        return std::unexpected(
            Failure{"no closed form for the integral of " + expr.str()
                    + " over [" + from.str() + ", " + to.str() + "]"});
    }
    return result;
}

std::expected<Expr, Failure> limit(const Expr &expr, const Symbol &wrt,
                                   const Expr &to, Side side, Kernel &kernel) {
    const std::string_view keyword = sideKeyword(side);
    auto result
        = keyword.empty()
              ? evaluate(kernel,
                         call("limit", {expr.str(), wrt.name(), to.str()}))
              : evaluate(kernel, call("limit", {expr.str(), wrt.name(), to.str(),
                                                std::string(keyword)}));
    if (!result) {
        return result;
    }
    if (isUnevaluated(*result, "limit")) {
        return std::unexpected(Failure{"Maxima could not determine the limit of "
                                       + expr.str() + " as " + wrt.name()
                                       + " approaches " + to.str()});
    }
    // `und` is Maxima's "undefined", a definite answer that no limit exists.
    if (result->is(Kind::Symbol) && result->name() == "und") {
        return std::unexpected(Failure{"the limit of " + expr.str() + " as "
                                       + wrt.name() + " approaches " + to.str()
                                       + " does not exist"});
    }
    return result;
}

std::expected<std::vector<Expr>, Failure>
solve(const Expr &equation, const Symbol &unknown, Kernel &kernel) {
    auto result
        = evaluate(kernel, call("solve", {equation.str(), unknown.name()}));
    if (!result) {
        return std::unexpected(result.error());
    }
    if (!isUnevaluated(*result, "list")) {
        return std::unexpected(
            Failure{"solve did not return a list of solutions, but "
                    + result->str()});
    }

    std::vector<Expr> values;
    values.reserve(result->arity());
    for (const Expr &solution : result->args()) {
        // Maxima reports failure to solve by returning something that is not a
        // solution rather than by erroring: an equation still mentioning the
        // unknown on both sides, or one it never rearranged. Rejecting those
        // here is what makes a success mean what it says.
        const bool isAssignment = solution.is(Kind::Relation)
                                  && solution.relationOp() == RelOp::Equal
                                  && solution.arg(0) == Expr(unknown)
                                  && !contains(solution.arg(1), unknown);
        if (!isAssignment) {
            return std::unexpected(
                Failure{"Maxima did not solve " + equation.str() + " for "
                        + unknown.name() + "; it returned " + result->str()});
        }
        values.push_back(solution.arg(1));
    }
    return values;
}

bool contains(const Expr &expr, const Symbol &symbol) {
    if (expr.is(Kind::Symbol)) {
        return expr.name() == symbol.name();
    }
    for (const Expr &operand : expr.args()) {
        if (contains(operand, symbol)) {
            return true;
        }
    }
    return false;
}

} // namespace mx
