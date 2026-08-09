# Chart an implementation-ready Notebook v0 specification

Label: wayfinder:map

## Destination

Produce an implementation-ready specification and delivery route for a single-user Linux desktop v0 that can serve as a dependable daily journal: typed blocks, pages, links and linked references, declarative queries, full-text search, and safe canonical persistence in one portable Notebook file.

## Notes

- Use the `grilling`, `domain-modeling`, and `codebase-design` skills for product and architecture decisions; use `prototype` and `research` where each ticket calls for them.
- The product is a keyboard-first native Linux desktop application for one user and one open Notebook at a time.
- C++ is the v0 implementation language. Qt Quick is the first UI adapter; Qt must not enter the Notebook module.
- The Notebook module is an in-process deep module. A daemon or IPC protocol is not part of the design.
- LMDB is the initial storage engine, hidden behind an internal persistence seam. The UI must see neither LMDB nor storage primitives.
- One canonical database file is the portability unit. Runtime locks, caches, and separate backup snapshots may exist outside it.
- The database is the sole canonical source of truth. Markdown round-trip storage is not required.
- Blocks have globally unique stable identities and immutable types. Ordered Containment gives every Entry one Page-rooted structural home; name-derived Page Hierarchy and Semantic References remain different relationship types.
- Journal entries are simple authored Unicode text with lightweight notation whose structured meaning is parsed and indexed transactionally.
- Queries are saved declarative views; interactive full-text search is a separate capability.
- Target a reference Notebook of approximately 250,000 blocks and 1 GiB of text, with typical editing, navigation, search, and linked-reference interactions completing within about 200 ms on a current desktop.
- V0 must provide atomic durable saves, crash recovery, automatic versioned backups, a tested restore path, visible backup status, and ordinary undo/redo.
- Live copying or generic file synchronization of an open LMDB Notebook is unsupported. Closed Notebooks and application-created snapshots are portable.

## Decisions so far

<!-- Resolved tickets are indexed here; the detailed answer lives in each ticket. -->

- [Establish LMDB operational constraints](issues/01-establish-lmdb-operational-constraints.md) — LMDB fits the portable Notebook model when wrapped behind owned transactions, synchronous durability, safe snapshot, map-growth, and portable-encoding rules.
- [Evaluate full-text index options](issues/02-evaluate-full-text-index-options.md) — SQLite FTS5 with ICU tokenization is the preferred rebuildable search sidecar; LMDB remains the sole canonical store.
- [Design the Notebook module interface](issues/03-design-the-notebook-module-interface.md) — one thread-safe synchronous `NotebookSession` is the toolkit-neutral seam.
- [Specify Block lifecycle and structural invariants](issues/04-specify-block-lifecycle-and-structural-invariants.md) — unified immutable Page and Entry types use Page-rooted Containment, hard deletion, logical timestamps, immutable Journal Dates, and Notebook-wide session history.
- [Design persistence schema and migrations](issues/07-design-persistence-schema-and-migrations.md) — bounded LMDB databases use portable TLV records and shadow-copy migrations.
- [Define link resolution and Linked References](issues/11-define-link-resolution-and-linked-references.md) — Page Links are literal Page Names, Block References are literal UUIDs, and incoming results are bounded source-Block views.
- [Choose the build and dependency strategy](issues/13-choose-the-build-and-dependency-strategy.md) — C++20, Qt 6.8+, system dependencies, CMake/Ninja, CTest, and CPack TGZ.
- [Define Page Hierarchy behavior](issues/16-define-page-hierarchy-behavior.md) — materialized Page Names exclusively derive a lazy, ordered hierarchy whose previews, cascades, queries, navigation, history, and performance semantics are explicit.

## Not yet specified

- Editing edge cases involving IME composition, accessibility, selection, and very large blocks; the Qt Quick interaction prototype will reveal which require explicit decisions.
- Concrete corruption and partial-recovery cases; these become specifiable after the LMDB constraints and persistence layout are known.
- Index rebuild, migration interruption, and compatibility cases; their exact questions depend on the selected index and schema designs.
- Packaging and distribution edge cases across Linux environments; these depend on the initial toolchain and dependency strategy.

## Out of scope

- Long-form article editing and richer content objects, including images and attachments.
- Application-level data-at-rest encryption and key management. V0 data, caches, snapshots, and backups may contain plaintext.
- Built-in synchronization, E2EE synchronization, conflict resolution, collaboration, or an operation log.
- Live synchronization of an open Notebook database file through generic file-sync software.
- Ncurses, mobile, web, and Electron UI adapters.
- Daemonization, IPC, plugin APIs, and multi-process access.
- Markdown files as canonical storage or lossless Markdown round-tripping.
