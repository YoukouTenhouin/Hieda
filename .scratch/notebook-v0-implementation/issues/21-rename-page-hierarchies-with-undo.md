# 21 — Rename Page Hierarchies with undo

**Parent spec:** [Notebook v0](../../notebook-v0/spec.md)

**What to build:** Let users rename a Named Page namespace as one atomic Notebook-wide Editing
History action, preserving Page identities and contained outlines while descendant names follow the
renamed namespace according to
[ADR 0024](../../../docs/adr/0024-define-page-hierarchy-behavior.md).

**Blocked by:** 07 — Author and follow durable Page Links; 20 — Browse and materialize Page Hierarchy

**Status:** ready-for-agent

- [ ] Renaming a Named Page rewrites the Page Name prefix of every materialized descendant while
  preserving all affected Page identities, Containment, Entry order, and Display Titles.
- [ ] Collision validation evaluates the final namespace: names vacated by the same cascade are
  available, Page Previews never collide, and any invalid generated name or conflict with an
  outside materialized Page rejects the complete command.
- [ ] Only the explicitly renamed Page may receive the submitted Display Title and new Block
  Update Time; mechanically propagated descendant names preserve their titles and update times.
- [ ] Page Links resolved to every renamed Page are rewritten without advancing source Block Update
  Times; unresolved links exactly matching final names resolve, while other unresolved links that
  merely share the old prefix remain unchanged.
- [ ] Rename is one Notebook-wide Editing History action containing the selected Page name and
  title, descendant propagation, indexes, hierarchy changes, and timestamps; undo and redo restore
  the complete corresponding logical state.
- [ ] Submitting the existing Page Name and identical Display Title succeeds as a no-op without a
  transaction, revision, timestamp change, notification, or history action.
- [ ] A successful rename commits once, advances the Notebook revision once, and produces one
  post-commit notification; every validation, capacity, and persistence failure leaves Page state,
  links, indexes, history, revision, and navigation unchanged.
- [ ] Renaming the current Page or a current descendant retains navigation by stable identity and
  reveals its resulting hierarchy position.
- [ ] Potentially large cascades preserve one uncapped atomic commit, run without blocking the Qt
  UI thread, and present an indeterminate busy state until acknowledged success or failure.
- [ ] Behavioral tests cover self-descendant moves, final-state name reuse, outside collisions,
  invalid descendant rewrites, title and timestamp preservation, undo/redo, failure atomicity,
  notification count, reopen, and current-destination behavior.
