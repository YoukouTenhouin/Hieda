# Define Block timestamps by logical user state

Status: accepted

Every durable Block receives an immutable Block Creation Time and a Block Update Time, stored as UTC
instants with microsecond precision. Meaningful edits and movement update the subject Block;
Containment changes also update each container whose immediate ordered children changed. Moving a
subtree therefore updates its root and old and new immediate containers while leaving unchanged
descendants and higher ancestors untouched. Presentation alone localizes these instants.

Mechanical maintenance—including link resolution and automatic Page Link rewrites—does not advance
affected source times. Session-local undo and redo restore the timestamps belonging to the restored
logical state instead of stamping the reversal moment. These semantics make Block Update Time
useful for user-facing recency rather than exposing internal storage writes.
