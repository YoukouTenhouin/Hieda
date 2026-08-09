# 22 — Query and benchmark Page Hierarchies

**Parent spec:** [Notebook v0](../../notebook-v0/spec.md)

**What to build:** Let users select materialized content by an inclusive Page Hierarchy subtree and
verify that normal hierarchy reads remain interactive at the Notebook v0 reference scale, completing
the Query and performance behavior accepted by
[ADR 0024](../../../docs/adr/0024-define-page-hierarchy-behavior.md).

**Blocked by:** 20 — Browse and materialize Page Hierarchy; 21 — Rename Page Hierarchies with undo;
07 — Author and follow durable Page Links; 09 — Save and run basic Queries

**Status:** ready-for-agent

- [ ] The Query language exposes an inclusive hierarchy-subtree predicate whose root is an exact
  valid Page Name and whose matching uses slash-segment boundaries rather than raw string prefixes.
- [ ] The predicate matches materialized Named Pages at or below the root even when the root is a
  Page Preview or absent from the current hierarchy, and never returns Page Previews as results.
- [ ] Applying the predicate to Entries tests their containing Named Page; Entries contained by
  Journal Pages do not match.
- [ ] Saved Queries preserve and reopen hierarchy predicates, report invalid Page Names through the
  agreed editable Query error behavior, and update after Page creation, rename, deletion, undo, and
  redo.
- [ ] Deterministic reference data exercises deep, sparse, and very wide Page Hierarchies within an
  approximately 250,000-Block Notebook.
- [ ] Exact hierarchy lookup and each 100-node enumeration batch are measured against the existing
  approximate 200-millisecond interaction goal on documented reference hardware.
- [ ] Performance results distinguish ordinary bounded reads from uncapped atomic rename cascades,
  which are measured and reported but are not required to meet the interaction target.
- [ ] Behavioral and performance tests cover inclusive roots, preview and absent roots, segment
  boundaries, Named and Journal containment, live updates, stale cursors, deterministic ordering,
  and reproducible latency reporting.
