# 09 — Save and run basic Queries

**Parent spec:** [Notebook v0](../../notebook-v0/spec.md)

**What to build:** Let users author and save declarative Queries over Block type, Page kind and context, Journal Date, Authored Text, and lightweight Properties. Query views support the agreed filtering, ordering, and limits and explain invalid input without executing backend syntax or arbitrary code.

**Blocked by:** 02 — Capture and reopen flat Journal Entries; 19 — Unify Page and Entry Block types; [Specify the authored text language](../../notebook-v0/issues/05-specify-the-authored-text-language.md); [Design the Query language](../../notebook-v0/issues/10-design-the-query-language.md)

**Status:** ready-for-agent

- [ ] Authored properties are parsed losslessly and made available to Queries according to the resolved text language.
- [ ] A user can save and reopen a Query using the agreed application-defined syntax.
- [ ] Queries can filter by Block type, Page kind, containing context, Journal Date, text, and supported Properties without separate Page Entry and Journal Entry types.
- [ ] Results obey the specified deterministic ordering and limit behavior.
- [ ] Invalid or incomplete Query text remains editable and produces a useful error without exposing storage-engine syntax.
- [ ] Query results update after relevant committed content and property edits.
- [ ] Tests cover every supported basic predicate, composition rule, ordering, limits, errors, persistence, and live updates.
