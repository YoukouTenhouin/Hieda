# 17 — Meet the reference performance envelope

**Parent spec:** [Notebook v0](../../notebook-v0/spec.md)

**What to build:** Generate the agreed long-lived personal Notebook workload, measure every important interaction reproducibly, and remove bottlenecks that prevent the application from meeting its documented v0 performance gates without weakening durability or correctness.

**Blocked by:** 05 — Support production desktop text input; 08 — Reference Blocks and browse Linked References; 10 — Query Containment and Semantic References; 12 — Detect and rebuild stale search indexes; 16 — Recover safely during startup; [Define verification and performance gates](../../notebook-v0/issues/14-define-verification-and-performance-gates.md)

**Status:** ready-for-agent

- [ ] A deterministic generator creates the reference Notebook of approximately 250,000 Blocks and 1 GiB of authored text with realistic nesting, links, properties, and dates.
- [ ] Benchmarks measure edit commit, Journal Page load, Page navigation, Linked Reference lookup, representative Queries, search p50/p95, startup, snapshot creation, restore, and index rebuild.
- [ ] Reference hardware, build mode, data shape, warm/cold conditions, and measurement procedure are documented and repeatable.
- [ ] Typical user interactions covered by the resolved gate meet the approximate 200-millisecond target or an explicitly approved exception is recorded.
- [ ] Performance work retains synchronous durability, canonical consistency, short read transactions, and complete behavioral test coverage.
- [ ] Benchmark regressions are detectable by the project's documented verification workflow without making ordinary tests prohibitively slow.

