# Traversal

`<proxima/traverse.hpp>` walks and rewrites expression trees locally: no
kernel, no round trip, no state. Every function here is pure.

## Every node, as a range

`nodes(e)` is the tree as an input range, a node before its operands and
operands in order, so the standard algorithms and views apply:

```cpp
const proxima::Expr e = pow(x, 2) + 3 * x + 2;

std::ranges::distance(proxima::nodes(e));                                    // the size of the tree
std::ranges::any_of(proxima::nodes(e), [](const proxima::Expr &n) { return n.is(proxima::Kind::Pow); });
```

It holds its own copy of the root, so a temporary is safe to walk. `visit(e,
f)` calls `f` on each node in the same order, and `any_of(e, p)` asks the same
question as the algorithm above and stops at the first yes.

## Folding a tree

`fold<R>(e, algebra)` is the one recursion the other walks are cases of: the
algebra is called on every node once its operands are folded, with their
results in operand order.

```cpp
// The number of nodes.
const auto size = proxima::fold<std::size_t>(
    e, [](const proxima::Expr &, std::span<const std::size_t> sizes) {
        return std::accumulate(sizes.begin(), sizes.end(), std::size_t{1});
    });

// The depth of the tree.
const auto depth = proxima::fold<std::size_t>(
    e, [](const proxima::Expr &, std::span<const std::size_t> depths) {
        return 1 + (depths.empty() ? 0 : std::ranges::max(depths));
    });
```

The algebra can use `Expr::match` on the node to be told its kind.

## Rewriting a tree

`rewrite(e, f)` rebuilds the tree bottom-up: `f` is called on every node once
its operands have been rewritten, and returns the node to put in its place, or
`std::nullopt` to keep it. Saying "unchanged" with an empty optional is what
lets the untouched parts of the tree be shared rather than rebuilt:

```cpp
// Every sin(u) becomes cos(u), the rest stays as it is.
const proxima::Expr swapped = proxima::rewrite(e, [](const proxima::Expr &n) -> std::optional<proxima::Expr> {
    if (n.is(proxima::Kind::Function) && n.name() == "sin") {
        return proxima::Expr::function("cos", {n.arg(0)});
    }
    return std::nullopt;
});
```

`transform(e, f)` is the same walk with `f` returning an `Expr`, handing back
the node it was given to keep it.

A node whose operands changed is rebuilt through the builders, so every result
is normalised — but nothing is evaluated: a rewrite that produces `sin(0)` or
`2^2` leaves exactly that. Evaluating is Maxima's work, or `eval_numeric`'s.

## Symbols

`contains(e, x)` says whether a symbol occurs anywhere, looking inside Opaque
nodes too. `replace(e, x, value)` substitutes locally:

```cpp
proxima::replace(3 * x + 2, x, proxima::Expr(2));    // 8: numbers fold
proxima::replace(proxima::sin(x), x, proxima::Expr(0));   // sin(0): not evaluated
```

`proxima::subst` does the same in Maxima, which evaluates as it goes.
