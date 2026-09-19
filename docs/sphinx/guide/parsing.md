# Parsing

There are two parsers, and they answer different questions.

`Expr::parse` is a Pratt parser over a subset of Maxima's grammar —
arithmetic, comparisons, function application, lists, strings — and needs
**no kernel**. Statements are refused; it parses expressions, not programs.
`proxima::parse` hands the text to Maxima and so accepts everything, at the
cost of a round trip.

```cpp
const auto offline = proxima::Expr::parse("x^2 + 3*x + 2");   // result<Expr>
const auto maxima = proxima::parse("5!");                      // result<Expr>: 120
```

They differ in more than grammar: **`Expr::parse` only normalises, and
`proxima::parse` also simplifies** — what Maxima does to any expression it
reads, folding numbers and powers. `5!` is the factorial `5!` to the first and
`120` to the second; `x^2^3` is `x^(2^3)` to the first and `x^8` to the
second.

Neither *evaluates*. A call such as `diff(x^2, x)` comes back from both as
that call, not as `2*x`.

## Evaluating text

To have Maxima carry out an instruction given as text, ask the kernel:

```cpp
proxima::shared_kernel().ask(proxima::Query::text("diff(x^2, x)"));   // 2*x
```

See [the how-to](../how-to/call-unwrapped-maxima.md), which also covers what
to watch for: only the first statement of the text counts, and a query must
not change Maxima's state.

Precedences are Maxima's, including the two that catch people out: `^` is
right-associative (`x^2^3` is `x^(2^3)`) and unary minus binds looser than it
(`-x^2` is `-(x^2)`).

Text that does not parse is an ordinary failure, not an exception:
`Expr::parse` returns a failure with `Cause::Parse` and a message naming the
offset. Text nested too deeply — thousands of parentheses, say — is refused the
same way rather than overflowing the stack.

What the infix printer writes, `Expr::parse` reads back to the same
expression: `*Expr::parse(e.str()) == e`. The fuzzer checks that promise on
whatever it invents.
