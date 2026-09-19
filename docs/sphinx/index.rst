Proxima
=======

Symbolic mathematics in C++23, with `Maxima <https://maxima.sourceforge.io>`_
doing the work in a child process. There is no Maxima syntax in the interface
and no strings standing in for expressions: you build expressions with
operators, ask for derivatives, integrals and solutions, and get expressions
back.

.. code-block:: cpp

   #include <proxima/functions.hpp>
   #include <proxima/numeric.hpp>
   #include <proxima/ops.hpp>

   namespace px = proxima;

   const px::Symbol x("x");
   const px::Expr f = pow(x, 2) * px::sin(x);

   if (const auto integral = px::integrate(f, x)) {
       std::println("{}", *integral);                          // cos(x)*(2 - x^2) + 2*x*sin(x)
       std::println("{}", *px::eval_numeric(*integral, {{x, 1.0}}));
   }

Every operation that can fail returns a ``proxima::result<T>``: the answer, or
a failure whose cause says why there is none.

.. toctree::
   :maxdepth: 2
   :caption: Using Proxima

   guide

.. toctree::
   :maxdepth: 2
   :caption: How it is built

   architecture
   design

.. toctree::
   :maxdepth: 2
   :caption: Reference

   api/index
