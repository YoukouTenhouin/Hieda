# 12 — Detect and rebuild stale search indexes

**Parent spec:** [Notebook v0](../../notebook-v0/spec.md)

**What to build:** Detect a missing, stale, corrupt, incompatible, or interrupted search sidecar and restore trustworthy search by rebuilding derived data from canonical Notebook content. Users must be told when results are unavailable or stale.

**Blocked by:** 11 — Search all Notebook text; [Design search behavior and indexing](../../notebook-v0/issues/09-design-search-behavior-and-indexing.md)

**Status:** ready-for-agent

- [ ] The application compares canonical and applied search revisions and never hides a detected revision gap.
- [ ] Missing, corrupt, incompatible, and irreconcilably stale sidecars enter the resolved unavailable or stale state.
- [ ] A rebuild scans a stable canonical snapshot into a temporary sidecar and publishes only a completed valid replacement.
- [ ] Restart, cancellation, disk-full, and failure during rebuild preserve canonical content and clean up or safely ignore incomplete temporary artifacts.
- [ ] The UI presents rebuild status and prevents incomplete results from appearing complete.
- [ ] Tests prove that deleting the entire search sidecar and rebuilding produces the same externally observable search results.

