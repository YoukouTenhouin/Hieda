# Use one Notebook-wide session history

Status: accepted

Outline undo and redo use one chronological, Notebook-wide session history, superseding ADR 0009's
independent history per Journal Date and the later analogous per-Named-Page histories. Each command
records one semantic action containing every affected Page state, so a cross-Page subtree move,
Page creation, Page deletion, Page rename cascade, or virtual Journal materialization restores
atomically without duplicated or conflicting history entries. A rename action also owns its
automatic Page Link rewrites. This keeps the Notebook module interface deep and gives standard
chronological undo behavior as operations span roots and namespace-derived state.

Undo and redo remain new durable Notebook transactions and revisions, while the history itself is
ephemeral: it is cleared on close and shares the existing 32 MiB estimated memory budget. A failed
commit leaves acknowledged state and history position unchanged; a new successful command clears
the single redo branch. History is an editing convenience rather than persisted Notebook version
control.
