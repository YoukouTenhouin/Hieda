# Separate Page names from duplicate-capable display titles

Status: accepted

Ticket 06 gives every ordinary Page a stable Block identity, a user-chosen name matching
`[a-z][a-z0-9_-]{0,63}`, and a non-empty single-line Unicode display title. Names are exact and
unique within a Notebook so later authored references have an unambiguous identifier. Display
titles are presentation only, preserve exact text, and may duplicate. Creation persists an empty
Page immediately; renaming changes the name and title atomically without changing Page identity,
contained Block identity, or ordering.

Ordinary Pages contain Page Entry Blocks rather than Journal Entries. Page Entries intentionally
share the Journal outline interactions—creation, text editing, split, join, nesting, sibling moves,
subtree deletion, and session-local Page history—while remaining a distinct public and persisted
Block type. The Qt adapter uses one editor presentation and dispatches to the matching Notebook
commands. A sidebar exposes ordinary Pages alongside previous, next, and Today Journal navigation;
New Page, Go to Page, and Rename Page dialogs provide keyboard-accessible command paths.

The schema-v1 `pages_by_title` physical database was reserved before title semantics were settled.
It now stores the unique name-to-identity index. Display titles are stored in the Page Block and are
not indexed by that database. This avoids rewriting valid Notebooks created by earlier tickets.

Changing only a display title rewrites no Authored Text and changes no Semantic Reference or source
Block Update Time. Resolved Page Links and Block References obtain the current title when rendered.
Selection lists and Linked Reference group labels disambiguate duplicate display titles as
`Display Title — page_name`; exact Page-name lookup remains unaffected by title collisions.
