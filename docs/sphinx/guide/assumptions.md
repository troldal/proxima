# Assumptions

Assumptions are a value, passed with the question — not a scope held open in
the kernel:

```cpp
const auto positive = proxima::assuming(gt(x, 0));
proxima::simplify(sqrt(pow(x, 2)));                        // abs(x)
proxima::simplify(sqrt(pow(x, 2)), positive);              // x
proxima::simplify(sqrt(pow(x, 2)), {positive, kernel});    // x, on a kernel of your own

const auto more = positive.with(gt(n, 0)).with(k, proxima::Feature::Integer);
```

## The environment of a call

Every operation's last parameter is a `proxima::Env`: the kernel to ask,
`shared_kernel()` unless said otherwise, and the `Assumptions` to ask under,
none unless said otherwise. It converts from either or both. So the meaning of
a call is a function of its arguments, on every thread, whatever else is
running; nothing is set up beforehand and nothing is left behind.

## Facts and declarations

`Assumptions` is immutable and canonical: the facts and declarations are kept
sorted and without duplicates, so the same facts in another order are equal,
hash alike, and share a Maxima context and cached answers. Facts that
contradict one another make the operation fail with `Cause::Inconsistent`; a
redundant fact is accepted quietly.

`assuming` takes one fact or a list of them. `declaring(k,
proxima::Feature::Integer)` covers every entry of Maxima's `features` list:
`Integer`, `Even`, `Odd`, `Rational`, `Real`, `Complex`, `Constant`, the
function properties (`Increasing`, `OddFun`, …) and the operator ones
(`Commutative`, `Symmetric`, …).

## When Maxima needs a fact it has not been told

Maxima sometimes needs a fact it has not been told. Asked to integrate `x^n` it
would normally *interrogate the user* — "Is n equal to -1?" — which over a
pipe cannot be answered. The kernel turns the question into an error naming
the missing fact, so it tells you exactly what to supply:

```cpp
const auto stuck = proxima::integrate(pow(x, n), x);
proxima::cause_of(stuck.error());   // Cause::NeedsAssumption
stuck.error().message();
// "this computation needs an assumption that was not supplied.
//  Maxima asked: Is n equal to -1?"

proxima::integrate(pow(x, n), x, proxima::assuming(gt(n, 0)));   // x^(1 + n)/(1 + n)
```

The assumption belongs to that call alone. The next call, asked under nothing,
fails as the first did. [The how-to](../how-to/supply-missing-facts.md) shows
how to retry with the fact supplied.
