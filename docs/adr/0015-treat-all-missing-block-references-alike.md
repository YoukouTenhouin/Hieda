# Treat all missing Block References alike

Status: accepted

Deleting a Block does not prevent the deletion or retarget its incoming Block References. A
syntactically valid Block Reference whose UUID identifies no current Block is a Missing Block
Reference and has the same behavior and presentation whether the Block was deleted, the UUID was
mistyped, or it never existed. The Notebook does not preserve or expose that history; restoring a
Block with the same stable identity naturally makes the reference resolvable again.

Activating a committed Missing Block Reference does not navigate or create anything. The
application keeps the current focus and reports a non-modal “Block not found” status; a context
action may copy the canonical UUID. The missing-reference rendering and interaction expose no clue
about whether a target formerly existed.

The future Query language may match Block Reference notation by its literal UUID independently of
resolution, so sources remain discoverable while the target is missing. Stable-target Semantic
Reference matching remains a separate predicate and includes only currently resolved targets.
