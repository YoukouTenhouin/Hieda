# Unify Page and Entry Block types

Status: accepted

Hieda persists one immutable Page Block type with immutable Named and Journal kinds, and one
immutable Entry Block type accepted beneath either kind of Page. “Page Entry” and “Journal Entry”
are contextual roles derived from the containing root rather than Block types. This preserves
identity while moving Entry subtrees between any Pages, removes duplicated outline behavior from
the Notebook interface, and lets Queries distinguish context through Page kind, Page Name, or
Journal Date instead of artificial Entry types.

Named and Journal Page semantics remain distinct: Named Pages are explicitly created, titled, and
participate in Page Hierarchy; Journal Pages are date-addressed Containment roots and remain virtual
until durable content exists. Neither Page kind nor any Block type can change during an identity's
lifetime. The accepted cost is a schema, interface, history, adapter, and test refactor while the
project is still an early prototype.
