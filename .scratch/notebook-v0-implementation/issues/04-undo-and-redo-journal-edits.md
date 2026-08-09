# 04 — Undo and redo Notebook edits

**Parent spec:** [Notebook v0](../../notebook-v0/spec.md)

**What to build:** Let users undo and redo observable Notebook operations through one bounded, session-local chronological Editing History. History describes semantic Notebook behavior rather than storage mutations and never exposes a partially reverted multi-Page action.

**Blocked by:** 03 — Edit nested journal outlines; 19 — Unify Page and Entry Block types; [Define the data safety contract](../../notebook-v0/issues/08-define-the-data-safety-contract.md)

**Status:** ready-for-agent

- [ ] Users can undo and redo text edits, structural edits, cross-Page moves, and supported Page lifecycle operations in chronological order.
- [ ] A compound action restores every affected Page atomically according to the resolved Editing History contract.
- [ ] New edits after undo invalidate the redo branch predictably.
- [ ] Undo or redo failure leaves the previously acknowledged Notebook state intact and produces a visible error.
- [ ] The UI accurately exposes whether undo and redo are currently available.
- [ ] Behavioral tests verify observable before/after states through the Notebook interface rather than persisted implementation details.
- [ ] Closing the Notebook clears history, and the shared 32 MiB budget evicts oldest actions according to the accepted policy.
