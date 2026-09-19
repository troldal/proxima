# Handle failures

**Task:** deal with an operation that has no answer, in whichever style suits
the surrounding code.

Every operation returns a `proxima::result<T>`: the answer, or a
`proxima::Failure` with a message and a `proxima::Cause`. Pick one of these
patterns.

## Check, and read the answer

```cpp
namespace px = proxima;

const auto integral = px::integrate(px::exp(px::sin(x)), x);
if (integral) {
    use(*integral);
} else {
    report(integral.error().message());
    // no closed form for the integral of %e^sin(x) with respect to x
}
```

## Branch on why

The cause says why without parsing the message:

```cpp
switch (px::cause_of(integral.error())) {
case px::Cause::NoClosedForm:    /* try numerically */ break;
case px::Cause::NeedsAssumption: /* supply a fact */   break;
default:                         /* give up */          break;
}
```

## Fall back to a default

```cpp
const px::Expr answer = integral.value_or(px::Expr::symbol("unknown"));
```

## Throw instead

For code that would rather catch, `proxima::unwrap` returns the answer or
throws the exception the cause names — `ParseError`, `EvalError`,
`OverflowError`, otherwise `MaximaError`, all derived from `proxima::Error`:

```cpp
try {
    const px::Expr F = px::unwrap(px::integrate(f, x));
} catch (const px::Error &e) {
    report(e.what());
}
```

## Chain operations, stopping at the first failure

Results are [FXT](https://github.com/troldal/FXT)'s `expected`, so its
adaptors chain operations and pass the first failure through:

```cpp
#include <fxt/monads/AndThen.hpp>
#include <fxt/monads/Transform.hpp>
#include <fxt/utils/Lift.hpp>

const auto tex = px::diff(f, x)
               | fxt::and_then(FXT_LIFT(px::factor))
               | fxt::transform(px::to_tex);          // result<std::string>
```

## What is not a failure

`proxima::KernelError` — Maxima could not be found or started, died, or did
not answer within `Config::timeout` — is thrown, not returned, because no
answer to any question could follow it. The kernel restarts itself; the next
call starts from a working session.
