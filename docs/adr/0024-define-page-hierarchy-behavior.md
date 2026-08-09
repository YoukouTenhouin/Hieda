# Define Page Hierarchy behavior

Status: accepted

Page Hierarchy contains every materialized Named Page and only the Page Previews required for
missing proper prefixes of materialized Page Names. Unresolved Page Links may open the same
canonical name-keyed Page Preview destination, but do not contribute hierarchy nodes. Exact
hierarchy lookup therefore returns a materialized Page, a required ancestor preview, or not found;
Queries likewise return only Blocks and never Page Previews. A hierarchy-subtree predicate matches
materialized Named Pages at or below an exact valid Page Name on slash-segment boundaries, and may
test Entries through their containing Named Page. Its root need not be materialized or currently
appear in the hierarchy.

Each parent orders its immediate children by ascending bytewise ASCII Page Name segment, regardless
of materialization, Display Title, creation time, or recent use. The Notebook enumerates roots and
immediate children lazily in deterministic batches of 100 through opaque revision-bound cursors;
stale cursors require restarting that parent's enumeration. Each result identifies whether the node
is materialized and whether it has children, while exact lookup remains a separate indexed read.

Creation materializes exactly the requested Page and no ancestors. It is one Notebook-wide Editing
History action: undo removes the Page and redo restores its identity and complete logical state.
Deleting a Named Page never deletes hierarchy descendants. Its node becomes a Page Preview while a
materialized descendant requires the prefix and otherwise disappears; unresolved links alone do
not retain it. Deleting the currently viewed Page leaves navigation at the same exact name as a
Page Preview, which undo replaces with the restored Page.

A rename cascade validates collisions against the final namespace. Names vacated by Pages in the
same cascade are available, Page Previews never collide, and a conflict with a materialized Page
outside the cascade or any invalid generated name rejects the operation. Only the explicitly
renamed Page may change its Display Title or advance its Block Update Time. Descendant titles remain
exactly unchanged, while descendant Page Name propagation is mechanical maintenance and preserves
their update times. Page Links resolved to every renamed Page identity are rewritten; unresolved
links that merely share the old prefix remain literal and unchanged because no target identity
establishes that they belong to the renamed concept. Unresolved links exactly matching any final
Page Name resolve to that renamed Page.

Page creation, rename, deletion, undo, and redo each commit their complete Block, hierarchy, link,
index, timestamp, and history effects as one transaction, advance the Notebook
revision once, and produce one post-commit notification. Failure changes none of them. Rename is one
Notebook-wide undoable action covering the selected Page name and title, descendant name
propagation, and Page Link rewrites; undo and redo restore the corresponding complete logical states
instead of rebasing older history. Supplying the existing name and identical title is a successful
no-op with no transaction, revision, timestamp, notification, or history action.

Activating any Page Preview opens the same non-materialized destination, shows unresolved
references for its exact name, and offers explicit creation. Creation turns that current
destination into the materialized Page in place. Hierarchy expansion is presentation state: it is
session-local, absent from Notebook revisions and Editing History, and initially collapsed except
for ancestors needed to reveal the current destination. Later navigation expands the destination's
ancestors and selects its node when it belongs to the hierarchy; a link-only preview has no selected
hierarchy node. Materialized nodes present their Display Title with their local segment as
disambiguation; previews expose their segment and unmaterialized state, while full exact names and
preview status remain accessible without relying on color.

Exact hierarchy lookup and every 100-node enumeration batch join the approximate 200-millisecond
acceptance measurement on the 250,000-Block reference Notebook and documented reference hardware.
Rename cost may grow with affected Pages, links, and history state: v0 imposes no arbitrary cascade
limit and preserves one atomic commit, so pathological cascades are not subject to that interaction
target. The Qt adapter runs potentially large cascades without blocking its UI thread and presents
an indeterminate busy state.
