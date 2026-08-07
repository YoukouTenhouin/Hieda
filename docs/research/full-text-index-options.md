# Full-text index options for Notebook v0

## Decision summary

Use **SQLite FTS5 in a disposable sidecar database** for v0, behind a
Notebook-owned search interface. Keep authored block text and every durable
fact needed to recreate the index in the LMDB Notebook file. The sidecar may
hold postings, ranking statistics, a block-ID mapping, and its applied Notebook
revision, but none of those are canonical.

Use an application-owned Unicode tokenization contract implemented with ICU
and registered as an FTS5 custom tokenizer. The built-in `unicode61` tokenizer
is an acceptable prototype fallback, not the long-term definition of search:
it uses Unicode 6.1 categories and case folding, whereas ICU tracks released
Unicode standards and adds dictionary word breaking for scripts including
Chinese, Japanese, Thai, Lao, Khmer, and Burmese. SQLite explicitly supports
application-defined tokenizers through the FTS5 API.

This recommendation should be confirmed with a benchmark using the reference
Notebook (about 250,000 blocks and 1 GiB of text). Neither project documentation
provides a latency guarantee for this exact workload.

## Non-negotiable storage boundary

The LMDB Notebook file is the **only canonical artifact**. Search is a derived
projection:

- Copying only the closed Notebook file transfers all user content. On a new
  device, search may be unavailable while the sidecar is rebuilt.
- Search results contain block identities that are resolved back through the
  Notebook. Snippets may be generated from current canonical text; no feature
  may depend on text or metadata that exists only in the search database.
- The Notebook carries a monotonically increasing content revision. The search
  sidecar records the revision it has applied in the same SQLite transaction as
  its index update. A mismatch on open or after an indexing failure makes the
  index stale and triggers repair or full rebuild.
- Updates are ordered: commit canonical LMDB content first, then update the
  sidecar. A crash can therefore leave search behind, never ahead of content.
  There is no cross-engine atomic transaction, so an operation must not report
  search as current until both commits have succeeded.
- Once a revision mismatch exists, isolated later updates must not advance the
  sidecar's revision and accidentally hide the gap. Either replay a durable,
  idempotent change queue recorded with the LMDB writes, or keep the sidecar
  stale until a complete rebuild succeeds. The queue is a rebuild optimization,
  not canonical user content.
- Rebuild into a new temporary sidecar by scanning a stable LMDB read
  transaction, record that snapshot's revision, then publish it atomically.
  If content advanced meanwhile, apply later changes or rebuild again. The old
  valid index remains usable until replacement.

LMDB transactions cover the databases within one environment and commit all
their operations together; that guarantee cannot extend to SQLite or Xapian
([LMDB API](https://github.com/LMDB/lmdb/blob/mdb.master/libraries/liblmdb/lmdb.h)).
SQLite is ACID within SQLite, including crash/power interruption, but even its
multi-file atomicity applies to SQLite databases attached to the same
connection, not to LMDB
([SQLite transactional guarantee](https://www.sqlite.org/transactional.html),
[SQLite `ATTACH`](https://www.sqlite.org/lang_attach.html)).

## Options compared

| Approach | Unicode and search capability | Consistency and rebuilds | Portability, packaging, and replacement | Assessment |
| --- | --- | --- | --- | --- |
| **SQLite FTS5 sidecar** | Phrase, prefix, proximity/`NEAR`, Boolean, column filtering, trigram matching, and BM25 ranking are built in. `unicode61` is case-insensitive and diacritic-aware but fixed to Unicode 6.1. A custom FTS5 tokenizer can use ICU normalization, case folding, and word boundaries. | Separate from LMDB, so use the revision protocol above. A contentless index avoids duplicating authored text; because LMDB is not a SQLite “external content” table, application code must populate and rebuild it by scanning LMDB. Build a replacement file instead of relying on FTS5's SQL `rebuild` command. | One disposable cache file. FTS5 ships in the SQLite amalgamation and can be compiled into the application; SQLite deliverables are public domain. The search adapter can accept backend-neutral index documents, making later replacement of either LMDB or FTS5 local to an adapter. | **Recommended.** Small operational footprint, mature embedded C API, sufficient search features, and the cleanest balance of implementation effort and replaceability. |
| **Xapian sidecar** | Native C++ search engine with ranking, phrase/proximity search, stemming, spelling, synonyms, and n-grams. Current 2.0 APIs can use ICU word breaking; the project advertises UTF-8 storage but its feature page names Unicode 9.0, so Unicode behavior still requires explicit tests. | Xapian provides atomic transactions inside one writable database, but not jointly with LMDB. The same revision/rebuild protocol is required. Standard writable databases use a directory. Xapian can compact Glass into one file, but official documentation says that form is read-only, so it does not simplify live v0 indexing. | Adds a C++ library and GPL licensing consideration. It is more feature-rich than v0 needs and its writable multi-file directory creates more cache lifecycle and packaging surface. A sidecar adapter would still isolate future storage changes. | Viable if relevance tuning, stemming, spelling, or advanced retrieval becomes a near-term requirement; otherwise heavier than needed. |
| **Custom inverted index in LMDB, with ICU** | Complete control over normalization, tokenization, postings, phrase positions, prefix strategy, and ranking, but all query execution and index maintenance must be designed, implemented, fuzzed, and tuned by this project. ICU supplies current normalization, full case folding, and locale/dictionary-aware word boundaries. | Strongest consistency: canonical content and derived postings can change in one LMDB write transaction. Rebuildable named databases can live in the same data file, although rebuilds then enlarge and mutate the portable Notebook itself. | No second database engine, but ICU remains a substantial packaged dependency. It couples posting layout, compaction, and index migrations to the LMDB persistence implementation, increasing the cost of the planned storage replacement. | Do not choose for v0. Its atomicity advantage does not justify owning a search engine before measurements show the sidecar protocol is inadequate. |

### Options excluded from the shortlist

- **Tantivy** is actively maintained and capable, but its official project is
  Rust and does not publish a supported C or C++ API. Adopting it would mean
  owning a C ABI wrapper, Rust toolchain integration, and cross-platform binary
  packaging, so it is not a maintained C/C++ option for this v0
  ([Tantivy repository](https://github.com/quickwit-oss/tantivy)).
- **CLucene** is a C++ port of old Lucene versions whose official file listing
  shows its latest release as 2.3.3.4 from 2011. It is not a credible maintained
  dependency
  ([CLucene files](https://sourceforge.net/projects/clucene/files/)).
- **Apache Lucene** is actively maintained but Java-based; JNI or a bundled JVM
  would add a language/runtime boundary that the embedded C++ core does not
  otherwise need ([Apache Lucene repository](https://github.com/apache/lucene)).

### SQLite details

FTS5 supports contentless tables specifically to omit the private copy of
indexed columns. Its documentation also warns that an external-content index is
the caller's responsibility to keep consistent. Since FTS5 cannot query LMDB as
an external SQLite table, use a contentless/contentless-delete table plus a
derived mapping between FTS5's integer `rowid` and the Notebook's globally
unique block ID. The mapping may be a normal table in the same sidecar and may
be reassigned during rebuild
([FTS5 contentless and external-content tables](https://www.sqlite.org/fts5.html#external_content_and_contentless_tables)).

The FTS5 documentation defines phrase, prefix, `NEAR`, Boolean, and trigram
queries, prefix indexes, and BM25 ranking. It also documents incremental index
merging, which bounds work during normal insert/update/delete operations
([FTS5](https://www.sqlite.org/fts5.html)). The project should expose its own
small search grammar and compile it to FTS5 rather than making SQLite syntax a
public Notebook API; this preserves storage replacement and lets malformed user
input be handled consistently.

The default tokenizer treats Unicode 6.1 letter and number runs as tokens,
folds case, and normally removes Latin diacritics. It does not supply current
Unicode semantics or dictionary segmentation. FTS5's custom-tokenizer API lets
the application supply those semantics
([FTS5 tokenizers](https://www.sqlite.org/fts5.html#tokenizers),
[custom tokenizer API](https://www.sqlite.org/fts5.html#custom_tokenizers)). ICU
provides standard normalization and full string case folding, and its word
`BreakIterator` follows Unicode word-boundary rules with automatic dictionary
support for several scripts without spaces
([ICU normalization](https://unicode-org.github.io/icu/userguide/transforms/normalization/),
[ICU case mappings](https://unicode-org.github.io/icu/userguide/transforms/casemappings.html),
[ICU boundary analysis](https://unicode-org.github.io/icu/userguide/boundaryanalysis/)).

SQLite FTS5 is part of SQLite's amalgamation and may be enabled at compile time,
which makes version pinning and static packaging straightforward
([building FTS5](https://www.sqlite.org/fts5.html#compiling_and_using_fts5)).
SQLite's shipped code is dedicated to the public domain
([SQLite copyright](https://www.sqlite.org/copyright.html)). ICU adds common and
internationalization libraries plus Unicode data to the application package;
its packaging guide documents those runtime pieces
([ICU4C packaging](https://unicode-org.github.io/icu/userguide/icu4c/packaging.html)).

### Xapian details

Xapian is a maintained, native C++ information-retrieval library. Its default
on-disk backend supports incremental changes and single-writer/multiple-reader
access, and its API supplies atomic groups of modifications within a database
([Xapian overview](https://xapian.org/docs/overview.html),
[writable transactions](https://xapian.org/docs/apidoc/html/classXapian_1_1WritableDatabase.html)).
Its term generator supports n-grams for scripts without explicit word breaks
and, in 2.0, ICU word breaking
([`TermGenerator`](https://xapian.org/docs/apidoc/html/classXapian_1_1TermGenerator.html)).

Xapian's normal writable Glass database is directory-backed. Its compact
single-file Glass representation is read-only, so an actively maintained index
would still be a directory of cache files
([Xapian administrator guide](https://xapian.org/docs/admin_notes.html),
[`Database::compact`](https://xapian.org/docs/apidoc/html/classXapian_1_1Database.html)).
The project lists Xapian as GPL-licensed
([Xapian features](https://xapian.org/features)); that may be compatible with
the eventual application license, but it is a decision the build/dependency
ticket would have to make deliberately.

## Consequences for the later search specification

The search design ticket should specify, independently of FTS5:

1. Exact normalization, case-folding, diacritic, punctuation, emoji, CJK, and
   prefix behavior, with multilingual golden tests shared by indexing and query
   tokenization.
2. The backend-neutral indexed-document shape (block ID, searchable text,
   type/date fields if needed, and ranking boosts) and the small user-visible
   query grammar.
3. Whether an edit waits for the sidecar commit before the UI reports search as
   current, and how stale/unavailable search is shown.
4. Revision mismatch detection, rebuild progress/cancellation, temporary-file
   cleanup, disk-full behavior, and atomic publication of a rebuilt index.
5. Benchmarks at 250,000 blocks / 1 GiB for incremental edit cost, common-query
   p50/p95 latency, index size, cold startup, and full rebuild time. Treat the
   200 ms interaction target as an acceptance test, not as an assumption based
   on library marketing.
