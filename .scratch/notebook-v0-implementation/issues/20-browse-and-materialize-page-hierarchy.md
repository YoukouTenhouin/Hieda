# 20 — Browse and materialize Page Hierarchy

**Parent spec:** [Notebook v0](../../notebook-v0/spec.md)

**What to build:** Let users browse the Page Hierarchy derived exclusively from materialized Named
Pages, navigate through accessible Page Preview ancestors without creating Blocks, and explicitly
materialize or delete Pages while the hierarchy and current destination update predictably. This
completes the hierarchy foundation accepted by
[ADR 0024](../../../docs/adr/0024-define-page-hierarchy-behavior.md).

**Blocked by:** 19 — Unify Page and Entry Block types

**Status:** ready-for-agent

- [ ] Page Names accept the slash-separated segment grammar and limits, while invalid names fail
  without changing acknowledged state.
- [ ] Exact hierarchy lookup distinguishes a materialized Named Page, a missing-prefix Page
  Preview required by a materialized descendant, and a name absent from the hierarchy.
- [ ] Root and immediate-child enumeration returns deterministic bytewise-ASCII segment order in
  revision-bound batches of 100, reports materialization and child presence, and rejects stale
  continuation cursors by requiring a fresh first batch.
- [ ] The native hierarchy UI loads children lazily, keeps expansion state session-local, reveals
  the current hierarchy destination, and exposes Display Title, local segment, full Page Name, and
  Page Preview status accessibly without relying on color.
- [ ] Activating a hierarchy Page Preview opens the same exact-name non-materialized destination
  used for unresolved links, creates no Block, and offers explicit creation.
- [ ] Creating from a Page Preview materializes only that exact Named Page, preserves descendant
  placement, and turns the current destination into the Page in place.
- [ ] Named Page creation is one Notebook-wide undoable action; undo and redo remove and restore
  the Page identity, timestamps, hierarchy effects, and exact logical state atomically.
- [ ] Deleting a Named Page never deletes hierarchy descendants: its node becomes a Page Preview
  while descendants require it and otherwise disappears. Deleting the current Page leaves its
  exact-name Page Preview current, and undo restores the Page in place.
- [ ] Deleting a Named Page removes its complete contained Entry forest atomically; undo restores
  all identities, timestamps, Containment, order, and hierarchy state.
- [ ] Creation, deletion, undo, redo, validation failures, persistence failures, revisions, and
  post-commit notifications obey the single-transaction contract.
- [ ] Notebook-interface, adapter, persistence, reopen, failure-atomicity, and packaged UI smoke
  tests cover the complete observable behavior.
