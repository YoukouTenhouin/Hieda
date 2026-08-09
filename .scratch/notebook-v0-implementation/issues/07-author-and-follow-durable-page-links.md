# 07 — Author and follow durable Page Links

**Parent spec:** [Notebook v0](../../notebook-v0/spec.md)

**What to build:** Parse literal Page Name notation in Authored Text, maintain resolved or unresolved Page Link state, and let users follow committed links or open Page Previews. Incomplete and invalid notation remains editable, while Named Page rename atomically rewrites affected source notation.

**Blocked by:** 06 — Create, navigate, and rename Pages; 19 — Unify Page and Entry Block types;
20 — Browse and materialize Page Hierarchy;
[Specify the authored text language](../../notebook-v0/issues/05-specify-the-authored-text-language.md);
[Define link resolution and linked references](../../notebook-v0/issues/11-define-link-resolution-and-linked-references.md)

**Status:** completed

- [x] Entering valid Page Link notation resolves the exact Page Name or creates a queryable Unresolved Page Link without materializing a Page.
- [x] Following a resolved Page Link navigates to its Named Page; following an unresolved link opens its non-materialized Page Preview.
- [x] A Page Preview shows the matching unresolved Page Link sources for its exact name, whether
  reached from a link or from Page Hierarchy navigation.
- [x] Renaming a Named Page rewrites all affected committed Page Link source and preserves resolved identity, source Block Update Times, and atomic failure behavior.
- [x] Deleting a Named Page makes its incoming Page Links unresolved without changing their source
  text or source Block Update Times; undo restores their original resolution atomically.
- [x] Incomplete, escaped, malformed, and unresolved Page Link text remains losslessly editable and follows the resolved behavior.
- [x] Editing or removing link notation updates the canonical Semantic Reference in the same acknowledged command.
- [x] Parser and behavioral tests cover hierarchical Page Names, display titles, escape rules, inert Drafts, rename rewriting, deletion/recreation, and reopen.

Existing schema-v2 Notebooks backfill the derived Page Link indexes when first opened without
advancing the Notebook revision. The Qt adapter renders dense Unicode link sets with an incremental
cursor, and resolved display titles participate in outline-row sizing. The complete 73-test suite
passes on both the development and reflow trees.
