# 06 — Create, navigate, and rename Pages

**Parent spec:** [Notebook v0](../../notebook-v0/spec.md)

**What to build:** Let users create titled Pages, move between ordinary Pages and Journal Pages, and rename Pages while stable Page identity remains unchanged. The UI should make the current Page and navigation outcome clear.

**Blocked by:** 02 — Capture and reopen flat Journal Entries; [Specify block lifecycle and structural invariants](../../notebook-v0/issues/04-specify-block-lifecycle-and-structural-invariants.md); [Define link resolution and linked references](../../notebook-v0/issues/11-define-link-resolution-and-linked-references.md)

**Status:** ready-for-agent

- [ ] A user can create a titled Page according to the resolved title and collision rules.
- [ ] A user can navigate among titled Pages and Journal Pages and return to today's Journal Page.
- [ ] Renaming a Page preserves its stable identity and contained Blocks.
- [ ] Missing, duplicate, invalid, and conflicting titles produce the agreed user-visible behavior.
- [ ] Pages and their titles, identity, and Containment survive close and reopen.
- [ ] Tests cover Page creation, navigation, rename, title edge cases, and identity stability through the Notebook interface.

