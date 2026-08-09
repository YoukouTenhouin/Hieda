# 06 — Create, navigate, and rename Pages

**Parent spec:** [Notebook v0](../../notebook-v0/spec.md)

**What to build:** Let users create titled hierarchical Named Pages, navigate among Named and Journal Pages, and rename Named Page namespaces while stable Page identities remain unchanged. The UI should make the current Page and Page Hierarchy outcome clear.

**Blocked by:** 02 — Capture and reopen flat Journal Entries; 19 — Unify Page and Entry Block types; [Define Page Hierarchy behavior](../../notebook-v0/issues/16-define-page-hierarchy-behavior.md); [Specify block lifecycle and structural invariants](../../notebook-v0/issues/04-specify-block-lifecycle-and-structural-invariants.md); [Define link resolution and linked references](../../notebook-v0/issues/11-define-link-resolution-and-linked-references.md)

**Status:** ready-for-agent

- [x] A user can create a titled Named Page according to the original flat-name and collision rules.
- [x] A user can navigate among Named Pages and Journal Pages and return to today's Journal Page.
- [x] Renaming a Page preserves its stable identity and contained Blocks.
- [x] Missing, duplicate, invalid, and conflicting flat names and titles produce the agreed user-visible behavior.
- [x] Pages and their titles, identity, and Containment survive close and reopen.
- [x] Tests cover Page creation, navigation, rename, title edge cases, and identity stability through the Notebook interface.
- [ ] Page Names support the accepted slash-separated grammar and Page Hierarchy presents missing prefixes as Page Previews without materializing Blocks.
- [ ] Renaming a Named Page atomically propagates its prefix through descendant Page names, links, indexes, and session history under issue 16's final collision and UI rules.
