# 15 — Handle capacity and save failures safely

**Parent spec:** [Notebook v0](../../notebook-v0/spec.md)

**What to build:** Keep acknowledged Notebook state honest when storage reaches map capacity or a save encounters disk-full, I/O, transaction, or other persistence failure. Recoverable map exhaustion is handled internally; unrecoverable saves remain visibly failed without partial canonical changes.

**Blocked by:** 03 — Edit nested journal outlines; [Define the data safety contract](../../notebook-v0/issues/08-define-the-data-safety-contract.md)

**Status:** ready-for-agent

- [ ] Map exhaustion ends all active transactions, grows the map safely, and retries the complete command in a fresh transaction.
- [ ] Disk-full, I/O, commit, reader-capacity, and relevant LMDB failures are translated into stable Notebook-level errors.
- [ ] The UI does not report or retain a failed command as durably saved.
- [ ] Failed compound edits expose neither partial canonical records nor partially updated canonical indexes.
- [ ] Diagnostics expose actionable capacity and stale-reader information without leaking storage primitives into normal UI behavior.
- [ ] Failure-injection integration tests verify acknowledged-state boundaries, whole-command retry, atomicity, and successful editing after recoverable failures.

