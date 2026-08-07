# 13 — Create and restore Notebook snapshots

**Parent spec:** [Notebook v0](../../notebook-v0/spec.md)

**What to build:** Let users create a consistent snapshot while a Notebook is open and restore a selected completed snapshot into a usable Notebook. Snapshot publication and restoration must protect the working Notebook from partial copies and failed replacement.

**Blocked by:** 02 — Capture and reopen flat Journal Entries; [Design persistence schema and migrations](../../notebook-v0/issues/07-design-persistence-schema-and-migrations.md); [Define the data safety contract](../../notebook-v0/issues/08-define-the-data-safety-contract.md)

**Status:** ready-for-agent

- [ ] An open Notebook is snapshotted through LMDB's supported environment-copy facility rather than raw file copying.
- [ ] Snapshot output is written to a temporary destination, checked, durably flushed as required, and atomically published only after success.
- [ ] Failed or interrupted snapshot creation never replaces a previously completed snapshot with partial output.
- [ ] A user can select and restore a completed snapshot through the agreed safe restore flow.
- [ ] Restoring into a fresh application instance reproduces all canonical Blocks, relationships, text, metadata, settings, and schema state.
- [ ] Integration and interruption tests exercise snapshot creation during writes, failed publication, and complete restore through observable Notebook behavior.

