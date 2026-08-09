# Specify block lifecycle and structural invariants

Type: grilling
Status: resolved

## Question

What are the exact invariants and user-visible semantics for Block identity, types, ordered Containment, moves, recursive children, timestamps, Journal Dates, Hard Deletion, missing Semantic Reference targets, and restoration?

## Comments

Ticket 02 partially resolved the original flat-Journal subset in [ADR 0007](../../../docs/adr/0007-flat-journal-contract.md): persisted Pages and Entries used stable UUIDv4 identity, UTC creation/update metadata, an always-active prototype field, and ordered single-parent Containment. Moves, recursive children, deletion, restoration, and missing-reference behavior were then open.

Ticket 03 resolved the nested-Journal subset in [ADR 0008](../../../docs/adr/0008-nested-journal-outline.md): Containment is recursive, ordered, single-parent, and acyclic; local moves preserve complete subtrees and stable identity; single-Entry deletion rejects parents; and leaf deletion is permanent without a standalone restoration command. ADR 0010 later added explicit complete-subtree deletion, while lifecycle behavior for later Blocks, reference targets, and history remained open until this resolution.

Resolved by [ADR 0018](../../../docs/adr/0018-unify-page-and-entry-block-types.md),
[ADR 0019](../../../docs/adr/0019-hard-delete-blocks-with-session-undo.md),
[ADR 0020](../../../docs/adr/0020-define-block-timestamp-semantics.md),
[ADR 0021](../../../docs/adr/0021-define-journal-date-identity.md),
[ADR 0022](../../../docs/adr/0022-use-notebook-wide-session-history.md), and
[ADR 0023](../../../docs/adr/0023-enforce-page-rooted-entry-containment.md). Missing-target and
restoration effects on Semantic References are completed by ADRs 0014 and 0015.

Every Block has a fresh stable UUIDv4 and immutable type; only undo or redo may restore a deleted
identity. Hieda has one Page Block type with immutable Named and Journal kinds and one Entry Block
type whose Page Entry or Journal Entry role derives from its containing root. Every materialized
Page is a Containment root, every Entry belongs to exactly one ordered acyclic Page-rooted tree, and
complete Entry subtrees may move atomically between any Pages without changing identity or type.

Named Page deletion hard-deletes its contained Entry forest but not name-derived Page Hierarchy
descendants. Journal Pages expose no explicit deletion command, and deleting their final Entry
leaves the date identity materialized. Single-Entry deletion remains leaf-only, while explicit
subtree deletion removes the selected complete subtrees. V0 has no inactive state, tombstone,
persisted trash, or standalone restore command; one bounded Notebook-wide chronological Editing
History provides session-local atomic undo and redo.

Block Creation Time is immutable. Block Update Time advances for meaningful edits, moves, and
changes to immediate ordered children, but not mechanical maintenance; undo and redo restore
logical-state timestamps. Journal Dates are immutable timezone-free Gregorian values, and a virtual
date materializes atomically when it first receives durable content.

Name-derived Page Hierarchy was separated from Containment and continues in
[issue 16](16-define-page-hierarchy-behavior.md). The accepted Page/Entry schema and interface
refactor is tracked by
[implementation issue 19](../../notebook-v0-implementation/issues/19-unify-page-and-entry-block-types.md).
