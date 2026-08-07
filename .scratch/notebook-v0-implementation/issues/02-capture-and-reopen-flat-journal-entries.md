# 02 — Capture and reopen flat Journal Entries

**Parent spec:** [Notebook v0](../../notebook-v0/spec.md)

**What to build:** Open on today's Journal Page and let the user create and edit flat Journal Entries whose authored Unicode text is durably committed to the canonical Notebook. Reopening the Notebook must reproduce the acknowledged entries in the same order.

**Blocked by:** 01 — Launch the app and create or open a Notebook; [Specify block lifecycle and structural invariants](../../notebook-v0/issues/04-specify-block-lifecycle-and-structural-invariants.md); [Specify the authored text language](../../notebook-v0/issues/05-specify-the-authored-text-language.md); [Define journal editing behavior](../../notebook-v0/issues/06-define-journal-editing-behavior.md)

**Status:** ready-for-agent

- [ ] Opening a Notebook presents the Journal Page for the user's current calendar date.
- [ ] A user can create and edit ordered top-level Journal Entries containing Unicode text.
- [ ] Each persisted Journal Page and Journal Entry receives stable globally unique identity and the agreed metadata.
- [ ] An acknowledged edit is visible only after its complete canonical transaction succeeds.
- [ ] Closing and reopening the Notebook reproduces all acknowledged entries, text, identity, and order.
- [ ] Behavioral tests cover creation, editing, ordering, durable reopen, Unicode content, and a rejected save through the Notebook interface.

