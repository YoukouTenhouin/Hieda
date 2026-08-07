# Define journal editing behavior

Type: prototype
Status: open
Blocked by: 03, 04, 05

## Question

What observable keyboard and mouse behavior should the journal outliner provide for creating, splitting, joining, nesting, moving, selecting, and undoing entries while preserving the agreed block invariants?

## Comments

Ticket 02 partially resolves flat capture in [ADR 0007](../../../docs/adr/0007-flat-journal-contract.md): new Entries insert after focus or append, edits commit on submit/focus loss, and rejected edits restore acknowledged text. Splitting, joining, nesting, moving, selection, and undo remain open.
