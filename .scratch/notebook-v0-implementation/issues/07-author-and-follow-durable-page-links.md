# 07 — Author and follow durable Page Links

**Parent spec:** [Notebook v0](../../notebook-v0/spec.md)

**What to build:** Parse lightweight Page Link notation in authored text, persist its stable Page target, and let users follow the link. Incomplete or invalid notation remains editable, and a target Page rename does not break an existing Page Link.

**Blocked by:** 06 — Create, navigate, and rename Pages; [Specify the authored text language](../../notebook-v0/issues/05-specify-the-authored-text-language.md); [Define link resolution and linked references](../../notebook-v0/issues/11-define-link-resolution-and-linked-references.md)

**Status:** ready-for-agent

- [ ] Entering valid Page Link notation produces the agreed Semantic Reference to stable Page identity.
- [ ] Following a Page Link navigates to its target Page.
- [ ] Renaming a target Page updates presentation as agreed without changing or breaking the persisted target identity.
- [ ] Incomplete, escaped, malformed, missing-target, and ambiguous Page Link text remains losslessly editable and follows the resolved behavior.
- [ ] Editing or removing link notation updates the canonical Semantic Reference in the same acknowledged command.
- [ ] Parser and behavioral tests cover Unicode titles, escape rules, incremental input, rename stability, and reopen.

