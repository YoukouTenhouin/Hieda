# 04 — Undo and redo journal edits

**Parent spec:** [Notebook v0](../../notebook-v0/spec.md)

**What to build:** Let users undo and redo observable journal operations as coherent user actions. Undo history must describe Notebook behavior rather than storage mutations and must never expose a partially reverted outline.

**Blocked by:** 03 — Edit nested journal outlines; [Define the data safety contract](../../notebook-v0/issues/08-define-the-data-safety-contract.md)

**Status:** ready-for-agent

- [ ] Users can undo and redo text edits and each supported structural journal operation.
- [ ] A compound user action is undone and redone atomically according to the resolved editing contract.
- [ ] New edits after undo invalidate the redo branch predictably.
- [ ] Undo or redo failure leaves the previously acknowledged Notebook state intact and produces a visible error.
- [ ] The UI accurately exposes whether undo and redo are currently available.
- [ ] Behavioral tests verify observable before/after states through the Notebook interface rather than persisted implementation details.

