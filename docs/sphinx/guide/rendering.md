# Rendering

`str()` gives Maxima-compatible infix, `to_tex()` gives LaTeX, and
`to_mathml()` gives Presentation MathML — a complete `<math>` element that
browsers typeset natively, written in plain ASCII with character references for
symbols like `&#x2212;` and `&#x3C0;`. All three are local — no kernel — and
all three are ordinary clients of `<proxima/render.hpp>`, which is the
supported way to add another.

```cpp
std::cout << e;                         // (1 + x)/(x - 1)          also e.str()
std::cout << std::format("{:tex}", e);  // \frac{1 + x}{x - 1}      also proxima::to_tex(e)
std::cout << std::format("{:mathml}", e); // <math ...><mfrac>...</mfrac></math>
std::cout << proxima::render(e, MyOwn{});    // whatever you like
```

`operator<<` and `std::formatter` work for `Expr`, `Symbol` and
`proxima::Integer`, so `std::println("{}", e)` works too. After a notation, or
instead of one, a format spec takes the usual string options: `{:>30}`,
`{:tex:*<40}`. An unknown notation is a `std::format_error`, so with a constant
format string it does not compile.

## Your own renderer

A renderer is a **plain struct** — it inherits nothing, overrides nothing, and
owes this library no base class. Conformance is a concept, and `render` walks
your type directly; `proxima::Renderer<T>` erases it, for when one value must
hold any of them. Supply `integer`, `real`, `symbol`, `verbatim`, `sum`,
`product`, `fraction`, `power`, `call`, `relation` and `group`; `root`,
`list`, `negate` and `postfix` (for `x!`, which is otherwise a call) are
synthesised from those if you omit them. Pass `std::ref(yours)` instead of the
object to keep a renderer that accumulates state. [The
how-to](../how-to/write-a-renderer.md) builds one step by step.

**It is generic over what you return.** TeX returns strings; a
two-dimensional text renderer returns boxes with a width, height and baseline,
so a fraction can stack and an exponent can actually be raised. An interface
fixed to `std::string` would rule that out.

There is one in [`examples/text2d.hpp`](../../../examples/text2d.hpp), and the
demo uses it. It is deliberately **not** part of the library — a plain struct
of about 300 lines, written the way you would write your own, and a good place
to start if you do. It stacks fractions, raises exponents, draws radicals and
brackets as tall as their contents, and leaves `negate()` for the library to
fill in. Here is a root of `a*x^2 + b*x + c = 0`, as Maxima solves it and the
demo prints it:

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

## The library decides when to bracket; you decide how

That is the part worth having: a TeX renderer written directly against the
expression tree had seven defects in under two hundred lines — `x - 1`
printing as `-1 + x`, `x/3` as a product containing a fraction, `1/x` as a
negative power, `-(x+1)` as `(-1)*(1+x)` — every one a presentation decision
rather than a question about TeX. They are made once, in a shared layer, and
every renderer inherits them. Two optional hooks, `strength_of` and
`context_for`, let you declare that your notation delimits itself: that is the
whole difference between `\frac{1+x}{x-1}` and `(1 + x)/(x - 1)`, from the
same walk.
