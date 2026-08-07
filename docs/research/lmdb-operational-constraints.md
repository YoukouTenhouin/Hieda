# LMDB operational constraints for Notebook v0

## Decision summary

LMDB fits the v0 portable-Notebook requirement if the application treats the LMDB environment as a carefully wrapped storage engine rather than exposing its handles or byte views. Open the Notebook with `MDB_NOSUBDIR`, keep the generated `-lock` file as non-canonical runtime state, use LMDB's default synchronous commit behavior, and create live snapshots with `mdb_env_copyfd2()` rather than an operating-system file copy.

The persistence adapter should own one environment handle per open Notebook, serialize all writes, keep read transactions short, and atomically update canonical records and derived indexes in the same write transaction. It should also own map-growth retries, snapshots, error translation, and all LMDB object lifetimes.

## Required operating rules

### Transactions and concurrency

- LMDB permits concurrent snapshot readers but only one writer; a read transaction's ID identifies the snapshot it sees. A Notebook command and every derived-index update caused by it should therefore commit in one write transaction. The UI must never observe or persist a partially indexed command. [LMDB API: transactions and transaction IDs](https://www.lmdb.tech/doc/group__mdb.html)
- A transaction and its cursors must be used by only one thread, and cursors cannot span transactions. `MDB_NOTLS` relaxes thread affinity only for read-only transactions; it does not make concurrent use of one transaction safe. The simplest v0 design is a storage-owned writer execution context plus short, scoped read transactions. [LMDB API: `mdb_txn_begin`](https://www.lmdb.tech/doc/group__mdb.html)
- Long-lived readers retain old page versions and prevent their reuse, potentially causing rapid file growth. Views must materialize/copy their result values and close the read transaction rather than retaining a transaction for the lifetime of a Qt model. [LMDB API: `mdb_txn_reset`](https://www.lmdb.tech/doc/group__mdb.html)
- Returned `MDB_val` memory is owned by LMDB and is valid only until a later update operation or the end of its transaction. The C++ boundary must return owned domain values, never `MDB_val`, raw mapped pointers, cursors, transactions, or `string_view` instances into mapped data. [LMDB API: `MDB_val` and `mdb_get`](https://www.lmdb.tech/doc/group__mdb.html)
- Keep LMDB locking enabled. `MDB_NOLOCK` transfers single-writer and old-reader exclusion to the caller, while the normal reader table is precisely how LMDB knows which old pages remain live. The adapter should periodically make stale-reader diagnosis available through `mdb_reader_check()`, but it should not disable the lock protocol. [LMDB API: `MDB_NOLOCK` and reader management](https://www.lmdb.tech/doc/group__mdb.html), [LMDB reader-lock-table internals](https://www.lmdb.tech/doc/group__readers.html)
- Open a particular Notebook environment only once inside the process, do not use an inherited environment after `fork()`, and finish all transactions/cursors before the sole environment owner closes it. These constraints belong in the RAII wrapper and tests. [LMDB source documentation: caveats](https://git.openldap.org/orent/openldap/-/commit/054812517f0099f489eb47fdb25f53396580c6a0), [LMDB API: `mdb_env_close`](https://www.lmdb.tech/doc/group__mdb.html)

### Durability and crash behavior

- Use the default synchronous mode for v0: do **not** enable `MDB_NOSYNC`, `MDB_NOMETASYNC`, `MDB_MAPASYNC`, or `MDB_WRITEMAP`. By default, commit writes data and flushes operating-system buffers. `MDB_NOMETASYNC` can lose the last committed transaction after a system crash; `MDB_NOSYNC` and `MDB_MAPASYNC` can lose transactions and, in documented combinations, corrupt the database. `MDB_WRITEMAP` also removes protection against stray application writes and must not be mixed across processes. [LMDB API: environment flags and `mdb_env_sync`](https://www.lmdb.tech/doc/group__mdb.html)
- Treat a successful `mdb_txn_commit()` as the v0 durable-save boundary, and translate failures such as `ENOSPC` and `EIO` into a visible failed save. Never report success or mutate the UI's acknowledged state before commit succeeds. [LMDB API: `mdb_txn_commit`](https://www.lmdb.tech/doc/group__mdb.html)
- Normal process or system crashes require reopening the environment, not replaying an application WAL. Opening can still report corruption or incompatibility (`MDB_INVALID`, `MDB_VERSION_MISMATCH`), and a fatal LMDB error can yield `MDB_PANIC`; the product therefore still needs tested snapshot restoration and must not claim recovery from filesystem or hardware corruption. [LMDB API: `mdb_env_open` and return codes](https://www.lmdb.tech/doc/group__mdb.html)

### Map sizing and resource limits

- The map size is the maximum database size, not merely a cache setting. LMDB's default is only 10 MiB, and its documentation says to choose a large value with future growth in mind. V0 should require a 64-bit process, start with generous virtual-address headroom beyond the 1 GiB content target, expose remaining map capacity in diagnostics, and test growth well before exhaustion. [LMDB API: `mdb_env_set_mapsize`](https://www.lmdb.tech/doc/group__mdb.html)
- `MDB_MAP_FULL` is an expected capacity condition. Resize only after aborting/finishing **all** transactions in the process; LMDB does not fully enforce that precondition for the caller. Then retry the entire Notebook command in a fresh write transaction. A resize performed elsewhere is reported as `MDB_MAP_RESIZED`, after which `mdb_env_set_mapsize(env, 0)` adopts the persisted increase. [LMDB API: map size and `mdb_txn_begin`](https://www.lmdb.tech/doc/group__mdb.html)
- Configure the small, known set of named databases with `mdb_env_set_maxdbs()` before opening the environment. Set the reader-slot count before open as well; the default is 126, and excess concurrent read transactions fail with `MDB_READERS_FULL`. Do not create a named database per page, type, or user query. [LMDB API: `mdb_env_set_maxdbs`, `mdb_env_set_maxreaders`, and return codes](https://www.lmdb.tech/doc/group__mdb.html)
- LMDB's default maximum key size is normally 511 bytes and can vary with its build. Use compact fixed-size/bounded binary keys, query `mdb_env_get_maxkeysize()` during open, and store unbounded titles/text in values. [LMDB API: `mdb_env_get_maxkeysize`](https://www.lmdb.tech/doc/group__mdb.html)

### Snapshots, copying, and the one-file promise

- `MDB_NOSUBDIR` makes the supplied path the canonical data file and creates a separate `<path>-lock` runtime file. LMDB's backup API does not copy a lock file because it is recreated when needed. Thus copying one quiescent data file or one application-created snapshot transfers the Notebook; the lock file is not Notebook content. [LMDB API: `MDB_NOSUBDIR` and environment-copy functions](https://www.lmdb.tech/doc/group__mdb.html)
- For an open Notebook, use `mdb_env_copyfd2()` (or `mdb_env_copy2()` when a directory-shaped destination is appropriate). LMDB explicitly supports copying an environment while it is in use. `MDB_CP_COMPACT` omits free pages but costs more CPU and time. A concurrent copy holds a read snapshot, so sustained writes during a slow backup can temporarily grow the source file. [Official `mdb_copy` man page](https://manpages.debian.org/testing/lmdb-utils/mdb_copy.1.en.html), [LMDB API: environment-copy functions](https://www.lmdb.tech/doc/group__mdb.html)
- The application should write a snapshot to a new temporary file, verify the copy result, durably flush it, and only then publish it atomically under its final backup name. This publication protocol is an application requirement; the LMDB copy API alone does not define backup naming, retention, or atomic replacement.
- Raw `cp`, cloud-drive synchronization, or replacement of the canonical file is supported only while the Notebook is closed. For a live Notebook, the documented safe mechanism is LMDB's copy API, so v0 must not advertise arbitrary live-file copying or syncing as safe.

### Portable C++ representation

- The C API is directly callable from C++, but every LMDB handle needs a non-copyable RAII owner with explicit transaction commit/abort behavior and translated error types. The public Notebook module must expose neither LMDB types nor LMDB transaction semantics.
- Encode application keys and values with an explicit version and byte order. In particular, avoid `MDB_INTEGERKEY`/`MDB_INTEGERDUP` for portable persisted identifiers because LMDB defines their representation in native byte order. Ordinary lexical byte keys with application-defined encoding are predictable across supported builds. [LMDB API: database flags](https://www.lmdb.tech/doc/group__mdb.html)
- Put schema/version metadata inside the data file and test opening a copied Notebook with every supported platform/build combination. LMDB can reject a file with `MDB_VERSION_MISMATCH`; application schema compatibility and migration remain the Notebook adapter's responsibility. [LMDB API: `mdb_env_open`](https://www.lmdb.tech/doc/group__mdb.html)

## Consequences for later design tickets

1. **Persistence schema:** group primary block data, containment, semantic-reference indexes, search metadata, schema version, and migration state into a bounded set of named databases, all updated by one write transaction.
2. **Notebook interface:** commands are the atomic write unit. Reads return owned snapshots/results. LMDB capacity, locking, backup, and retry details stay behind the module boundary.
3. **Data-safety contract:** default synchronous commits are mandatory; successful commit, snapshot publication, and restore verification need explicit tests, including process-kill, power-loss simulation where practical, disk-full, corrupt-file, and map-full cases.
4. **Backup behavior:** live automatic backups use the copy API and publish a new file atomically; user-directed transfer can use a closed canonical file or a completed snapshot.
5. **Performance:** transactions should be short. Search/query result production should page or materialize promptly, never pin an LMDB read transaction behind long-lived UI state.

## Primary sources

- [LMDB C API documentation](https://www.lmdb.tech/doc/group__mdb.html)
- [LMDB reader lock table documentation](https://www.lmdb.tech/doc/group__readers.html)
- [Official `mdb_copy(1)` manual, distributed from the LMDB source](https://manpages.debian.org/testing/lmdb-utils/mdb_copy.1.en.html)
- [OpenLDAP LMDB source documentation containing the environment caveats](https://git.openldap.org/orent/openldap/-/commit/054812517f0099f489eb47fdb25f53396580c6a0)
