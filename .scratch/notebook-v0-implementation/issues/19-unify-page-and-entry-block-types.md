# 19 — Unify Page and Entry Block types

**Parent spec:** [Notebook v0](../../notebook-v0/spec.md)

**What to build:** Refactor the prototype to implement
[ADR 0018](../../../docs/adr/0018-unify-page-and-entry-block-types.md): persist one Page Block type
with immutable Named and Journal kinds, persist one Entry Block type, and replace duplicated
Page/Journal outline behavior with a shared deep Notebook interface.

**Blocked by:** [Specify block lifecycle and structural invariants](../../notebook-v0/issues/04-specify-block-lifecycle-and-structural-invariants.md)

**Status:** ready-for-agent

- [ ] Existing Named Page and Journal behavior remains covered through the public Notebook
  interface.
- [ ] Entry subtrees move atomically between any two materialized Pages while retaining every
  Block identity, Authored Text value, creation time, and descendant relationship.
- [ ] Page kind and Entry Block type are immutable, and invalid persisted combinations are
  rejected.
- [ ] Query-facing type semantics distinguish Page kind and containing context rather than
  separate Page Entry and Journal Entry Block types.
- [ ] One Notebook-wide chronological session-history action restores or reapplies both sides of a
  cross-Page move.
- [ ] The prototype schema encoding is replaced consistently; unsupported pre-refactor prototype
  files may be rejected, while new-format reopening and failure atomicity are covered.
- [ ] Block records remove the prototype's constant active-state field; decoding rejects no
  supported released format because no compatibility contract exists yet.
- [ ] The Qt adapter uses the shared outline behavior without exposing persistence representation.
