# Design the Notebook module interface

Type: prototype
Status: resolved

## Question

What small, toolkit-neutral C++ interface should the deep Notebook module present so UI adapters can edit blocks, navigate pages, search, execute queries, observe committed changes, handle errors, and participate in undo/redo without learning persistence or indexing internals?

## Comments

Resolved by [ADR 0001](../../../docs/adr/0001-notebook-module-interface.md). The external seam is a thread-safe synchronous `NotebookSession` with typed expected failures, owned values, capability-specific methods, and post-commit RAII observers. Prototype: `44bb318391bdc5d0930114a55f2c4298ccdfd9b1` on `prototype/notebook-session-interface`.
