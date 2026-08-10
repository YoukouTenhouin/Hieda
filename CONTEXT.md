# Notes

This context describes the content users create and connect in the local-first note application.

## Language

**Notebook**:
A portable collection of blocks, relationships, and notebook settings whose complete canonical contents are carried in one file.
_Avoid_: Database, workspace, graph

**Block**:
The universal addressable content entity. A Block has a stable identity that ordinary creation never reuses and an immutable type, and can participate in typed relationships with other Blocks.
_Avoid_: Node, item, record

**Block Creation Time**:
The UTC instant at which a Block identity first became durable. It remains unchanged for that identity, including across movement and restoration.
_Avoid_: Import time, current location time

**Block Update Time**:
The UTC instant at which a Block was last meaningfully edited, moved, or had its immediate ordered children changed. Mechanical maintenance such as link resolution, Page Link rewriting, or descendant Page Name propagation does not advance it.
_Avoid_: Last-written time, storage timestamp

**Hard Deletion**:
Removal of a Block and its owned Containment descendants from canonical Notebook state. Only session-local Editing History may retain a restorable copy.
_Avoid_: Soft deletion, trash, inactive state

**Editing History**:
The bounded, session-local chronological sequence of semantic Notebook actions available to undo and redo. It is an editing convenience, not persisted Notebook version history.
_Avoid_: Version history, operation log, per-Page history

**Journal Page**:
A Page of Journal kind associated with one calendar date. It is a Containment root and remains non-materialized until it has durable content.
_Avoid_: Daily note, journal

**Journal Date**:
An immutable, timezone-free proleptic Gregorian date from year 1 through 9999 that identifies at most one materialized Journal Page.
_Avoid_: Timestamp, UTC date

**Entry**:
An authored-text Block with one structural home beneath a Page, possibly through other Entries. Its Block type is independent of the kind of Page that contains it.
_Avoid_: Note, item

**Journal Entry**:
An Entry whose containing root is a Journal Page.
_Avoid_: Journal Block, distinct Block type

**Page Entry**:
An Entry whose containing root is a Named Page.
_Avoid_: Distinct Block type, document

**Authored Text**:
The exact canonical Unicode source of a Journal Entry or Page Entry, including literal text and any Authored Text Notation.
_Avoid_: Rendered text, body

**Draft**:
Transient uncommitted text in an active Entry editor. A Draft is plain text with no active Authored Text Notation and does not replace the Entry's last committed derived meaning.
_Avoid_: Authored Text, pending links

**Authored Text Notation**:
Lightweight constructs within Authored Text from which Hieda derives relationships and Properties without replacing the canonical source.
_Avoid_: Authored text language, markup

**Property**:
An ordered named string value derived from Authored Text. A Block may carry multiple, duplicate-preserving values under the same name.
_Avoid_: Metadata, field

**Page**:
A Block with an immutable Named or Journal kind that acts as a Containment root for Entries.
_Avoid_: Document, article

**Page Context**:
A Page together with every Entry whose Containment root is that Page. A Page Context has the immutable Named or Journal kind of its root.
_Avoid_: Entry type, Containment subtree

**Named Page**:
A Page with an exact Page Name and display title that participates in Page Hierarchy.
_Avoid_: Ordinary Page, document

**Page Name**:
An exact, Notebook-unique hierarchical name of one or more slash-separated ASCII segments, each matching `[a-z][a-z0-9_-]{0,63}`, with at most 255 bytes overall.
_Avoid_: Display title, file path

**Display Title**:
A non-empty single-line Unicode presentation label for a Named Page that may duplicate another Page's title. It determines neither identity, Page Hierarchy, nor Page Link resolution and is never changed implicitly by a Page Name rewrite.
_Avoid_: Page Name, identifier

**Page Hierarchy**:
A name-derived organization of materialized Named Pages and the Page Previews required for their missing name prefixes, independent of Block identity and Containment. Unresolved Page Links do not contribute nodes; a hierarchical Page name's prefix identifies its hierarchy parent.
_Avoid_: Containment, folder tree

**Containment**:
A single-parent ordered relationship giving every Entry one structural home beneath exactly one materialized Page. Pages are roots; Entry parent chains are acyclic and may contain further Entries.
_Avoid_: Membership, child reference

**Semantic Reference**:
A relationship expressing a meaningful link between blocks without implying ownership, ordering, or containment.
_Avoid_: Reference, containment

**Page Link**:
A semantic reference whose literal exact Page name denotes the current Page with that name, presented to the user by its title. The name denotes the same concept if its Page is deleted and later recreated.
_Avoid_: Wiki link, title reference

**Unresolved Page Link**:
A derived occurrence of Page Link notation whose exact Page name does not identify an existing Page, including after its former Page is deleted. It remains queryable and becomes a Page Link when a Page with that name is created.
_Avoid_: Broken link, Semantic Reference, Linked Reference

**Page Preview**:
A non-materialized view for an exact Page name that has no current Page, arising from Unresolved Page Links or a missing Page Hierarchy prefix without creating a Block or stable identity.
_Avoid_: Virtual Page, empty Page

**Block Reference**:
A semantic reference whose literal UUID targets only the Block with that stable identity, independent of Page names.
_Avoid_: Page link, embed

**Missing Block Reference**:
A syntactically valid Block Reference whose identity does not identify a current Block, whether because the Block was deleted or never existed.
_Avoid_: Dangling reference, deleted reference

**Linked Reference**:
An incoming semantic reference derived for the block or page it targets.
_Avoid_: Backlink, unlinked mention

**Query**:
A live selection of Blocks defined by Query Notation in an ordinary Entry. Query behavior does not change the Entry's Block type or identity, and its containing Entry remains selectable by Queries, including its own.
_Avoid_: Search, script, database query

**Query Result**:
An ordered, revision-bound projection of the current Blocks selected by a Query. An Entry result uses its normal committed Authored Text presentation without recursively rendering its own Query Results; Query Results are derived views rather than durable Blocks or Containment children.
_Avoid_: Child Entry, copied Block, Search Result

**Query Error**:
A source-located diagnostic for committed Authored Text that declares Query intent but cannot define a valid Query. The text remains editable, while no partial or prior Query executes or supplies Query Results.
_Avoid_: Storage error, partial Query Result, rejected Authored Text

**Query Notation**:
An Authored Text Notation construct containing a Hieda-defined S-expression with one filter clause followed by optional ordering and limit clauses. It defines a Query and gives its containing Entry an expandable Query Result area, activates Query behavior only when it is the Entry's sole non-whitespace content, and makes that entire Entry opaque to every other Authored Text Notation parser.
_Avoid_: Query Block, Query Entry type, executable code

**Query Anchor**:
A target selector within Query Notation, expressed as `self`, an exact Page Name in double brackets, or a Block UUID in Block Reference spelling. A Page-name or UUID anchor used directly as a Boolean predicate selects Entries containing that exact authored reference notation regardless of resolution; explicit relationship predicates use anchors as endpoints. A Query Anchor creates no Semantic Reference or Linked Reference, and a resolved Page-name anchor follows its Page through rename and otherwise follows Page Name continuity through deletion and recreation.
_Avoid_: Page Link, Block Reference, Semantic Reference
