# Evaluate full-text index options

Type: research
Status: resolved

## Question

Which maintained C or C++ full-text indexing approaches can support fast Unicode-aware search for roughly 250,000 blocks while preserving the one-canonical-file Notebook promise, and what trade-offs would each impose on transactions, index rebuilds, packaging, and future storage replacement?

## Comments

## Answer

[Full-text index options for Notebook v0](../../../docs/research/full-text-index-options.md)
compares the maintained native choices and their operational consequences.

Adopt SQLite FTS5 as a disposable sidecar behind a backend-neutral search
interface. LMDB remains the sole canonical store; the index contains no unique
user data and is rebuilt after a Notebook-only transfer, corruption, schema or
tokenizer change, or irrecoverable revision mismatch. Because LMDB and SQLite
cannot share a transaction, serialize content/index updates, track the applied
Notebook revision, and either replay a durable idempotent change queue or keep
search explicitly stale until a full rebuild succeeds.

Define Unicode behavior in the Notebook search contract rather than inheriting
SQLite syntax or tokenizer behavior. Prefer an ICU-backed custom FTS5 tokenizer;
the built-in `unicode61` tokenizer is useful for a prototype but is fixed to
Unicode 6.1 and lacks modern dictionary word segmentation. Validate the choice
against the 250,000-block / 1-GiB reference Notebook and the 200 ms interaction
target.

Xapian is the credible native-C++ fallback if richer retrieval features justify
its heavier packaging, GPL consideration, and writable directory index. A
custom ICU-tokenized LMDB postings engine offers same-transaction consistency
but couples search to the initial storage engine and makes the project own too
much information-retrieval machinery for v0. Tantivy lacks an official C/C++
API, and CLucene is obsolete.
