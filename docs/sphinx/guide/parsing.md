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

They differ in more than grammar: **`Expr::parse` parses, `proxima::parse`
also evaluates.** `5!` is the unevaluated factorial `5!` to the first and
`120` to the second.

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
