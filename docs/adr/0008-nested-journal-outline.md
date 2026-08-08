# Use local subtree editing and leaf-only deletion in the Journal

Status: accepted

Ticket 03 extends each Journal Page into a recursive ordered outline while keeping the public
`JournalPage` read in depth-first visible order. Each Journal Entry reports its optional parent
Entry identity; a missing parent means the Entry is directly contained by the Journal Page.
Containment remains single-parent and acyclic, and every structural command commits its complete
Block, ordering, and reverse-index changes in one Notebook transaction.

Enter splits the complete edited text at a valid UTF-8 boundary. The original Entry retains the
prefix, identity, and existing children; a new following sibling receives the suffix. Backspace at
the start joins a leaf into the previous visible Entry without adding a separator, preserving the
previous Entry's identity. An Entry with children cannot be joined or deleted. Leaf deletion is
hard deletion, has no standalone restoration command, and leaves an already materialized Journal
Page in place. Ticket 04 must retain the deleted state itself if undo is to recreate it.

Indent makes a subtree the previous sibling's last child. Outdent places it after its parent, and
move-up or move-down swaps the complete subtree with one sibling without crossing a parent level.
The moved Entry and containers whose immediate ordered children change receive updated timestamps;
unchanged descendants retain theirs. Invalid moves and failed commits leave the complete outline
and Notebook revision unchanged.

The Qt Quick adapter presents nesting with platform-derived metrics. Tab and Shift+Tab indent and
outdent. Control+Shift+Up/Down on Windows and Linux and Command+Shift+Up/Down on macOS reorder
siblings. A bullet context menu exposes the same structural actions plus leaf deletion, disabling
commands that cannot succeed.
