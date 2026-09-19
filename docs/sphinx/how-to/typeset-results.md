# Typeset results in LaTeX or MathML

**Task:** put an expression Proxima computed into a LaTeX document or a web
page.

Both renderers are local — no Maxima — and work on any expression:

```cpp
#include <proxima/mathml.hpp>
#include <proxima/tex.hpp>

namespace px = proxima;

const px::Symbol x("x");
const px::Expr e = (x + 1) / (x - 1);

px::to_tex(e);        // \frac{1 + x}{x - 1}
px::to_mathml(e);     // <math xmlns="http://www.w3.org/1998/Math/MathML"><mfrac>...</mfrac></math>
```

## With std::format and std::print

The notation is part of the format spec, so a whole document can be written
with format strings:

```cpp
std::println(out, "\\begin{{equation}}\n{:tex}\n\\end{{equation}}", e);
std::println(out, "<p>The answer is {:mathml}.</p>", e);
```

After the notation, a spec takes the usual string options: `{:tex:>40}`. An
unknown notation does not compile.

## What you get

- **LaTeX** uses `\frac`, `\sqrt`, `\left( \right)`, operator names like
  `\sin`, and Greek letters for symbols named `alpha`, `pi` and so on. Paste
  it inside a math environment.
- **MathML** is a complete `<math>` element in plain ASCII, with character
  references for symbols (`&#x2212;`, `&#x3C0;`), which browsers typeset
  natively with no script.

For another notation, or a different taste in the same one, [write your own
renderer](write-a-renderer.md).
