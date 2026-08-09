# 18 — Package and verify Notebook v0

**Parent spec:** [Notebook v0](../../notebook-v0/spec.md)

**What to build:** Produce a distributable Linux v0 and verify the entire supported user journey from Notebook creation through journaling, Pages, links, Linked References, Queries, search, backup, failure handling, close, reopen, and restore. Documentation must state the portability and plaintext-security boundaries clearly.

**Blocked by:** 04 — Undo and redo Notebook edits; 05 — Support production desktop text input; 08 — Reference Blocks and browse Linked References; 10 — Query Containment and Semantic References; 12 — Detect and rebuild stale search indexes; 14 — Run automatic versioned backups; 16 — Recover safely during startup; 17 — Meet the reference performance envelope; 19 — Unify Page and Entry Block types; [Prototype the Qt Quick journal experience](../../notebook-v0/issues/12-prototype-the-qt-quick-journal-experience.md); [Choose the build and dependency strategy](../../notebook-v0/issues/13-choose-the-build-and-dependency-strategy.md); [Define verification and performance gates](../../notebook-v0/issues/14-define-verification-and-performance-gates.md)

**Status:** ready-for-agent

- [ ] The selected Linux packaging workflow produces an installable artifact from a clean environment using documented commands.
- [ ] A packaged-build smoke test covers creating and reopening a Notebook, journaling, nested editing, undo/redo, Pages, Page Links, Block References, Linked References, Queries, and search.
- [ ] The smoke test also covers snapshot creation, automatic backup status, search-index rebuild, a failed save, startup recovery, and restoring a backup.
- [ ] Documentation explains that one closed Notebook file or completed snapshot carries all canonical content, while lock, cache, and search files are non-canonical.
- [ ] Documentation warns that open-file synchronization is unsupported and that Notebook files, caches, snapshots, and backups may contain plaintext.
- [ ] Full behavioral, adapter, Qt Quick, recovery, backup/restore, end-to-end, and performance verification passes against the packaged release candidate.
- [ ] The release contains none of the features explicitly excluded from the Notebook v0 specification.
