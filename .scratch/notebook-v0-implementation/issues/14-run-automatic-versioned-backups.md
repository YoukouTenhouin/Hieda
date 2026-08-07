# 14 — Run automatic versioned backups

**Parent spec:** [Notebook v0](../../notebook-v0/spec.md)

**What to build:** Protect active work with automatic versioned snapshot backups, the resolved cadence and retention policy, and visible backup status. Automatic cleanup must never remove the only valid recovery point because a newer backup failed.

**Blocked by:** 13 — Create and restore Notebook snapshots; [Define the data safety contract](../../notebook-v0/issues/08-define-the-data-safety-contract.md)

**Status:** ready-for-agent

- [ ] Backups run at the agreed trigger or cadence without requiring unsafe raw copying of the open Notebook.
- [ ] Completed backup generations are named, retained, and expired according to the resolved policy.
- [ ] Retention cleanup preserves the required valid generations and handles partial or failed backups safely.
- [ ] The UI shows last successful backup time, backup-in-progress state, and actionable failure status.
- [ ] Closing or crashing during an automatic backup leaves canonical content and previously published backups intact.
- [ ] Tests use a controllable clock and failure injection to verify scheduling, retention, status, restart, and recovery behavior.

