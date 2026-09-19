# Expressions

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

A `Symbol` is a named unknown, and an expression wherever one is wanted, so
`pow(x, 2)`, `x * x + 3 * x + 2` and `gt(x, 0)` need no conversion spelled
out. The operations that differentiate, integrate, solve or take limits take a
`Symbol` rather than any expression, so `diff(f, 2 * x)` is a compile error
rather than a runtime surprise.

## Ten kinds of node

Ten node kinds — `Integer`, `Rational`, `Real`, `Symbol`, `Add`, `Mul`, `Pow`,
`Function`, `Relation`, `Opaque` — stay that few because two of them absorb
everything else. `Function(head, args)` is an *uninterpreted* application, so
`bessel_j`, a matrix, a derivative or your own `f` need no new type; `Opaque`
holds Maxima source for whatever is not an application at all. A result from
Maxima is never unrepresentable.

## Exact, unbounded arithmetic

`1/3 + 2/5` is `11/15`, not `0.7333…`, and `2` is not `2.0`.
`proxima::Integer` has no fixed width, so `30!` is a number you can compute
with rather than a value that wraps or has to be handed back to Maxima. Values
that fit in 64 bits never allocate.

## Normalised at construction

Nested sums are flattened, numeric terms folded, identities dropped, an integer
power of a power combined (`(x^2)^-1` is `x^-2`, as Maxima has it), and
operands canonically ordered. So `x + 1` and `1 + x` are equal and hash alike.
It is normalisation, not algebra: `x - x` stays `x - x`, and nothing is
expanded or factored, because that is Maxima's job and having two things
simplify would make results depend on which path produced them.

`str()` renders Maxima-compatible infix, parenthesised by precedence; see
[Rendering](rendering.md) for the others.

## Reading an expression

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
`is(Kind::...)`, and throw if it has not. To walk a whole tree rather than one
node, see [Traversal](traversal.md).

## Functions, constants and relations

Builders for `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `sinh`, `cosh`,
`tanh`, `asinh`, `acosh`, `atanh`, `log`, `abs`, `erf`, `floor`, `ceiling`,
`signum`, `exp`, `sqrt`, and the constants `%pi`, `%e`, `%i`, `inf`, `minf`.
They take an `Expr` or a `Symbol` and nothing else, so they never compete with
`<cmath>` for a plain number: `proxima::sqrt(Expr(2))`, not
`proxima::sqrt(2)`.

Relations are built by name — `eq`, `ne`, `lt`, `le`, `gt`, `ge` — so that `==`
can keep its ordinary meaning. For the same reason there is no `<`: `x < 0`
would compile and mean "sorts before". The canonical order the normaliser uses
is `proxima::canonical_order` instead, with `proxima::CanonicalLess` for
sorting and for ordered containers: `std::set<Expr, proxima::CanonicalLess>`,
`std::map<Expr, T, proxima::CanonicalLess>`. Two expressions are equivalent in
that order exactly when they are `==`. (`std::less` is not specialised for
them: libc++ 22 ignores such a specialisation in its trees and calls `<`
instead.)
