# Establish LMDB operational constraints

Type: research
Status: resolved

## Question

What guarantees, limits, and operational rules from primary LMDB sources must the v0 persistence design respect for transactions, durability, crash recovery, map sizing, backup/snapshot creation, file copying, locking, and one-file portability from C++?

## Comments

## Answer

Research report: [LMDB operational constraints for Notebook v0](../../../docs/research/lmdb-operational-constraints.md).

Decision-relevant findings: use `MDB_NOSUBDIR` with the main data file as the sole canonical artifact and the `-lock` file as disposable runtime state; retain LMDB locking and synchronous durability defaults; own one environment per open Notebook and serialize short atomic write transactions; keep reads short and return owned values; plan and monitor generous map headroom with whole-command retry after transaction-free growth; create live backups through LMDB's snapshot copy API and publish them atomically; permit raw file transfer only while closed; and encode schema, keys, and values independently of native C++ representation. LMDB errors and snapshots still require explicit recovery and restore behavior—LMDB does not replace a tested backup contract.
