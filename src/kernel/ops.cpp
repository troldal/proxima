#include <mx/ops.hpp>

#include "wire/from_maxima.hpp"
#include "wire/sexpr.hpp"

#include <mx/errors.hpp>

#include <algorithm>
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

std::expected<std::vector<Solution>, Failure>
solve(std::span<const Expr> equations, std::span<const Symbol> unknowns,
      Kernel &kernel) {
    if (unknowns.empty()) {
        return std::unexpected(Failure{"solve was given no unknowns"});
    }
    if (equations.empty()) {
        return std::unexpected(Failure{"solve was given no equations"});
    }

    const auto listOf = [](auto &&items, auto &&render) {
        std::string text = "[";
        bool first = true;
        for (const auto &item : items) {
            if (!first) {
                text += ", ";
            }
            first = false;
            text += render(item);
        }
        text += "]";
        return text;
    };

    const std::string equationList
        = listOf(equations, [](const Expr &e) { return e.str(); });
    const std::string unknownList
        = listOf(unknowns, [](const Symbol &s) { return s.name(); });

    auto result = evaluate(kernel, call("solve", {equationList, unknownList}));
    if (!result) {
        return std::unexpected(result.error());
    }
    if (!isUnevaluated(*result, "list")) {
        return std::unexpected(
            Failure{"solve did not return a list of solutions, but "
                    + result->str()});
    }

    // Maxima flattens the result when there is one unknown: solve([x^2=1], [x])
    // gives [x = -1, x = 1], not [[x = -1], [x = 1]]. Detecting that from the
    // shape rather than from the number of unknowns keeps this right whichever
    // way Maxima decides to answer.
    const bool flattened
        = !result->args().empty() && result->arg(0).is(Kind::Relation);

    const auto reject = [&](const std::string &why) {
        return std::unexpected(Failure{"Maxima did not solve " + equationList
                                       + " for " + unknownList + ": " + why});
    };

    std::vector<Solution> solutions;
    solutions.reserve(result->arity());

    for (const Expr &candidate : result->args()) {
        // Each solution is a list of assignments — or, when flattened, a single
        // assignment standing on its own.
        std::vector<Expr> assignments;
        if (flattened) {
            assignments.push_back(candidate);
        } else if (isUnevaluated(candidate, "list")) {
            assignments.assign(candidate.args().begin(), candidate.args().end());
        } else {
            return reject("expected a list of assignments but found "
                          + candidate.str());
        }

        // Collected by name, so the caller's ordering is honoured whatever
        // order Maxima chose to answer in.
        std::vector<std::pair<std::string, Expr>> byName;
        for (const Expr &assignment : assignments) {
            if (!assignment.is(Kind::Relation)
                || assignment.relationOp() != RelOp::Equal
                || !assignment.arg(0).is(Kind::Symbol)) {
                return reject(assignment.str() + " is not an assignment");
            }
            // The same rule the single-unknown case has always applied: a value
            // that still mentions an unknown is Maxima saying it could not
            // finish, not a solution. `[x = sin(x)]` is the classic shape.
            for (const Symbol &unknown : unknowns) {
                if (contains(assignment.arg(1), unknown)) {
                    return reject(assignment.str()
                                  + " still depends on " + unknown.name());
                }
            }
            byName.emplace_back(assignment.arg(0).name(), assignment.arg(1));
        }

        Solution solution;
        solution.reserve(unknowns.size());
        for (const Symbol &unknown : unknowns) {
            const auto found = std::find_if(
                byName.begin(), byName.end(),
                [&](const auto &entry) { return entry.first == unknown.name(); });
            if (found == byName.end()) {
                return reject("no value for " + unknown.name() + " in "
                              + candidate.str());
            }
            solution.push_back(found->second);
        }
        solutions.push_back(std::move(solution));
    }
    return solutions;
}

std::expected<std::vector<Expr>, Failure>
solve(const Expr &equation, const Symbol &unknown, Kernel &kernel) {
    // Delegates, so that the rules deciding what counts as a solution live in
    // one place rather than being maintained twice.
    const Expr equations[] = {equation};
    const Symbol unknowns[] = {unknown};

    auto solutions = solve(std::span<const Expr>(equations),
                           std::span<const Symbol>(unknowns), kernel);
    if (!solutions) {
        return std::unexpected(solutions.error());
    }

    std::vector<Expr> values;
    values.reserve(solutions->size());
    for (const Solution &solution : *solutions) {
        values.push_back(solution.front());
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
