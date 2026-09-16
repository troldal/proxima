#include <proxima/ops.hpp>

#include "wire/to_maxima.hpp"

#include <proxima/errors.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <utility>

namespace proxima {
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
    // eval_pure, not eval: every operation here is a question rather than an
    // instruction, so the answer can be remembered. The promise that goes with
    // eval_pure is exactly what these functions are — nothing below assigns,
    // assumes or defines anything.
    return to_expr(kernel.eval_pure(form));
}

/// For the operations with no ordinary failure mode.
Expr evaluate_or_throw(Kernel &kernel, const Expr &form) {
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
bool is_unevaluated(const Expr &expr, std::string_view head) {
    return expr.is(Kind::Function) && expr.name() == head;
}

bool is_list(const Expr &expr) {
    return is_unevaluated(expr, "list");
}

std::string_view side_keyword(Side side) {
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

Kernel &shared_kernel() {
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
                                 {Expr::opaque(detail::string_literal(source))}));
}

Expr diff(const Expr &expr, const Symbol &wrt, unsigned order, Kernel &kernel) {
    return evaluate_or_throw(kernel, call("diff", {expr, wrt, Expr(order)}));
}

Expr expand(const Expr &expr, Kernel &kernel) {
    return evaluate_or_throw(kernel, call("expand", {expr}));
}

Expr factor(const Expr &expr, Kernel &kernel) {
    return evaluate_or_throw(kernel, call("factor", {expr}));
}

Expr ratsimp(const Expr &expr, Kernel &kernel) {
    return evaluate_or_throw(kernel, call("ratsimp", {expr}));
}

Expr simplify(const Expr &expr, Kernel &kernel) {
    return ratsimp(expr, kernel);
}

Expr subst(const Expr &expr, const Symbol &symbol, const Expr &value,
           Kernel &kernel) {
    // Maxima's argument order is (replacement, target, expression).
    return evaluate_or_throw(kernel, call("subst", {value, symbol, expr}));
}

std::expected<Expr, Failure> integrate(const Expr &expr, const Symbol &wrt,
                                       Kernel &kernel) {
    auto result = evaluate(kernel, call("integrate", {expr, wrt}));
    if (!result) {
        return result;
    }
    // Maxima does not treat "I cannot do this" as an error; it hands the
    // integral back unevaluated. That noun form is the failure signal.
    if (is_unevaluated(*result, "integrate")) {
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
    if (is_unevaluated(*result, "integrate")) {
        return std::unexpected(
            Failure{"no closed form for the integral of " + expr.str()
                    + " over [" + from.str() + ", " + to.str() + "]"});
    }
    return result;
}

std::expected<Expr, Failure> limit(const Expr &expr, const Symbol &wrt,
                                   const Expr &to, Side side, Kernel &kernel) {
    std::vector<Expr> args{expr, wrt, to};
    if (const std::string_view keyword = side_keyword(side); !keyword.empty()) {
        args.push_back(Expr::symbol(std::string(keyword)));
    }
    auto result = evaluate(kernel, call("limit", std::move(args)));
    if (!result) {
        return result;
    }
    if (is_unevaluated(*result, "limit")) {
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

    const Expr equation_list
        = call("list", std::vector<Expr>(equations.begin(), equations.end()));
    std::vector<Expr> unknown_exprs;
    unknown_exprs.reserve(unknowns.size());
    for (const Symbol &unknown : unknowns) {
        unknown_exprs.push_back(unknown);
    }
    const Expr unknown_list = call("list", std::move(unknown_exprs));

    auto result = evaluate(kernel, call("solve", {equation_list, unknown_list}));
    if (!result) {
        return std::unexpected(result.error());
    }
    if (!is_list(*result)) {
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
                                       + equation_list.str() + " for "
                                       + unknown_list.str() + ": " + why});
    };

    std::vector<Solution> solutions;
    solutions.reserve(result->arity());

    for (const Expr &candidate : result->args()) {
        // Each solution is a list of assignments — or, when flattened, a single
        // assignment standing on its own.
        std::vector<Expr> assignments;
        if (flattened) {
            assignments.push_back(candidate);
        } else if (is_list(candidate)) {
            assignments.assign(candidate.args().begin(), candidate.args().end());
        } else {
            return reject("expected a list of assignments but found "
                          + candidate.str());
        }

        // Collected by name, so the caller's ordering is honoured whatever
        // order Maxima chose to answer in.
        std::vector<std::pair<std::string, Expr>> by_name;
        for (const Expr &assignment : assignments) {
            if (!assignment.is(Kind::Relation)
                || assignment.relation_op() != RelOp::Equal
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
            by_name.emplace_back(assignment.arg(0).name(), assignment.arg(1));
        }

        Solution solution;
        solution.reserve(unknowns.size());
        for (const Symbol &unknown : unknowns) {
            const auto found = std::find_if(
                by_name.begin(), by_name.end(),
                [&](const auto &entry) { return entry.first == unknown.name(); });
            if (found == by_name.end()) {
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

std::expected<Expr, Failure> ode2(const Expr &equation, const Symbol &dependent,
                                  const Symbol &independent, Kernel &kernel) {
    auto result = evaluate(kernel, call("ode2", {equation, dependent, independent}));
    if (!result) {
        return result;
    }
    // Maxima prints why to the console, and answers false.
    if (result->is(Kind::Symbol) && result->name() == "false") {
        return std::unexpected(Failure{"ode2 could not solve " + equation.str()
                                       + " for " + dependent.name() + " as a function of "
                                       + independent.name()});
    }
    return result;
}

Truth is(const Expr &predicate, Kernel &kernel) {
    // eval_pure is safe although the answer depends on the assumptions: the
    // reply cache is discarded whenever a Context changes them.
    const Expr answer = evaluate_or_throw(kernel, call("is", {predicate}));
    if (answer.is(Kind::Symbol)) {
        if (answer.name() == "true") {
            return Truth::True;
        }
        if (answer.name() == "false") {
            return Truth::False;
        }
        if (answer.name() == "unknown") {
            return Truth::Unknown;
        }
    }
    throw MaximaError("is(" + predicate.str() + ") answered " + answer.str()
                      + ", which is not a truth value");
}

Expr taylor(const Expr &expr, const Symbol &wrt, const Expr &at, unsigned order,
            Kernel &kernel) {
    // Maxima answers a taylor series in its own truncated-series form; the
    // protocol hands every reply through ratdisrep, which makes it a sum.
    return evaluate_or_throw(kernel, call("taylor", {expr, wrt, at, Expr(order)}));
}

Expr trigsimp(const Expr &expr, Kernel &kernel) {
    return evaluate_or_throw(kernel, call("trigsimp", {expr}));
}

Expr trigexpand(const Expr &expr, Kernel &kernel) {
    return evaluate_or_throw(kernel, call("trigexpand", {expr}));
}

Expr radcan(const Expr &expr, Kernel &kernel) {
    return evaluate_or_throw(kernel, call("radcan", {expr}));
}

Expr partfrac(const Expr &expr, const Symbol &wrt, Kernel &kernel) {
    return evaluate_or_throw(kernel, call("partfrac", {expr, wrt}));
}

Expr to_float(const Expr &expr, Kernel &kernel) {
    return evaluate_or_throw(kernel, call("float", {expr}));
}

Expr coeff(const Expr &expr, const Expr &term, int power, Kernel &kernel) {
    return evaluate_or_throw(kernel, call("coeff", {expr, term, Expr(power)}));
}

namespace {

/// sum or product, closed or a Failure.
std::expected<Expr, Failure> closed_form(const std::string &head, const Expr &term,
                                        const Symbol &index, const Expr &from,
                                        const Expr &to, Kernel &kernel) {
    // `simpsum` is what has Maxima look for a closed form when a bound is
    // symbolic; without it `sum(k, k, 1, n)` stays the noun. ev turns it on
    // for this evaluation alone, so no setting outlives the call.
    const Expr series = call(head, {term, index, from, to});
    auto result = evaluate(kernel, call("ev", {series, Expr::symbol("simpsum")}));
    if (!result) {
        return result;
    }
    if (is_unevaluated(*result, head)) {
        return std::unexpected(Failure{"no closed form for " + series.str()});
    }
    return result;
}

} // namespace

std::expected<Expr, Failure> sum(const Expr &term, const Symbol &index,
                                 const Expr &from, const Expr &to, Kernel &kernel) {
    return closed_form("sum", term, index, from, to, kernel);
}

std::expected<Expr, Failure> product(const Expr &term, const Symbol &index,
                                     const Expr &from, const Expr &to, Kernel &kernel) {
    return closed_form("product", term, index, from, to, kernel);
}

std::size_t nroots(const Expr &polynomial, const Expr &low, const Expr &high,
                   Kernel &kernel) {
    const Expr count = evaluate_or_throw(kernel, call("nroots", {polynomial, low, high}));
    if (count.is(Kind::Integer)) {
        const auto value = count.integer_value().to_int64();
        if (value && *value >= 0) {
            return static_cast<std::size_t>(*value);
        }
    }
    throw MaximaError("nroots answered " + count.str() + ", which is not a count");
}

std::vector<Expr> realroots(const Expr &polynomial, Kernel &kernel) {
    const Expr roots = evaluate_or_throw(kernel, call("realroots", {polynomial}));
    const auto not_roots = [&roots] {
        return MaximaError("realroots answered " + roots.str()
                           + ", which is not a list of roots");
    };
    if (!is_list(roots)) {
        throw not_roots();
    }
    // Each root arrives as `x = value`.
    std::vector<Expr> values;
    values.reserve(roots.arity());
    for (const Expr &root : roots.args()) {
        if (!root.is(Kind::Relation) || root.relation_op() != RelOp::Equal) {
            throw not_roots();
        }
        values.push_back(root.arg(1));
    }
    return values;
}

std::expected<double, Failure> find_root(const Expr &expr, const Symbol &wrt,
                                        double low, double high, Kernel &kernel) {
    auto result
        = evaluate(kernel, call("find_root", {expr, wrt, Expr(low), Expr(high)}));
    if (!result) {
        return std::unexpected(result.error());
    }
    if (result->is(Kind::Real)) {
        return result->real_value();
    }
    // Handed back unevaluated: the expression was not a number at some point
    // of the interval, so there was nothing to bisect.
    // std::format, not std::to_string: to_string prints six decimals, so an
    // interval from 1e-9 to 1e-8 read "between 0.000000 and 0.000000".
    return std::unexpected(Failure{std::format(
        "find_root could not evaluate {} to a number between {} and {}",
        expr.str(), low, high)});
}

} // namespace proxima
