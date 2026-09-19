Proxima
=======

Symbolic mathematics in C++23, with `Maxima <https://maxima.sourceforge.io>`_
doing the work in a child process. You build expressions with operators, ask
for derivatives, integrals and solutions, and get expressions back.

- **Introduction** — what Proxima is, why, and how to build it; then a first
  program, step by step.
- **User guide** — each part of the library in depth.
- **How-to guides** — recipes for particular tasks.
- **Reference** — every public declaration, from the headers.

.. toctree::
   :maxdepth: 1
   :caption: Introduction

   intro/introduction
   intro/getting-started

.. toctree::
   :maxdepth: 1
   :caption: User guide

   guide/expressions
   guide/operations
   guide/parsing
   guide/assumptions
   guide/results
   guide/rendering
   guide/numeric
   guide/traversal
   guide/kernel
   guide/purity
   guide/architecture
   guide/design

.. toctree::
   :maxdepth: 1
   :caption: How-to guides

   how-to/solve-and-evaluate
   how-to/evaluate-many-points
   how-to/typeset-results
   how-to/write-a-renderer
   how-to/supply-missing-facts
   how-to/handle-failures
   how-to/use-threads
   how-to/cache-between-runs
   how-to/choose-maxima
   how-to/call-unwrapped-maxima

.. toctree::
   :maxdepth: 1
   :caption: Reference

   api/index
