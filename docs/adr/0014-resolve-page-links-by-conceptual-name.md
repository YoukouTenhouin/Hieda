# Let Page names carry conceptual continuity across deletion

Status: accepted

A resolved Page Link targets the current Page identity for its exact Page name. Deleting that Page
turns its incoming Page Links into Unresolved Page Links, and later creating a Page with the same
name resolves them to the new Page identity. This deliberately treats the user-defined unique name
as the concept represented by a Page, avoiding visually identical `[[name]]` links that secretly
refer to different deleted and current Pages. Users who need a reference to one specific Block
independent of names use a Block Reference instead.

Activating an Unresolved Page Link opens a Page Preview keyed only by that exact name. The preview
shows the matching unresolved occurrences and offers explicit Page creation, but viewing it creates
neither a Page Block nor a stable identity. Creating the Page resolves all matching occurrences
atomically and turns the destination into the normal Page view.

The future Query language exposes Page-link-name matching separately from stable-target Semantic
Reference matching. A Page-link-name predicate selects source Blocks containing an exact name in
valid Page Link notation whether resolved or unresolved. A stable-target predicate selects only
current Semantic References to the specified Block identity. Renaming changes Page-link-name
matches through source rewriting; deleting a Page leaves its literal name matches intact.

If the deleted Page also had incoming Block References, those become Missing Block References to
its UUID and do not appear in the name-keyed Page Preview. Recreating the Page name resolves only
the Page Links; it cannot retarget those UUID references to the new Page identity. This follows the
literal distinction between `[[name]]` and `[[block:uuid]]` rather than attempting to infer intent.

When Page lifecycle rules permit deletion, removing the Page, converting its Page Links to
unresolved name occurrences, and converting its Block References to missing UUID occurrences form
one Notebook transaction and one revision. Source Authored Text and Block Update Times do not
change. Any failure leaves the Page and every resolution state unchanged.

Restoring the deleted Page with its original name and UUID atomically resolves both the
name-matched Unresolved Page Links and UUID-matched Missing Block References. By contrast, creating
a new Page with the same name but a new UUID resolves only the Page Links. In either case source
text and source Block Update Times remain unchanged.
