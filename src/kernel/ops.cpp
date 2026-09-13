#include <mx/ops.hpp>

#include "wire/from_maxima.hpp"
#include "wire/sexpr.hpp"
#include "wire/to_maxima.hpp"

#include <mx/errors.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace mx {
namespace {

/// Sends a form to Maxima and maps the reply back into an expression.
///
/// The whole round trip in one place. Note what is *not* here: no printing.
/// The form goes out as structure — its internal s-expression — and comes
/// back the same way, so nothing between this function and Maxima ever
/// parses infix text. That is what makes a symbol called `x y`, or an Opaque
/// holding a `$`, a question Maxima can answer (or refuse, with a message)
/// rather than a stall.
std::expected<Expr, Failure> evaluate(Kernel &kernel, const Expr &form) {
    // evalPure, not eval: every operation here is a question rather than an
    // instruction, so the answer can be remembered. The promise that goes with
    // evalPure is exactly what these functions are — nothing below assigns,
    // assumes or defines anything.
    const Reply reply = kernel.evalPure(form);
    if (!reply.ok) {
        return std::unexpected(Failure{reply.reason});
    }
    return detail::fromMaxima(detail::parseSExpr(reply.value));
}

/// For the operations with no ordinary failure mode.
Expr evaluateOrThrow(Kernel &kernel, const Expr &form) {
    auto result = evaluate(kernel, form);
    if (!result) {
        throw MaximaError(result.error().message);
    }
    return std::move(*result);
}

/// `head(args...)` as a form. Unnormalised, since Expr::function does not
/// reorder its arguments — which for a Maxima call is essential.
Expr call(std::string head, std::vector<Expr> args) {
    return Expr::function(std::move(head), std::move(args));
}

/// True when `expr` is `head(...)` left unevaluated by Maxima — its way of
/// saying it could not do the job.
bool isUnevaluated(const Expr &expr, std::string_view head) {
    return expr.is(Kind::Function) && expr.name() == head;
}

bool isList(const Expr &expr) {
    return isUnevaluated(expr, "list");
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
    // The source travels as a string literal, which an Opaque of that shape
    // becomes, for parse_string to read on the far side.
    return evaluate(kernel, call("parse_string",
                                 {Expr::opaque(detail::stringLiteral(source))}));
}

Expr diff(const Expr &expr, const Symbol &wrt, unsigned order, Kernel &kernel) {
    return evaluateOrThrow(kernel, call("diff", {expr, wrt, Expr(order)}));
}

Expr expand(const Expr &expr, Kernel &kernel) {
    return evaluateOrThrow(kernel, call("expand", {expr}));
}

Expr factor(const Expr &expr, Kernel &kernel) {
    return evaluateOrThrow(kernel, call("factor", {expr}));
}

Expr simplify(const Expr &expr, Kernel &kernel) {
    return evaluateOrThrow(kernel, call("ratsimp", {expr}));
}

Expr subst(const Expr &expr, const Symbol &symbol, const Expr &value,
           Kernel &kernel) {
    // Maxima's argument order is (replacement, target, expression).
    return evaluateOrThrow(kernel, call("subst", {value, symbol, expr}));
}

std::expected<Expr, Failure> integrate(const Expr &expr, const Symbol &wrt,
                                       Kernel &kernel) {
    auto result = evaluate(kernel, call("integrate", {expr, wrt}));
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
    auto result = evaluate(kernel, call("integrate", {expr, wrt, from, to}));
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
    std::vector<Expr> args{expr, wrt, to};
    if (const std::string_view keyword = sideKeyword(side); !keyword.empty()) {
        args.push_back(Expr::symbol(std::string(keyword)));
    }
    auto result = evaluate(kernel, call("limit", std::move(args)));
    if (!result) {
        return result;
    }
    if (isUnevaluated(*result, "limit")) {
        return std::unexpected(Failure{"Maxima could not determine the limit of "
                                       + expr.str() + " as " + wrt.name()
                                       + " approaches " + to.str()});
    }
    // `und` and `ind` are both Maxima saying, definitely, that there is no
    // limit: `und` that the expression is undefined there, `ind` that it stays
    // bounded without settling — sin(1/x) at 0, or abs(x)/x, which is 1 on one
    // side and -1 on the other. `ind` used to come back as a success, and a
    // caller checking only the std::expected took the symbol for an answer.
    if (result->is(Kind::Symbol) && result->name() == "und") {
        return std::unexpected(Failure{"the limit of " + expr.str() + " as "
                                       + wrt.name() + " approaches " + to.str()
                                       + " does not exist"});
    }
    if (result->is(Kind::Symbol) && result->name() == "ind") {
        return std::unexpected(Failure{"the limit of " + expr.str() + " as "
                                       + wrt.name() + " approaches " + to.str()
                                       + " does not exist: it stays bounded but "
                                         "does not settle on a value"});
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

    const Expr equationList
        = call("list", std::vector<Expr>(equations.begin(), equations.end()));
    std::vector<Expr> unknownExprs;
    unknownExprs.reserve(unknowns.size());
    for (const Symbol &unknown : unknowns) {
        unknownExprs.push_back(unknown);
    }
    const Expr unknownList = call("list", std::move(unknownExprs));

    auto result = evaluate(kernel, call("solve", {equationList, unknownList}));
    if (!result) {
        return std::unexpected(result.error());
    }
    if (!isList(*result)) {
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
        return std::unexpected(Failure{"Maxima did not solve "
                                       + equationList.str() + " for "
                                       + unknownList.str() + ": " + why});
    };

    std::vector<Solution> solutions;
    solutions.reserve(result->arity());

    for (const Expr &candidate : result->args()) {
        // Each solution is a list of assignments — or, when flattened, a single
        // assignment standing on its own.
        std::vector<Expr> assignments;
        if (flattened) {
            assignments.push_back(candidate);
        } else if (isList(candidate)) {
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

// contains() lived here, although it needs no kernel; it is in
// src/core/traverse.cpp now, beside replace().

} // namespace mx
