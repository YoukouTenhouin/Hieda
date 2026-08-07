# Specify block lifecycle and structural invariants

Type: grilling
Status: open

## Question

What are the exact invariants and user-visible semantics for block identity, types, ordered containment, moves, recursive children, timestamps, journal dates, deletion, dangling semantic references, and restoration?

## Comments

Ticket 02 partially resolves the flat-Journal subset in [ADR 0007](../../../docs/adr/0007-flat-journal-contract.md): persisted Pages and Entries use stable UUIDv4 identity, UTC creation/update metadata, active lifecycle state, and ordered single-parent Containment. Moves, recursive children, deletion, restoration, and dangling-reference behavior remain open.
