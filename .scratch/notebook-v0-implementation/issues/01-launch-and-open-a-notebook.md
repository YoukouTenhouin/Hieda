# 01 — Launch the app and create or open a Notebook

**Parent spec:** [Notebook v0](../../notebook-v0/spec.md)

**What to build:** A native Qt Quick application that lets a user create a new portable Notebook or open one existing Notebook through the toolkit-neutral C++ Notebook module. This first tracer establishes the build, module seam, LMDB ownership, and real-file test path while presenting a minimal usable empty state.

**Blocked by:** [Design the Notebook module interface](../../notebook-v0/issues/03-design-the-notebook-module-interface.md), [Design persistence schema and migrations](../../notebook-v0/issues/07-design-persistence-schema-and-migrations.md), and [Choose the build and dependency strategy](../../notebook-v0/issues/13-choose-the-build-and-dependency-strategy.md)

**Status:** completed

- [x] The documented canonical build and test commands succeed from a clean checkout.
- [x] The packaged application launches as a native Qt Quick process and can create a Notebook at a user-selected path.
- [x] The application can close and reopen a created Notebook without creating a second canonical content file.
- [x] Opening a Notebook is mediated by the toolkit-neutral Notebook module; Qt and LMDB types do not cross its interface.
- [x] Only one Notebook can be open at a time, and attempts to open unsupported or invalid inputs produce a visible error rather than a crash.
- [x] Automated tests exercise create, close, and reopen behavior through the Notebook module using a temporary real Notebook file.
