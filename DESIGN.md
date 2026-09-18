# Proxima design: the main types

How the main classes and types relate and talk to each other. This is not
full UML. Each diagram shows one subsystem, with only the members and calls
that explain how the pieces fit.

- [`ARCH.md`](ARCH.md) shows the layers these types sit in.
- [`docs/design.md`](docs/design.md) records why each was built the way it is.

## Notation

```
  A <>---- B     A owns B: B is part of A, and goes when A goes
  A o----- B     A shares B through a shared_ptr; B is immutable
  A ------ B     A refers to B, without owning it
  A <|---- B     B is a kind of A: inherits it, or satisfies the concept
  A ====> B      A produces B: a function taking A and returning B
  [x]            a free function, not a type
```

`detail::` types are internal. Everything else is public.

## 1. Expressions

`Expr` is the one type everything else is built around: an immutable value,
cheap to copy, compared and hashed by structure.

```
                 +----------------------------+
                 | Symbol                     |  holds one Expr, and
                 |  name()                    |  converts implicitly to
                 |  expr()                    |  const Expr & wherever
                 +----------------------------+  an expression is wanted
                              <>
                              |
 +--------------------------------------------------------------------+
 | Expr                                                               |
 |  kind() -> Kind                                                    |
 |  match(visitor)  -- one callable per node kind; the compiler       |
 |                     checks that every kind is handled              |
 |  as_integer() as_fraction() as_real() as_symbol()                  |
 |  operators + - * / ^, add(range), mul(range), str()                |
 +--------------------------------------------------------------------+
                              o
                              | shared, never mutated
                              v
 +--------------------------------------------------------------------+
 | detail::Node                                                       |
 |  kind, hash (computed once), and one payload:                      |
 |     Integer | Fraction | double | string | vector<Expr> | call     |
 +--------------------------------------------------------------------+
        ^                                          |
        | every Node is built here                 | operands are Exprs,
 +--------------+                                  | so trees share subtrees
 | [normalize]  |  sorts, folds numbers, drops     v
 |              |  identities: one canonical   (Expr again)
 +--------------+  form per value
```

The numbers inside:

```
 +---------------------------+          +---------------------------+
 | Integer                   |          | Fraction                  |
 |  small: int64, inline     |          |  numerator:   Integer     |
 |  big:   o-- detail::BigInt|          |  denominator: Integer     |
 |         (Boost cpp_int),  |          +---------------------------+
 |         only when needed  |
 +---------------------------+
```

`match` hands the visitor a view of the node's kind, never the `Node`
itself:

```
 Expr::match ====> node::Integer | node::Rational | node::Real | node::Symbol
                   node::Sum     | node::Product  | node::Power
                   node::Call    | node::Relation | node::Opaque
```

## 2. Assumptions and Env

Assumptions are a value, passed with each question. They are never state
you set beforehand.

```
 +------------------------------------+       +-------------------------+
 | Assumptions                        |       | Declaration             |
 |  facts:        vector<Expr>        |<>---->|  symbol:  Symbol        |
 |  declarations: vector<Declaration> |       |  feature: Feature       |
 |  sorted and deduplicated, so equal |       |   (Integer, Real, Even, |
 |  sets are equal values             |       |    Increasing, ...)     |
 +------------------------------------+       +-------------------------+
     ^ built by [assuming], [declaring], .with()

 +------------------------------------+
 | Env                                |  every operation's last parameter
 |  kernel_:      Kernel* ------------+---> the Kernel given, or
 |  assumptions_: <>-- Assumptions    |     shared_kernel() if none
 +------------------------------------+
     ^ converts implicitly from Kernel&, from Assumptions, or from both
```

## 3. The kernel

```
  [integrate] [diff] [solve] [limit] [is] [parse] ...      ops.hpp
        |
        |  Env.kernel().ask(Query, Env.assumptions())
        v
 +------------------------------+      +------------------------------+
 | Kernel                       |      | Query / Statement            |
 |  ask(Query, Assumptions)     |----->|  a form:  Expr, or           |
 |     -> result<Expr>          |      |  a text:  Maxima source      |
 |  tell(Statement)             |      +------------------------------+
 |     -> result<unit>          |
 |  restart(), set_timeout(),   |
 |  cache_stats()               |
 +------------------------------+
                <>
                |
 +------------------------------------------------------------------------+
 | detail::MaximaSession                                                  |
 |                                                                        |
 |  Config ..................... timeouts, cache sizes, where Maxima is   |
 |  ReplyCache ................. in-memory LRU of Replies                 |
 |  PersistentCache ............ optional; one file per Reply             |
 |  contexts ................... one Maxima context per assumption set    |
 |  TransportFactory ........... how to start (and restart) Maxima        |
 |  ITransport ................. the live pipe                            |
 |                                                                        |
 |  eval_pure(Payload, Environment) -> Reply    for ask: cached           |
 |  eval(Payload) -> Reply                      for tell: clears caches   |
 +------------------------------------------------------------------------+
        <>                                  |
        |                                   | the default factory:
 +----------------+                         |   [discover_maxima](Config)
 | ITransport     |                         |      ====> MaximaInstall
 |  send(bytes)   |                         |            sbcl, maxima.core
 |  receive(wait) |                         |   ====> ChildProcessTransport
 |  alive()       |                         |
 |  kill()        |                         |
 |  terminate()   |                         v
 +----------------+              +----------------------+
    <|--- ChildProcessTransport  | a real SBCL + Maxima |
    |      Boost.Process ------->| child process        |
    |                            +----------------------+
    <|--- FakeTransport
           scripted replies, for tests
```

The internal values that pass between these:

```
 Environment  { key, statements }   Assumptions, as the session needs them:
                                     a canonical key naming them, and the
                                     declare/assume statements to apply
 Payload      cppread("...")        the one part of a request that varies;
              eval_string("...")    only these two factories make one, and
                                     both escape their argument
 Reply        { ok, value, reason } one answer, as the caches store it
```

## 4. One question's types, in order

The same round trip as in `ARCH.md`, following the types rather than the
layers:

```
 Assumptions --[environment_for]--> Environment ------------+
                                                            |
 Expr --[to_maxima]--> std::string ----------> Payload -----+
                       ((%INTEGRATE) ...)      cppread(...) |
                                                            |
                                                            v
                                          MaximaSession::eval_pure
                                                            |
                                                            v
                                          Reply { ok, value, reason }
                                                            |
                                                [to_result] v
                                          result<std::string>
                                                            |
                                              [parse_sexpr] v
                                          SExpr      a generic s-expression
                                                            |
                                              [from_maxima] v
                                          Expr       normalised as it is built
                                                            |
                                                            v
                                          result<Expr>
                                                            |
                                  each operation reads it   v
                                  into its own type:  result<Expr>,
                                  result<Solution>, result<Truth>, ...
```

(`Solution` is a `vector<Expr>`, and `Truth` is `True`, `False` or `Unknown`.)

## 5. Results and errors

```
 result<T>  =  fxt::expected<T, Failure>
                                 |
                                 = fxt::failure
                                     message
                                     context: <>-- Cause
                                        MaximaError  NeedsAssumption
                                        Inconsistent NoClosedForm
                                        NotSolved    NoLimit  Parse
                                        Eval  Overflow  UnexpectedAnswer
                                        Argument  Unknown

 [fail](Cause, message) ====> Failure       [cause_of](Failure) ====> Cause

 [unwrap](result<T>) ====> T, or throws:
                                                  std::runtime_error
                                                          <|
                                                        Error
            +-------------+-----------+----------+--------+---------+
            |             |           |          |                  |
       MaximaError   ParseError   EvalError  OverflowError     KernelError
       (any other    (Parse)      (Eval)     (Overflow)          <|
        Cause)                                                TimeoutError

 KernelError and TimeoutError are not Causes: the kernel throws them
 directly, after repairing itself, when Maxima dies or does not answer.
```

Because `result<T>` is FXT's `expected`, FXT's adaptors work on it directly:
`r | fxt::and_then(...)`.

## 6. Rendering

```
 Expr ==[to_display]==> detail::DisplayNode tree
                         (fractions, roots, and signs in presentation form)
                                  |
                                  | walked by [render](expr, R)
                                  v
                        detail::Resolved<R, T>
                         fills in the operations R leaves out
                         (root, list, negate, precedence)
                                  |
                                  | direct calls: integer(), sum(), power(),
                                  | group(), ...
                                  v
 RendererFor<R, T>      a concept, not a base class
        <|------ InfixRenderer     ==> std::string    Expr::str(), operator<<
        <|------ TeXRenderer       ==> std::string    to_tex(), {:tex}
        <|------ MathMLRenderer    ==> std::string    to_mathml(), {:mathml}
        <|------ your own          ==> any T (a 2-D text box, say)

 The three built in are internal (src/core/); what they offer publicly is
 the function or format spec on the right.

 Renderer<T>   holds any one RendererFor<R, T> as a single type
               <>-- the renderer, stored inline when small
               <>-- a three-entry table that runs the same walk
```

## 7. Numbers and traversal

These use no kernel at all.

```
 Bindings { symbol -> double }
      |
      v
 [eval_numeric](Expr, Bindings) ====> result<double>       one value

 [compile](Expr, Symbols) ====> result<Compiled>
                                  +--------------------------------+
                                  | Compiled                       |
                                  |  <>-- vector<Instruction>      |
                                  |       a flat stack program     |
                                  |  operator()(values) -> double  |
                                  +--------------------------------+
                                  one tree walk at compile time;
                                  none per call

 [nodes](Expr) ====> Nodes             a range over every subexpression
 [fold](Expr, f) ====> R               bottom-up reduction
 [rewrite](Expr, f) ====> Expr         bottom-up; f returns a replacement,
                                       or nullopt to keep the node
 [transform](Expr, f) ====> Expr       bottom-up; f returns the node itself
                                       to keep it
                                       (both share every part they leave
                                        unchanged)
```

Every `Expr` these produce goes through the normaliser too, so rewritten
trees are canonical like any other.
