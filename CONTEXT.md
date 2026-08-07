# Notes

This context describes the content users create and connect in the local-first note application.

## Language

**Notebook**:
A portable collection of blocks, relationships, and notebook settings whose complete canonical contents are carried in one file.
_Avoid_: Database, workspace, graph

**Block**:
The universal addressable content entity. A block has a stable identity and a type, and can participate in typed relationships with other blocks.
_Avoid_: Node, item, record

**Journal Page**:
A block associated with one calendar date that contains that date's journal entries in an explicit order.
_Avoid_: Daily note, journal

**Journal Entry**:
A short, plain-text block for quickly capturing an idea, included in a journal page through an ordered containment relationship.
_Avoid_: Journal block, note

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
A semantic reference to a page's stable identity, presented to the user by its title.
_Avoid_: Wiki link, title reference

**Block Reference**:
A semantic reference directly targeting a block's stable identity.
_Avoid_: Page link, embed

**Linked Reference**:
An incoming semantic reference derived for the block or page it targets.
_Avoid_: Backlink, unlinked mention

**Query**:
A saved, live view that selects blocks using declarative conditions over their content, attributes, containment, and semantic references.
_Avoid_: Search, script, database query
