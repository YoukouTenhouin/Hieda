# 11 — Search all Notebook text

**Parent spec:** [Notebook v0](../../notebook-v0/spec.md)

**What to build:** Provide a global search palette that finds Unicode authored Block text through the rebuildable SQLite FTS5 sidecar, presents ranked matches and snippets, and navigates to the canonical target Block. Search syntax and behavior belong to the Notebook contract rather than SQLite.

**Blocked by:** 02 — Capture and reopen flat Journal Entries; [Design search behavior and indexing](../../notebook-v0/issues/09-design-search-behavior-and-indexing.md)

**Status:** ready-for-agent

- [ ] A user can open global search, enter the agreed search syntax, and receive matching canonical Blocks.
- [ ] Results use the resolved ranking, snippet, limit, and navigation behavior.
- [ ] Indexing and query processing share the specified ICU-backed Unicode normalization and tokenization contract.
- [ ] A committed Journal Entry creation, edit, or deletion is reflected promptly and the UI does not claim search is current before the applicable index revision is committed.
- [ ] The search sidecar contains no unique user content and can be removed without affecting canonical Notebook use.
- [ ] Tests cover multilingual golden cases, malformed input, ranking behavior, snippets, incremental updates, and target navigation.

