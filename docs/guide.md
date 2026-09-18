# Proxima: the guide

Everything the [README](../README.md) leaves out: each part of the library in
turn, with examples. The examples here and in the README are compiled and
checked by `tests/test_docs.cpp`, so they describe the library as it is.

- [What it does](#expressions): expressions, operations, the two parsers,
  rendering, numeric evaluation, assumptions, the kernel
- [Failure is an outcome, not an exception](#failure-is-an-outcome-not-an-exception)
- [When Maxima needs a fact it has not been told](#when-maxima-needs-a-fact-it-has-not-been-told)
- [What is pure, and what is not](#what-is-pure-and-what-is-not)
- [Notes on the design](#notes-on-the-design)

## Expressions

`proxima::Expr` is an immutable value: copying is a pointer copy, the hash is
computed once, and equality is structural. It works in `std::unordered_map`,
`std::find` and anything else expecting a regular type — and because it is a
local value rather than a handle into the Maxima process, it survives a kernel
restart.

```cpp
const proxima::Symbol x("x"), y("y");

proxima::Expr f = pow(x, 2) + 3 * x + 2;          // operators
f = *proxima::Expr::parse("x^2 + 3*x + 2");       // or infix text, no kernel
```

Ten node kinds — `Integer`, `Rational`, `Real`, `Symbol`, `Add`, `Mul`, `Pow`,
`Function`, `Relation`, `Opaque` — stay that few because two of them absorb
everything else. `Function(head, args)` is an *uninterpreted* application, so
`bessel_j`, a matrix, a derivative or your own `f` need no new type; `Opaque`
holds Maxima source for whatever is not an application at all. A result from
Maxima is never unrepresentable.

**Arithmetic is exact, and unbounded.** `1/3 + 2/5` is `11/15`, not `0.7333…`,
and `2` is not `2.0`. `proxima::Integer` has no fixed width, so `30!` is a number you
can compute with rather than a value that wraps or has to be handed back to
Maxima. Values that fit in 64 bits never allocate.

Expressions are **normalised at construction**: nested sums flattened, numeric
terms folded, identities dropped, an integer power of a power combined
(`(x^2)^-1` is `x^-2`, as Maxima has it), operands canonically ordered. So `x + 1` and
`1 + x` are equal and hash alike. It is normalisation, not algebra: `x - x`
stays `x - x`, and nothing is expanded or factored, because that is Maxima's
job and having two things simplify would make results depend on which path
produced them.

`str()` renders Maxima-compatible infix, parenthesised by precedence.

Reading an expression is a `match`: one handler per kind, each given a view of
that node's parts by reference, and a set that misses a kind does not compile.
A generic `[](const auto &)` handler catches the rest.

```cpp
namespace node = proxima::node;
const std::string what = e.match(
    [](const node::Integer &n) { return "the integer " + n.value.to_string(); },
    [](const node::Symbol &s) { return "the symbol " + s.name; },
    [](const node::Sum &s) { return std::to_string(s.terms.size()) + " terms"; },
    [](const node::Call &c) { return "a call to " + c.head; },
    [](const auto &) { return std::string("something else"); });

e.as_integer();   // std::optional<Integer>; as_fraction, as_real and as_symbol likewise
```

The views are `node::Integer`, `Rational`, `Real`, `Symbol`, `Sum`, `Product`,
`Power`, `Call`, `Relation` and `Opaque`. The accessors `integer_value()`,
`name()`, `arg(i)` and the rest remain, for code that has already checked
`is(Kind::...)`, and throw if it has not.

## Operations

| | |
|---|---|
| `diff(f, x, n)` | derivative, to any order |
| `integrate(f, x)` / `integrate(f, x, a, b)` | indefinite and definite |
| `limit(f, x, a, side)` | two-sided, or from above or below |
| `solve(equation, x)` | values for one unknown |
| `solve(equations, unknowns)` | a system; one value per unknown, in the order asked for |
| `expand` `factor` `ratsimp` `subst` | algebraic rearrangement (`simplify` is an older name for `ratsimp`) |
| `trigsimp` `trigexpand` `radcan` `partfrac(f, x)` `coeff(f, x, n)` | more rearrangement |
| `taylor(f, x, a, n)` | Taylor expansion, as an ordinary expression |
| `sum(t, k, a, b)` `product(t, k, a, b)` | closed forms; `Cause::NoClosedForm` when there is none |
| `ode2(equation, y, x)` | first- and second-order ODEs, written with `derivative(y, x)` |
| `is(predicate)` | `Truth::True`, `False` or `Unknown` under the assumptions in force |

Every one of them returns a `proxima::result<T>` — `fxt::expected<T,
fxt::failure>` — so `*diff(f, x)` is the derivative and `diff(f, x) |
fxt::and_then(...)` is a pipeline; see *Failure is an outcome, not an
exception*.
| `to_float(f)` `nroots(p, a, b)` `realroots(p)` `find_root(f, x, a, b)` | numbers from Maxima: floats, root counts, isolated and numeric roots |
| `lhs(r)` `rhs(r)` | local, no kernel: the sides of a relation |
| `parse(text)` | Maxima's own parser, for anything the offline one will not take |
| `contains(e, x)` `replace(e, x, v)` | local, no kernel: find a symbol, or rewrite it (normalised, not evaluated) |
| `visit(e, f)` `any_of(e, p)` `transform(e, f)` | local, no kernel: walk every node, search, or rewrite bottom-up |

Builders for `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `sinh`, `cosh`, `tanh`,
`asinh`, `acosh`, `atanh`, `log`, `abs`, `erf`, `floor`, `ceiling`, `signum`,
`exp`, `sqrt`, and the constants `%pi`, `%e`, `%i`, `inf`, `minf`. They take an
`Expr` or a `Symbol` and nothing else, so they never compete with `<cmath>` for
a plain number: `proxima::sqrt(Expr(2))`, not `proxima::sqrt(2)`.
Relations are built by name — `eq`, `ne`, `lt`, `le`, `gt`, `ge` — so that `==`
can keep its ordinary meaning.

For the same reason there is no `<`: `x < 0` would compile and mean "sorts
before". The canonical order the normaliser uses is `proxima::canonical_order` instead,
with `proxima::CanonicalLess` for sorting and for ordered containers:
`std::set<Expr, proxima::CanonicalLess>`, `std::map<Expr, T, proxima::CanonicalLess>`.
Two expressions are equivalent in that order exactly when they are `==`.
(`std::less` is not specialised for them: libc++ 22 ignores such a
specialisation in its trees and calls `<` instead.)

Results chain, because what comes back is an expression rather than text —
and a chain of operations stops at the first that fails:

```cpp
proxima::diff(f, x) | fxt::and_then(FXT_LIFT(proxima::factor))
                    | fxt::and_then(FXT_LIFT(proxima::expand));
```

## Two parsers

`Expr::parse` is a Pratt parser over a subset of Maxima's grammar — arithmetic,
comparisons, function application, lists, strings — and needs **no kernel**.
Statements are refused; it parses expressions, not programs. `proxima::parse` hands
the text to Maxima and so accepts everything, at the cost of a round trip.

They differ in more than grammar: **`Expr::parse` parses, `proxima::parse` also
evaluates.** `5!` is `factorial(5)` to the first and `120` to the second.

Precedences are Maxima's, including the two that catch people out: `^` is
right-associative (`x^2^3` is `x^(2^3)`) and unary minus binds looser than it
(`-x^2` is `-(x^2)`).

## Rendering, including your own

`str()` gives Maxima-compatible infix, `to_tex()` gives LaTeX, and `to_mathml()`
gives Presentation MathML — a complete `<math>` element that browsers typeset
natively, written in plain ASCII with character references for symbols like
`&#x2212;` and `&#x3C0;`. All three are local — no kernel — and all three are
ordinary clients of `<proxima/render.hpp>`, which is the supported way to add
another.

```cpp
std::cout << e;                         // (1 + x)/(x - 1)          also e.str()
std::cout << std::format("{:tex}", e);  // \frac{1 + x}{x - 1}      also proxima::to_tex(e)
std::cout << std::format("{:mathml}", e); // <math ...><mfrac>...</mfrac></math>
std::cout << proxima::render(e, MyOwn{});    // whatever you like
```

`operator<<` and `std::formatter` work for `Expr`, `Symbol` and `proxima::Integer`.
After a notation, or instead of one, a format spec takes the usual string
options: `{:>30}`, `{:tex:*<40}`. An unknown notation is a `std::format_error`,
so with a constant format string it does not compile.

A renderer is a **plain struct** — it inherits nothing, overrides nothing, and
owes this library no base class. Conformance is a concept, and `render` walks your type
directly; `proxima::Renderer<T>` erases it, for when one value must hold any of
them. Supply `integer`, `real`,
`symbol`, `verbatim`, `sum`, `product`, `fraction`, `power`, `call`, `relation`
and `group`; `root`, `list` and `negate` are synthesised from those if you omit
them. Pass `std::ref(yours)` instead of the object to keep a renderer that
accumulates state.

**It is generic over what you return.** TeX returns strings; a two-dimensional
text renderer returns boxes with a width, height and baseline, so a fraction can
stack and an exponent can actually be raised. An interface fixed to
`std::string` would rule that out.

There is one in [`examples/text2d.hpp`](examples/text2d.hpp), and the demo uses
it. It is deliberately **not** part of the library — a plain struct of about
300 lines, written the way you would write your own, and a good place to start
if you do. It stacks fractions, raises exponents, draws radicals and brackets as
tall as their contents, and leaves `negate()` for the library to fill in. Here
is a root of `a*x^2 + b*x + c = 0`, as Maxima solves it and the demo prints it:

```
  str()      ((b^2 - 4*a*c)^(1/2) - b)/(2*a)
  to_tex()    \frac{\sqrt{b^{2} - 4 a c} - b}{2 a}
  to_mathml() <math xmlns="http://www.w3.org/1998/Math/MathML"><mfrac><mrow><msqrt><mrow><msup><mi>b</mi><mn>2</mn></msup><mo>&#x2212;</mo><mrow><mn>4</mn><mo>&#x2062;</mo><mi>a</mi><mo>&#x2062;</mo><mi>c</mi></mrow></mrow></msqrt><mo>&#x2212;</mo><mi>b</mi></mrow><mrow><mn>2</mn><mo>&#x2062;</mo><mi>a</mi></mrow></mfrac></math>
  text2d
        ___________
       /  2
     \/  b  - 4 a c - b
    --------------------
            2 a
```

**The library decides when to bracket; you decide how.** That is the part worth
having: a TeX renderer written directly against the expression tree had seven
defects in under two hundred lines — `x - 1` printing as `-1 + x`, `x/3` as a
product containing a fraction, `1/x` as a negative power, `-(x+1)` as
`(-1)*(1+x)` — every one a presentation decision rather than a question about
TeX. They are made once, in a shared layer, and every renderer inherits them.
Two optional hooks, `strength_of` and `context_for`, let you declare that your
notation delimits itself: that is the whole difference between
`\frac{1+x}{x-1}` and `(1 + x)/(x - 1)`, from the same walk.

## Numeric evaluation

Once a closed form exists, turning it into numbers is ordinary arithmetic — no
round trip per point.

```cpp
*proxima::eval_numeric(*integral, {{x, 1.0}});        // 2.22324, one shot
proxima::is_evaluable(e, bindings);                    // ask, without evaluating
```

For repeated evaluation — plotting, root-finding, quadrature — compile once:

```cpp
const proxima::Compiled f(*integral, x);
for (int i = 0; i < points; ++i) { plot(f(i * step)); }

const auto g = proxima::as_function(*integral, x);     // the same Compiled, bound to x
```

`Compiled` resolves every symbol to an argument slot and every function to a
table index up front, and evaluates integer powers by squaring rather than
`std::pow`. About **6× faster** than `eval_numeric` in a loop, and errors surface
when you compile rather than at every point. Thread-safe to share.

`%pi` and friends are recognised; an explicit binding overrides them. An unknown
function is an error rather than a guess.

## Assumptions

Assumptions are a value, passed with the question — not a scope held open in
the kernel:

```cpp
const auto positive = proxima::assuming(gt(x, 0));
proxima::simplify(sqrt(pow(x, 2)));                        // abs(x)
proxima::simplify(sqrt(pow(x, 2)), positive);              // x
proxima::simplify(sqrt(pow(x, 2)), {positive, kernel});    // x, on a kernel of your own

const auto more = positive.with(gt(n, 0)).with(k, proxima::Feature::Integer);
```

Every operation's last parameter is a `proxima::Env`: the kernel to ask,
`shared_kernel()` unless said otherwise, and the `Assumptions` to ask under,
none unless said otherwise. It converts from either or both. So the meaning of
a call is a function of its arguments, on every thread, whatever else is
running; nothing is set up beforehand and nothing is left behind.

`Assumptions` is immutable and canonical: the facts and declarations are kept
sorted and without duplicates, so the same facts in another order are equal,
hash alike, and share a Maxima context and cached answers. Facts that
contradict one another make the operation fail with `Cause::Inconsistent`; a
redundant fact is accepted quietly. `declaring` covers every entry of Maxima's
`features` list: `Integer`, `Even`, `Odd`, `Rational`, `Real`, `Complex`,
`Constant`, the function properties (`Increasing`, `OddFun`, …) and the
operator ones (`Commutative`, `Symmetric`, …).

## The kernel

- **Started on first use**, or constructed explicitly. `shared_kernel()` is the
  process-wide one; every operation's `Env` defaults to it.
- **Serialised**, so a `Kernel` is safe to share between threads. For real
  parallelism, give each thread its own — Maxima is one process doing one thing.
- **Survives its own death.** If Maxima hangs or exits, the failing call reports
  it and the kernel is restarted, so the next call starts from a working
  session. Nothing needs replaying: assumptions come with each question, and
  the Maxima context for a set of them is made again when next asked for.
- **Speaks structure, not text.** An expression reaches Maxima as its internal
  s-expression and comes back the same way; the infix printer is for people
  and is not on the path. So a symbol called `x y`, or an `Opaque` holding a
  `$`, is simply a symbol or simply a question — nothing this library sends
  can be misread by Maxima's parser, and nothing Maxima *cannot* read (a
  malformed string given as a `Query`) costs more than one round trip and a
  message.
- **Two verbs, and the type says which.** `kernel.ask(Query::text("gcd(12,
  18)"))` is 6, as an `Expr`: a question, for a Maxima function this library
  does not wrap, cached like any operation and asked under assumptions like
  any operation. `kernel.tell(Statement::text("a: 7"))` carries out a
  statement: it empties the cache and stops persistence, since nothing in it
  says what it changed. Both take an `Expr` (`Query::form`,
  `Statement::form`) as well as text. There is no "evaluate, and promise it
  changes nothing" to get wrong.
- **Remembers answers.** An LRU keyed on the question *and* the assumptions it
  was asked under, so asking under different assumptions never needs to
  discard anything; only a `tell` does. Bounded by `Config::cache_entries` and
  `Config::cache_bytes` (64 MB); zero entries disables it.
- **Optionally between runs.** Set `Config::cache_directory` and answers survive
  process exit and are shared with other processes using the same directory.
  Every key carries the Maxima version, this library's version *and* the
  assumptions, so an entry can only be read back under the conditions that
  produced it — `sqrt(x^2)` asked under `x > 0` is not visible to a question
  asked under nothing. A `tell` may change Maxima in a way no key can describe,
  so it stops persistence for that kernel; `Kernel::persistence_active()` says
  so, and `Kernel::restart()` resumes it with a fresh Maxima.
- **Cannot be deadlocked by a prompt.** Maxima asks the user for facts it lacks,
  and reads the answer from standard input; over a pipe that would block and
  then swallow the next request. Questions become errors instead — see *When
  Maxima needs a fact it has not been told*.

`Config` covers `maxima_root`, `timeout`, `startup_timeout`, `cache_entries`,
`cache_bytes`, `cache_directory`, `cache_directory_limit` (256 MB by default, evicting the least
recently used answers), `load_user_init` and `user_dir`. The user's own `maxima-init.mac` is **not** loaded
by default: a library should compute the same answer on every machine.

## Failure is an outcome, not an exception

Every operation that can fail for an ordinary reason returns a value saying
so: `proxima::result<T>`, which is `fxt::expected<T, fxt::failure>` from
[FXT](https://github.com/troldal/FXT). `proxima::Failure` *is* `fxt::failure`,
so a Proxima result takes part in any FXT pipeline with nothing to convert.
What Proxima adds is a `proxima::Cause`, carried as the failure's context, so
a program can branch on *why* without reading the message.

| | |
|---|---|
| `proxima::result<T>`, returned | an ordinary outcome: no closed form (`Cause::NoClosedForm`), nothing solved (`NotSolved`), a fact Maxima needed (`NeedsAssumption`), text that would not parse (`Parse`), an expression with no number in it (`Eval`), Maxima objecting to an argument (`MaximaError`) |
| `proxima::KernelError`, thrown | the conversation broke down: the kernel died, or nothing answered in time |

```cpp
const auto area = proxima::integrate(proxima::exp(proxima::sin(x)), x);
if (!area) {
    std::cerr << area.error().message() << '\n';        // no closed form for ...
    if (proxima::cause_of(area.error()) == proxima::Cause::NeedsAssumption) { ... }
}

```

### Composing with FXT

Proxima adds no pipe adaptors of its own: its results are FXT's, so FXT's
adaptors apply to them directly, and a chain stops at the first failure.
The operations take their subject first, so a single-argument one lifts as
it is, and one that takes a variable is bound with a lambda or
`std::bind_back`:

```cpp
#include <fxt/monads/AndThen.hpp>
#include <fxt/monads/Transform.hpp>
#include <fxt/monads/ValueOr.hpp>
#include <fxt/utils/Lift.hpp>

const std::string answer
    = proxima::diff(f, x)                                        // result<Expr>
    | fxt::and_then(FXT_LIFT(proxima::factor))                    // one argument: lift it
    | fxt::and_then([&](const proxima::Expr &e) { return proxima::diff(e, x); })
    | fxt::and_then(std::bind_back(FXT_LIFT(proxima::expand), proxima::Env(kernel)))
    | fxt::transform(proxima::to_tex)
    | fxt::value_or(std::string("no answer"));
```

`FXT_LIFT` is variadic, so the defaulted `order` and `kernel` parameters
need no mention; a lambda binds the extra argument anywhere, and
`std::bind_back` does where the standard library has it (libstdc++ 14,
MSVC's STL). A chain starts from an operation's result or from
`proxima::result<Expr>{f}`; `fxt::match`, `fxt::with`, `fxt::sequence` and
the rest work on the same values. Only the headers used need including —
`<fxt.hpp>` brings in `fxt::match`, which needs deducing-this (GCC 14,
Clang 18, MSVC 19.34).

[`examples/functional.cpp`](../examples/functional.cpp) runs through the
adaptors one at a time: validating with `ensure`, bringing throwing code in
with `attempt`, recovering from a particular `Cause` with `or_else`,
all-or-nothing work with `traverse`, and combining independent results with
`with` and `zip`. `traverse`, `zip`, `curry` and `match` need
`std::forward_like` and deducing this, so GCC 13 builds the demo without
them.

`*diff(f, x)` reads a result on the spot — `diff` fails only for a malformed
argument — and `proxima::unwrap(r)` throws a failure as the exception its
cause names (`ParseError`, `EvalError`, `OverflowError`, otherwise
`MaximaError`), for code that would rather catch than check. Nothing in the
library throws those of its own accord.

`Expr::parse`, `eval_numeric` and `compile` (the value-returning form of
`Compiled`'s constructor) follow the same rule, with no kernel involved.

## When Maxima needs a fact it has not been told

Maxima sometimes needs a fact it has not been told. Asked to integrate `x^n` it
would normally *interrogate the user* — "Is n equal to -1?" — which over a pipe
cannot be answered. The kernel turns the question into an error naming the
missing fact, so it tells you exactly what to supply:

```cpp
const auto stuck = proxima::integrate(pow(x, n), x);
proxima::cause_of(stuck.error());   // Cause::NeedsAssumption
stuck.error().message();
// "this computation needs an assumption that was not supplied.
//  Maxima asked: Is n equal to -1?"

proxima::integrate(pow(x, n), x, proxima::assuming(gt(n, 0)));   // x^(1 + n)/(1 + n)
```

The assumption belongs to that call alone. The next call, asked under nothing,
fails as the first did.

## What is pure, and what is not

Everything that reads, builds, rewrites, renders or evaluates an expression
is a pure function of its arguments: no I/O, no Maxima, and no state beyond
constants. The effects live in two headers, and `Kernel` is the only type
that holds state.

| Pure — no kernel, no state | Effects — talks to Maxima |
|---|---|
| `expr.hpp`, `integer.hpp`, `symbol.hpp`, `functions.hpp` | `kernel.hpp`: `Kernel`, a Maxima process and its caches, and `shared_kernel()` |
| `assumptions.hpp`: `Assumptions`, `assuming`, `declaring` | `ops.hpp`: every operation |
| `traverse.hpp`: `nodes`, `fold`, `rewrite`, `transform`, `replace` | |
| `numeric.hpp`: `eval_numeric`, `compile`, `Compiled`, `Bindings` | |
| `render.hpp`, `tex.hpp`, `mathml.hpp` | |
| `result.hpp`, `errors.hpp`, `config.hpp`, `version.hpp` | |

The process-wide state is exactly one thing: `shared_kernel()`, started on
first use. (`Compiled` keeps a thread-local scratch stack for its
evaluation, which no caller can observe.) Assumptions are not state
anywhere: they are values, passed with each question. This is a promise, not an
accident: it is what lets a pure result be cached, shared between threads,
and composed without asking what it might have changed.

## Notes on the design

[`design.md`](design.md) records the architecture and the reasoning, including the decisions
that shaped it most: that the library owns a C++ expression tree with Maxima as
an oracle rather than holding remote handles; that the wire format is Maxima's
internal s-expressions in both directions rather than its display output or
infix text, because `(%oN)` output is ambiguous and loses exact rationals and
because anything Maxima has to *parse* can fail outside the error trap; that
there is exactly one canonicaliser,
which is why no second symbolic engine is linked in; and that Maxima runs as a
separate process, which is a licensing requirement and not merely a convenience.

