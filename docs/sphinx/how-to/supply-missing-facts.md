# Deal with Maxima asking for a fact

**Task:** an operation fails because Maxima wanted to know something about a
symbol — whether `n` is positive, say — and you want an answer anyway.

Maxima normally *asks the user* such questions. Proxima cannot answer them
over a pipe, so the operation fails with `Cause::NeedsAssumption` and a
message quoting Maxima's question:

```cpp
#include <proxima/assumptions.hpp>
#include <proxima/ops.hpp>

namespace px = proxima;

const px::Symbol x("x"), n("n");

const auto first = px::integrate(pow(x, n), x);
// first.error().message():
//   this computation needs an assumption that was not supplied.
//   Maxima asked: Is n equal to -1?
```

## Supply the fact with the question

Pass the fact as an assumption. It applies to that call alone:

```cpp
px::integrate(pow(x, n), x, px::assuming(gt(n, 0)));      // x^(1 + n)/(1 + n)
px::integrate(pow(x, n), x, px::assuming(ne(n, -1)));     // the same, with less assumed
```

Several facts go together — `px::assuming({gt(a, 0), lt(b, 0)})` — and
properties of a symbol are declared: `px::declaring(k,
px::Feature::Integer)`. `a.with(b)` combines two sets.

## Retry only when that is the reason

When the fact is known only at run time, try without it and retry with it on
this cause alone, leaving every other failure as it was:

```cpp
auto antiderivative = px::integrate(pow(x, n), x);
if (!antiderivative
    && px::cause_of(antiderivative.error()) == px::Cause::NeedsAssumption) {
    antiderivative = px::integrate(pow(x, n), x, px::assuming(gt(n, 0)));
}
```

With FXT the same reads as a pipeline — see
[`examples/functional.cpp`](../../../examples/functional.cpp), whose recovery
section uses `fxt::or_else`.

## Asking what Maxima can deduce

`px::is(predicate, assumptions)` asks whether something follows from the
facts: `Truth::True`, `False`, or `Unknown`.

```cpp
px::is(gt(pow(n, 2), 0), px::assuming(gt(n, 0)));     // Truth::True
px::is(gt(n, 0));                                      // Truth::Unknown
```
