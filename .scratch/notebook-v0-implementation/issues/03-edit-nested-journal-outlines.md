# 03 — Edit nested journal outlines

**Parent spec:** [Notebook v0](../../notebook-v0/spec.md)

**What to build:** Extend Journal Entry editing into an ordered nested outline. Users can restructure ideas with split, join, indent, outdent, reorder, move, and delete operations while the Notebook preserves stable Block identity and the agreed Containment invariants.

**Blocked by:** 02 — Capture and reopen flat Journal Entries; [Specify block lifecycle and structural invariants](../../notebook-v0/issues/04-specify-block-lifecycle-and-structural-invariants.md); [Define journal editing behavior](../../notebook-v0/issues/06-define-journal-editing-behavior.md)

**Status:** ready-for-agent

- [ ] Users can split and join Journal Entries according to the agreed cursor and child-handling rules.
- [ ] Users can indent, outdent, reorder, and move Blocks using the agreed keyboard interactions.
- [ ] A Block has at most one Containment parent, and invalid structural moves are rejected without partial mutation.
- [ ] Moving or editing a Block preserves its stable identity.
- [ ] Delete and restoration behavior follows the resolved lifecycle rules, including treatment of descendants.
- [ ] The complete outline, including identity, order, and ancestry, survives close and reopen.
- [ ] Property or model-based tests exercise valid and invalid structural operations through the Notebook interface.

