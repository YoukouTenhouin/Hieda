# Keep Journal history semantic and session-local

Status: superseded by ADR-0022

Ticket 04 adds undo and redo at the `NotebookSession` interface. History records complete
before-and-after Journal Page domain state rather than LMDB operations, so text, stable Block
identity, timestamps, Containment, ordering, deletion, and virtual Page state are restored
together. Each undo or redo is a new durable Notebook transaction and revision. A failed commit
leaves both the acknowledged Page and the history position unchanged.

History is independent per Journal date, lives only while its Notebook is open, and shares a
32 MiB estimated memory budget across Pages. The oldest actions are evicted first; the newest
action remains available even when it alone exceeds the budget. Closing or reopening clears all
history, and no operation log or schema change is introduced. A successful new command clears
that Page's redo branch; rejected and no-op commands do not.

An existing Entry is committed after one second of typing inactivity or immediately at an editing
transition. Each commit is one text action. Split, join, indent, outdent, and sibling moves include
pending text in their single compound structural action. Draft submission remains one insertion
action. The Qt adapter does not commit while an input method reports active composition; complete
IME behavior remains ticket 05 work.
