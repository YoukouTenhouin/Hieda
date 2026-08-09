# 03 — Edit nested journal outlines

**Parent spec:** [Notebook v0](../../notebook-v0/spec.md)

**What to build:** Extend contextual Journal Entry editing into an ordered nested Entry outline. Users can restructure ideas with split, join, indent, outdent, reorder, move, and delete operations while the Notebook preserves stable Block identity and the agreed Containment invariants.

**Blocked by:** 02 — Capture and reopen flat Journal Entries; [Specify block lifecycle and structural invariants](../../notebook-v0/issues/04-specify-block-lifecycle-and-structural-invariants.md); [Define journal editing behavior](../../notebook-v0/issues/06-define-journal-editing-behavior.md)

**Status:** completed

- [x] Users can split and join Journal Entries according to the agreed cursor and child-handling rules.
- [x] Users can indent, outdent, reorder, and move Blocks using the agreed keyboard interactions.
- [x] Every Entry has one Page-rooted Containment parent chain, and invalid structural moves are rejected without partial mutation.
- [x] Moving or editing an Entry subtree preserves its stable identities.
- [x] Single-Entry deletion is leaf-only, explicit subtree Cut removes complete subtrees, and restoration is reserved for Editing History.
- [x] The complete outline, including identity, order, and ancestry, survives close and reopen.
- [x] Property-based tests exercise valid and invalid structural operations through the Notebook interface.
