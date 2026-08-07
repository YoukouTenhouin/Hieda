# Keep empty Journal Pages virtual and commit flat Entries explicitly

Status: accepted

Ticket 02 presents the user's current local date without persisting an empty Journal Page. The first submitted Journal Entry creates its Page and ordered Containment atomically; later Entries insert after the focused Entry or append when none is focused. Journal Entries preserve exact valid single-line Unicode text, including empty and whitespace-only content, and receive UUIDv4 identity plus UTC creation and update timestamps. Entry edits commit on submit or focus loss, and failed commits restore the last acknowledged state. This keeps capture durable without introducing the split, nesting, deletion, authored-notation, or undo semantics reserved for later tickets.
