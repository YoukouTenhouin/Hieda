# 10 — Query Containment and Semantic References

**Parent spec:** [Notebook v0](../../notebook-v0/spec.md)

**What to build:** Extend saved Queries to select Blocks through ordered Containment and Semantic Reference relationships. Results remain live as the outline and link graph change and retain deterministic behavior across reopen.

**Blocked by:** 08 — Reference Blocks and browse Linked References; 09 — Save and run basic Queries; [Design the Query language](../../notebook-v0/issues/10-design-the-query-language.md)

**Status:** ready-for-agent

- [ ] Queries can express every v0 Containment and Semantic Reference predicate defined by the resolved language.
- [ ] Structural predicates distinguish ancestry, direct parentage, and ordering only where those concepts are part of the agreed contract.
- [ ] Reference predicates distinguish Page Links, Block References, outgoing references, and incoming Linked References as specified.
- [ ] Results update after moves, reordering where relevant, link edits, deletion, and restoration.
- [ ] Query execution returns stable Block identities and owned values without exposing persistence internals.
- [ ] Tests cover relationship combinations, cycles or invalid inputs where applicable, deterministic results, live updates, and reopen.

