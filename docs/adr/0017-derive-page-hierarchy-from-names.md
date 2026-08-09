# Derive Page Hierarchy from Page names

Status: accepted

Materialized Named Pages and the Page Previews required for their missing proper prefixes form a
Page Hierarchy derived from slash-separated exact names. Unresolved Page Links do not contribute
hierarchy nodes.
This hierarchy is distinct from Containment: all Pages remain Containment roots, while Entries are
contained beneath their Page. A hierarchy parent may be a current Page or only a name represented
by a Page Preview, so hierarchy placement creates no parent Block or identity. Renaming a Page
changes its hierarchy placement without moving its contained Blocks or changing any Block identity.

Each Page Name segment matches `[a-z][a-z0-9_-]{0,63}`. A name contains one or more segments
separated by single slashes, with no leading, trailing, or repeated slash and a 255-byte overall
limit. Names remain exact, case-sensitive, and unique within a Notebook.

Every proper prefix of an existing hierarchical Page Name appears as a hierarchy node. A prefix
with no current Page is represented by a Page Preview; activating or expanding it creates no Block
or identity. Consequently, the presented hierarchy remains connected even when any number of
intermediate Pages are absent.

Renaming a Named Page also renames every existing descendant Page whose name begins with the
old name followed by `/`, substituting the new prefix while preserving each Page identity and
contained outline. The Page names, affected Page Link sources, derived hierarchy, link indexes, and
Notebook-wide Editing History action form one all-or-nothing namespace edit; any resulting name
conflict or invalid rewrite rejects the complete rename.
