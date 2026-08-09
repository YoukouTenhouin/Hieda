# Notebook v0

Status: ready-for-agent

## Problem Statement

People who think and work through daily notes need the speed and connected structure of a journal-based outliner without accepting an Electron application, a directory of Markdown files as the canonical model, or fragile relationships encoded only in filenames and text. They need to capture ideas quickly, organize them into pages, follow durable links, discover incoming references, retrieve old material through queries and search, and trust that their notes remain safe and portable.

The first release must establish a dependable foundation without prematurely solving rich document editing, object storage, encryption, or synchronization. It must also avoid coupling the content model to Qt Quick or LMDB so later UI adapters and storage engines do not require rewriting product behavior.

## Solution

Build a keyboard-first native Linux desktop application for a single user and one open Notebook at a time. The application opens on today's Journal Page and provides a fast outliner for short, plain-text Entries. Every persisted content entity is a typed Block with globally unique stable identity. Ordered Containment gives each Entry one Page-rooted structural home, while Page Links, Block References, and other Semantic References connect Blocks without changing ownership.

Pages, Linked References, saved declarative Queries, and interactive full-text search make captured ideas discoverable. One canonical Notebook file stores all durable user content and notebook settings. LMDB is the initial storage engine; operational lock files, rebuildable indexes, caches, and versioned backup snapshots may exist separately. Copying a closed Notebook file or an application-created snapshot is sufficient to transfer all canonical content.

A deep, toolkit-neutral C++ Notebook module owns content behavior, transactions, indexing coordination, persistence, search, and undo/redo. Qt Quick is the first UI adapter and remains outside that module. The first release emphasizes crash safety, backup and restore, predictable semantics, and interactive performance at personal-knowledge-base scale.

## User Stories

1. As a note-taker, I want the application to open on today's Journal Page, so that I can begin capturing ideas immediately.
2. As a note-taker, I want to create a Journal Entry with minimal interaction, so that capturing a thought does not interrupt my work.
3. As a keyboard-oriented note-taker, I want to create, split, join, indent, outdent, and move entries from the keyboard, so that I can maintain flow while writing.
4. As a mouse user, I want essential journal and navigation interactions to remain usable with a pointer, so that keyboard optimization does not make the application inaccessible to me.
5. As a note-taker, I want Journal Entries to contain simple Unicode text, so that notes remain quick to write and understandable without a rich-text editor.
6. As a multilingual note-taker, I want text, links, queries, and search to handle Unicode consistently, so that the application works with the languages and symbols I use.
7. As a note-taker, I want Entries to contain ordered child Entries, so that I can expand an idea into a nested outline.
8. As a note-taker, I want every Entry to have one unambiguous Page-rooted structural home, so that moving, ordering, restoring, and deleting content behave predictably.
9. As a note-taker, I want to reorder sibling Entry subtrees, so that the visible outline reflects the sequence I intend.
10. As a note-taker, I want to move an Entry subtree within or between Pages without changing its identities, so that links to it remain valid.
11. As a note-taker, I want ordinary editing undo and redo, so that I can recover quickly from mistakes.
12. As a note-taker, I want each calendar date to have a distinct Journal Page, so that entries retain their daily context.
13. As a note-taker, I want to navigate to earlier or later Journal Pages, so that I can review notes chronologically.
14. As a note-taker, I want to create titled hierarchical Named Pages in addition to Journal Pages, so that I can organize enduring topics.
15. As a note-taker, I want to enter a Page Link using lightweight notation such as `[[project_alpha]]`, so that linking while typing remains fast.
16. As a note-taker, I want Page renames to rewrite Page Links without changing their resolved target, so that names remain literal and renaming does not break existing links.
17. As a note-taker, I want to create a Block Reference by explicitly selecting a Block, so that I can link directly to a particular idea.
18. As a note-taker, I want to follow Page Links and Block References, so that I can move through connected material.
19. As a note-taker, I want to see incoming Linked References for a Page or Block, so that I can discover where an idea is discussed elsewhere.
20. As a note-taker, I want Containment and Semantic References to remain visibly and behaviorally distinct, so that a link never unexpectedly changes content ownership or order.
21. As a note-taker, I want lightweight properties in authored text, so that I can attach structured facts that Queries can select.
22. As a note-taker, I want incomplete or invalid inline notation to remain editable, so that transient typing states do not lose content.
23. As a note-taker, I want to save a declarative Query, so that a useful selection of Blocks remains available as a live view.
24. As a note-taker, I want a Query to filter Blocks by type, Journal date, text, properties, Containment, and Semantic References, so that I can retrieve structurally relevant material.
25. As a note-taker, I want Query results to support ordering and limits, so that live views remain focused and useful.
26. As a note-taker, I want a saved Query to update when matching content changes, so that its results do not silently become obsolete.
27. As a note-taker, I want a malformed Query to produce a clear editable error rather than execute arbitrary code, so that experimentation is safe.
28. As a note-taker, I want Queries to use an application-defined declarative language rather than a storage-engine query language, so that I do not need to understand LMDB or another backend.
29. As a note-taker, I want global full-text search to be available separately from saved Queries, so that quick retrieval and structured live views each have an interaction suited to their purpose.
30. As a note-taker, I want search results to identify and navigate to matching Blocks, so that I can act on what I find.
31. As a note-taker, I want useful result ordering and snippets, so that I can distinguish relevant matches quickly.
32. As a note-taker, I want edits to become searchable promptly, so that newly captured material can be found without manual maintenance.
33. As a note-taker, I want the application to report when search is rebuilding, stale, or unavailable, so that incomplete results are never mistaken for complete results.
34. As a note-taker, I want typical editing, navigation, Linked Reference, Query, and search interactions to complete within roughly 200 milliseconds on the reference workload, so that the application continues to feel immediate after years of use.
35. As a note-taker, I want one Notebook file to contain every canonical Block, relationship, and notebook setting, so that I can understand what must be protected and transferred.
36. As a note-taker, I want to copy a closed Notebook file to another supported device and open all its canonical content there, so that manual transfer remains simple.
37. As a note-taker, I want an application-created snapshot of an open Notebook, so that I can back it up without making an unsafe raw copy.
38. As a note-taker, I want automatic versioned local backups, so that one accidental edit or damaged working file does not destroy my only history.
39. As a note-taker, I want visible backup status, so that I can tell whether recent content has been protected.
40. As a note-taker, I want a documented and tested restore operation, so that backups are useful during an actual failure.
41. As a note-taker, I want a successful save to mean the complete edit was durably committed, so that the UI never acknowledges a partial or failed change.
42. As a note-taker, I want disk-full, capacity, I/O, corruption, and incompatible-file failures reported clearly, so that I can stop editing and recover safely.
43. As a note-taker, I want the application to reopen cleanly after an ordinary process or system crash, so that journaling does not require database maintenance.
44. As a note-taker, I want the application to avoid claiming recovery from unrecoverable filesystem or hardware corruption, so that its safety promises remain honest.
45. As a note-taker, I want runtime locks, caches, and search indexes to be recreatable or non-canonical, so that losing them does not lose authored content.
46. As a privacy-conscious note-taker, I want v0 documentation to state that the Notebook and backups may contain plaintext, so that I can choose appropriate operating-system or full-disk protection.
47. As a user of file synchronization tools, I want the application to state that arbitrary synchronization of an open Notebook is unsupported, so that I do not mistake whole-file copying for safe live synchronization.
48. As a future UI-adapter author, I want Notebook behavior exposed through toolkit-neutral C++ domain values, so that I can build another native UI without importing Qt into the core.
49. As a future storage-adapter author, I want product behavior isolated from LMDB primitives and persisted byte layout, so that a later encrypted or otherwise improved backend can replace LMDB locally.
50. As a maintainer, I want globally unique Block identities that do not assume one permanent origin device, so that future single-user synchronization remains possible without implementing it in v0.
51. As a maintainer, I want canonical content to use explicit portable encodings and schema versions, so that copied Notebooks can be validated and migrated across supported builds.
52. As a maintainer, I want the system tested with approximately 250,000 Blocks and 1 GiB of text, so that performance decisions reflect realistic long-term personal use.

## Implementation Decisions

- V0 targets a single user on a single Linux desktop, with one open Notebook at a time.
- The primary experience is a keyboard-first native desktop outliner that opens on today's Journal Page. Essential mouse interaction remains supported.
- C++ is the implementation language and Qt Quick is the first UI adapter. The exact C++ standard, compiler baseline, Qt baseline, build tooling, dependency policy, and packaging approach remain delivery-planning decisions.
- The Notebook module is an in-process deep module. Its small toolkit-neutral interface is the seam used by UI adapters and behavioral tests. A daemon, local network protocol, or IPC boundary is unnecessary.
- Qt types, QObject lifetime, Qt threading, signals, models, and rendering concerns remain in the Qt Quick UI adapter. They do not enter the Notebook module's interface or implementation.
- The Notebook module owns commands, reads, block semantics, validation, transactions, reference maintenance, parsed attributes, Queries, search coordination, persistence, change notification, and undo/redo.
- UI adapters operate on owned C++ domain values and observable committed outcomes. They never receive LMDB handles, cursors, mapped memory, storage transactions, SQLite statements, or storage-engine errors.
- A Notebook is the portable unit and its database file is the sole canonical source of truth. Runtime lock files, caches, rebuildable search indexes, and backup snapshots may live separately, but none may contain unique user content required to reconstruct the Notebook.
- Markdown files are not canonical storage and lossless Markdown round-tripping is not a requirement.
- A Block is the universal persisted content entity. Every Block has a fresh globally unique stable identity, immutable type, type-appropriate payload, immutable Block Creation Time, and logical Block Update Time. Ordinary creation never reuses a deleted identity.
- One Page Block type has immutable Named and Journal kinds. Named Pages have unique slash-separated names, titles, and a separate name-derived Page Hierarchy; Journal Pages have immutable timezone-free Gregorian Journal Dates and remain virtual until durable content materializes them.
- One Entry Block type carries Authored Text. Page Entry and Journal Entry are contextual roles derived from the containing Page kind rather than distinct Block types.
- Containment is Page-rooted, ordered, single-parent, and acyclic: every materialized Page is a root and every Entry has exactly one parent chain ending at one Page. Complete Entry subtrees may move atomically between any Pages without changing identity or type.
- Named Pages and their contained Entry forests may be hard-deleted; Journal Pages have no explicit deletion command. V0 has no inactive Blocks, tombstones, persisted trash, or standalone restore command.
- Editing History is bounded, Notebook-wide, chronological, and session-local. Undo and redo commit restored logical state durably but do not provide persisted Notebook version history.
- Semantic Reference types have their own invariants and do not imply ordering, ownership, or deletion behavior.
- A Page Link literally names the current Page with an exact unique Page name; it is queryable while unresolved and is rewritten on Page rename. A Block Reference literally targets only its UUID, remaining missing when no current Block has that identity. Linked References are derived incoming Semantic References.
- V0 supports Page Links and explicit Block References. Embedding/transclusion, aliases, and unlinked textual mentions are excluded.
- Entry content is authored Unicode text with lightweight notation for Page Links, Block References, and properties. The original Authored Text remains canonical while its structured meaning is parsed and indexed transactionally.
- Queries are saved live views over Block type, Journal date, text, properties, Containment, and Semantic References. They support filtering, ordering, and limits.
- The Query language is application-defined and declarative. Arbitrary scripting, exposed database joins, aggregation, and user-defined functions are excluded from v0.
- Interactive full-text search is separate from Queries and exposes an application-defined search contract rather than a storage library's native syntax.
- LMDB is the initial canonical persistence engine and is hidden behind an internal persistence seam.
- LMDB uses `MDB_NOSUBDIR`: the main data file is canonical and its generated lock file is disposable runtime state.
- V0 retains LMDB's locking and default synchronous durability. Unsafe performance flags that weaken durability or memory protection are not used.
- One owner manages the LMDB environment for the open Notebook. Writes are serialized and each Notebook command plus its canonical derived updates commits in one short atomic write transaction.
- Read transactions remain short. Results crossing the persistence seam are owned values; no UI or long-lived model may retain mapped LMDB memory or pin a read transaction.
- A successful LMDB commit is the durable-save boundary. The application acknowledges a change only after commit and reports commit failures without presenting the change as safely saved.
- Map exhaustion is an expected capacity condition. Map growth occurs with no active transactions, after which the entire command is retried in a fresh transaction. V0 requires a 64-bit process and generous map headroom beyond the reference workload.
- Persisted keys and values use explicit versioned encodings and byte order rather than native C++ object layouts. Schema metadata and migration state live inside the canonical Notebook.
- Raw copying or replacement of the canonical data file is supported only while the Notebook is closed.
- Live snapshots use LMDB's environment-copy facility, are written to a temporary destination, verified and durably flushed, and only then published atomically as a completed backup.
- The research-backed v0 search choice is SQLite FTS5 in a disposable sidecar behind a backend-neutral search interface, with an ICU-backed application tokenizer. LMDB remains the only canonical store.
- The search sidecar contains only derived data and an applied Notebook revision. Missing, corrupt, incompatible, or irreconcilably stale indexes can be rebuilt from a stable Notebook snapshot into a temporary sidecar and atomically published.
- Canonical LMDB content commits before its corresponding sidecar update. The UI must not claim search is current across a revision gap; the exact choice between durable idempotent replay and full rebuild is left to the search-design ticket.
- The reference performance workload is approximately 250,000 Blocks and 1 GiB of text. Typical editing, Page navigation, Linked Reference views, Queries, and search should complete within about 200 milliseconds on a typical current desktop.
- V0 provides atomic durable saves, ordinary crash reopening, automatic versioned local backups, visible backup status, a tested restore path, and ordinary editing undo/redo.
- V0 provides no application-level confidentiality. Notebook files, caches, snapshots, and backups may contain plaintext and rely on operating-system permissions or full-disk encryption.
- Stable globally unique identities preserve a path to future single-user synchronization, but v0 does not add an operation log, conflict engine, sync protocol, or encryption scaffolding.

## Testing Decisions

- Tests assert observable behavior through the highest practical seam: the Notebook module's toolkit-neutral interface. This is the primary test surface for commands, reads, invariants, transactions, links, Queries, search coordination, error behavior, notifications, undo/redo, backup, and restore.
- Good behavioral tests describe outcomes a caller can observe and survive internal refactoring. They do not inspect private data structures, LMDB named databases, cursor sequences, Qt object trees, or SQLite implementation details unless exercising a focused adapter contract.
- The Notebook module is exercised with deterministic test dependencies where practical. A lightweight in-memory persistence adapter may support fast model tests, while the production LMDB adapter is covered by conformance and integration tests against temporary real Notebook files.
- All persistence adapters used by tests must satisfy the same externally observable durability and transaction contract. An in-memory adapter must not become a second, weaker definition of product behavior.
- LMDB integration tests cover atomic multi-record changes, commit failure, short-read ownership, one-environment ownership, map-full growth and whole-command retry, reader exhaustion diagnostics, incompatible/corrupt files, disk-full or I/O failure where practical, clean reopening, and portable encoding.
- Backup tests create live snapshots through the supported copy path, verify atomic publication behavior, restore into a fresh application instance, and compare all canonical content and relationships through the Notebook interface.
- Crash tests terminate the application at controlled points around commit and snapshot publication, then reopen or restore and assert that only complete acknowledged states are visible.
- Search adapter tests cover revision tracking, failure between the LMDB and sidecar commits, stale-index reporting, missing/corrupt sidecars, interrupted rebuilds, temporary-file cleanup, atomic sidecar publication, and complete reconstruction from canonical content.
- Shared Unicode golden cases define normalization, case folding, diacritics, punctuation, emoji, prefix behavior, and scripts requiring dictionary segmentation. Indexing and query tokenization must agree on these cases.
- Domain property tests cover fresh stable identity, immutable type and Page kind, Page-rooted single-parent Containment, acyclic cross-Page subtree movement, ordering, timestamp semantics, Journal Date identity, Page rename stability, incoming Linked References, Hard Deletion/restoration, and command-level atomicity.
- Authored-text parser tests cover valid notation, escaping, nested delimiters, Unicode, incomplete input, malformed input, incremental edits, and lossless retention of the user's original text.
- Query tests cover every supported predicate and composition rule, ordering, limits, live updates after committed changes, invalid syntax, deterministic results, and separation from arbitrary execution or backend query syntax.
- Qt Quick UI tests are reserved for behavior that only the UI adapter owns: focus, keyboard routing, text composition, selection, visual navigation, accessibility exposure, model-to-view updates, and presentation of save, backup, stale-search, and recovery states.
- A small end-to-end smoke suite drives the packaged Qt Quick application through creating and reopening a Notebook, journaling, linking, querying, searching, backing up, and restoring. Most permutations remain at the faster Notebook seam.
- Performance tests generate a reproducible reference Notebook of approximately 250,000 Blocks and 1 GiB of text. They measure edit commit cost, Page load, Linked Reference lookup, representative Query latency, search p50/p95 latency, startup, index size, index rebuild, snapshot creation, and restore.
- The approximate 200-millisecond interaction goal is treated as an acceptance measurement on documented reference hardware, not as a guarantee inferred from LMDB, SQLite, Qt, or ICU documentation.
- The repository is currently an empty skeleton, so there is no prior test framework or comparable test suite to reuse. The build-and-dependency decision must select and document the test framework, canonical full-suite command, and single-test command before implementation tickets are executed.

## Out of Scope

- Long-form article editing or a specialized article presentation and editing experience.
- Images, attachments, rich media, arbitrary object types, and other binary object storage.
- Rich-text spans, arbitrary styling, tables, or a general WYSIWYG document editor.
- Application-level data-at-rest encryption, password handling, key management, encrypted backups, and secure deletion claims.
- Built-in synchronization, E2EE synchronization, logical change replication, conflict resolution, collaboration, team sharing, and operation logs.
- Safe live synchronization or raw copying of an open Notebook through generic cloud-drive or file-sync tools.
- Multi-user, multi-writer, or multi-process editing.
- Ncurses, mobile, web, and Electron UI adapters. A future adapter may use the Notebook module but is not a v0 deliverable.
- Daemonization, IPC, a local server, or a remotely callable Notebook interface.
- Multiple simultaneously open Notebooks.
- Markdown as canonical storage, filesystem-to-database synchronization, or lossless Markdown import/export round-tripping.
- Embedding/transclusion, Page aliases, unlinked text mentions, arbitrary scripting, Query aggregation, and user-defined Query functions.
- Historical browsing or a complete per-Block version-history interface beyond ordinary undo/redo and backup restoration.
- Plugin interfaces and third-party extension execution.

## Further Notes

- The canonical domain vocabulary is maintained in the project glossary and should be used consistently in implementation tickets, interfaces, UI copy, and tests.
- The Wayfinder map remains the index of open design decisions. This specification records the agreed product and architecture envelope; tickets should not invent behavior where the map still calls for a grilling or prototype decision.
- Before implementation tickets are considered executable, the remaining decisions must settle the exact Notebook interface, authored-text grammar, journal editing behavior, persistence schema and migration protocol, data-safety contract, search semantics, Query language, Page Hierarchy, Qt Quick interaction model, build/toolchain baseline, and acceptance gates.
- The LMDB and full-text-search research reports provide primary-source constraints and should be consulted when resolving persistence and search tickets.
- The eventual application license has not been selected. Dependency licensing must be checked as part of the build-and-dependency decision.
- Data-at-rest encryption and E2EE synchronization are possible future efforts, not guaranteed roadmap commitments. If pursued, users should still be able to opt out.
