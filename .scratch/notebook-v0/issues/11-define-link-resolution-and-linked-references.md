# Define link resolution and linked references

Type: grilling
Status: resolved
Blocked by: 04, 05

## Question

How should page creation, title lookup, title collisions, renames, direct block selection, missing targets, link editing, deletion, and incoming linked-reference presentation behave while every persisted link continues to target stable identity?

## Comments

Resolved by [ADR 0013](../../../docs/adr/0013-rewrite-page-links-on-rename.md),
[ADR 0014](../../../docs/adr/0014-resolve-page-links-by-conceptual-name.md),
[ADR 0015](../../../docs/adr/0015-treat-all-missing-block-references-alike.md), and
[ADR 0016](../../../docs/adr/0016-page-linked-references-by-source-block.md).

Page Links literally use exact unique Page names. Missing names produce queryable Unresolved Page
Links and non-materialized Page Previews; creation, restoration, or rename into that name resolves
them atomically. Renames rewrite committed Page Link notation and session-local history without
advancing mechanically affected source Block Update Times. Deletion returns Page Links to the
unresolved name, so a later same-named Page represents the same concept with a new identity.

Block References literally use UUID identity. Deleted, mistyped, and never-existing targets share
one Missing Block Reference state and presentation; only restoration of that UUID resolves them.
Draft notation is inert until commit, while pickers insert canonical notation and rendered links
use target-derived labels without transcluding content.

Linked References deduplicate by source Block across occurrences and reference kinds, group by the
source's containing Page and Containment path, include self-references, and remain read-only
navigation previews. The Notebook returns bounded source rows and occurrence snippets through
revision-bound cursors; Page names and Block UUIDs remain independently queryable whether or not
they currently resolve.
