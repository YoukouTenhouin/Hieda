# Rewrite resolved Page Links when renaming a Page

Status: accepted

Renaming a Page atomically rewrites the Page-name body of every resolved Page Link that targets
that Page. The reverse-reference index identifies the affected Authored Text occurrences, and each
persisted Semantic Reference continues to target the same stable Page identity throughout the
Notebook-wide content edit. This keeps canonical Authored Text current without retaining permanent
name aliases or relying on occurrence matching to recover identity during later edits; the accepted
cost is that Page rename work grows with the number of incoming Page Link occurrences.

Outside that explicit rename operation, Page Link editing is literal. Changing a committed link
body removes its former Semantic Reference and resolves the replacement exact Page name from
scratch; the replacement either targets that name's current Page or becomes an Unresolved Page
Link. No prior target identity remains hidden behind changed Authored Text.

The automatic rewrites preserve each source Block's Block Update Time because they are mechanical
maintenance rather than direct edits of those source Blocks. The renamed Page's update time and the
Notebook revision advance normally. Consequently, a rename does not make all incoming-reference
contexts appear recently edited.

Before writing, rename validates the new Page name and title plus every resulting Authored Text
value, including the 1 MiB limit. The Page mutation and all source rewrites then commit in one
Notebook transaction. Any validation or persistence failure rejects the entire rename without
changing the Page, its links, source text, timestamps, or Notebook revision.

Page rename is one Notebook-wide Editing History action containing the Page mutation and every Page
Link rewrite. Undo and redo restore the corresponding complete logical states atomically. This
supersedes the earlier page-local-history workaround that rebased stored Authored Text and excluded
rename itself from history; Notebook-wide chronological history preserves the necessary ordering
without that exception.

When the new name already has Unresolved Page Links, the same transaction resolves them to the
renamed Page while rewriting links from the former name. Thus rename and Page creation have the
same resolution effect for pre-existing unresolved occurrences of the resulting exact name.
