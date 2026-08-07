# 16 — Recover safely during startup

**Parent spec:** [Notebook v0](../../notebook-v0/spec.md)

**What to build:** Reopen normally after process or system interruption and guide the user safely when a Notebook, migration, or derived search state is incompatible or damaged. Recovery behavior must distinguish recreatable artifacts from canonical content and offer the supported restore path when needed.

**Blocked by:** 12 — Detect and rebuild stale search indexes; 14 — Run automatic versioned backups; 15 — Handle capacity and save failures safely; [Define the data safety contract](../../notebook-v0/issues/08-define-the-data-safety-contract.md)

**Status:** ready-for-agent

- [ ] Terminating the application after any acknowledged command still allows the complete acknowledged Notebook state to reopen.
- [ ] Disposable lock and search artifacts are recreated or repaired without being mistaken for canonical data loss.
- [ ] Interrupted or incompatible schema migration follows the resolved resume, rollback, or refusal behavior.
- [ ] Invalid, version-incompatible, corrupt, and panic-state LMDB errors produce distinct actionable Notebook-level outcomes.
- [ ] When automatic repair is unsafe, the UI preserves the original file and offers the supported snapshot restore workflow.
- [ ] Startup recovery and process-termination tests assert externally observable states and never claim recovery from unrecoverable filesystem or hardware corruption.

