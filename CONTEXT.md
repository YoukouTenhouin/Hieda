# Notes

This context describes the content users create and connect in the local-first note application.

## Language

**Notebook**:
A portable collection of blocks, relationships, and notebook settings whose complete canonical contents are carried in one file.
_Avoid_: Database, workspace, graph

**Block**:
The universal addressable content entity. A block has a stable identity and a type, and can participate in typed relationships with other blocks.
_Avoid_: Node, item, record

**Block Update Time**:
The time at which a Block was last meaningfully modified as the subject of a user action. Mechanical maintenance such as rewriting its Page Link names after a target rename does not advance it.
_Avoid_: Last-written time, storage timestamp

**Journal Page**:
A block associated with one calendar date that contains that date's journal entries in an explicit order.
_Avoid_: Daily note, journal

**Journal Entry**:
A short, plain-text block for quickly capturing an idea, included in a journal page through an ordered containment relationship.
_Avoid_: Journal block, note

**Page Entry**:
A short authored-text block contained in an ordinary Page. It has the same outline editing behavior as a Journal Entry but remains a distinct Block type.
_Avoid_: Journal Entry, document

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
A titled block that provides a stable destination for organizing and linking content. A journal page is a date-associated specialization of a page.
_Avoid_: Document, article

**Containment**:
An ordered relationship expressing that one block includes another as part of its presented content. A block has at most one containment parent, while contained blocks may themselves contain children.
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
A non-materialized view for an exact Page name that has no current Page, showing its Unresolved Page Links without creating a Block or stable identity.
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
A saved, live view that selects blocks using declarative conditions over their content, attributes, containment, and semantic references.
_Avoid_: Search, script, database query
