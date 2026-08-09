# 08 — Reference Blocks and browse Linked References

**Parent spec:** [Notebook v0](../../notebook-v0/spec.md)

**What to build:** Let a user explicitly choose any eligible Block as a Block Reference target, follow that reference, and browse incoming Linked References for a Page or Block. Semantic References must never alter structural ownership.

**Blocked by:** 03 — Edit nested journal outlines; 07 — Author and follow durable Page Links; 19 — Unify Page and Entry Block types; [Define link resolution and linked references](../../notebook-v0/issues/11-define-link-resolution-and-linked-references.md)

**Status:** ready-for-agent

- [ ] A user can select an eligible target and insert a Block Reference using the resolved authored notation.
- [ ] Following a Block Reference navigates to and identifies the target in its structural context.
- [ ] Page Links and Block References appear as incoming Linked References on the correct target.
- [ ] Linked References update after link creation, editing, removal, target movement, and target lifecycle changes.
- [ ] Deleting a target makes incoming Block References missing without changing their source text
  or source Block Update Times; undo restores their original resolution atomically.
- [ ] Creating or deleting a Semantic Reference never changes either Block's Containment parent or order.
- [ ] All relationships survive close and reopen, and tests cover missing or restored targets under the resolved lifecycle rules.
