# Define the v0 Query language

Status: accepted

Hieda uses a small application-defined S-expression DSL embedded as the sole content of an
ordinary Entry. This keeps saved Queries inside the existing Block, Containment, persistence, and
editing model while providing an unambiguous declarative language that exposes neither arbitrary
execution nor storage-engine syntax. Exact Authored Text is the authoritative saved definition;
parsed forms, execution plans, cursors, diagnostics, Query Results, and expansion state are derived
or session-local.

## Entry and presentation model

After trimming leading and trailing whitespace, an Entry declares Query intent when it begins with
the reserved `{{query` token. A valid Query occupies the complete non-whitespace content and has
exactly one `where` clause followed by optional `order-by` and `limit` clauses in that order:

```text
{{query
  (where
    (and
      (type entry)
      (property-equals status "open")))
  (order-by update-time desc)
  (limit 20)
}}
```

Query behavior changes neither the containing Entry's Block type nor its stable identity. The Entry
participates in Queries like every other Entry and may match its own Query. It may also retain
ordinary contained child Entries. Query Results have their own disclosure control, independent of
the child-outline disclosure; a newly committed or reopened valid Query defaults to expanded, and
subsequent expansion state is session-local.

An expanded Query Result is an ordered read-only projection, never durable content or Containment.
It contains one row per matching Block. An Entry row uses the same committed Authored Text
presentation as the Entry at its structural home, including rendered links, references, and future
notation styles, but never recursively renders that Entry's Query Results. A Page row presents its
Journal Date or its Display Title and exact Page Name. Every row identifies the stable Block and
activating it navigates to and selects the original Block; result rows are not edited in place.

While the containing Entry has an active Draft, the UI shows raw Query source and hides its Query
Result or Query Error. Other readers continue to observe the last committed state. Committing a
valid Draft replaces the prior Query atomically; committing invalid Query intent saves the exact
Authored Text but executes nothing, removes prior results, and shows a Query Error. Canceling the
Draft restores the previous committed presentation.

Query intent makes the complete Entry opaque to the ordinary Property, Page Link, and Block
Reference parsers, whether the Query is valid or invalid. Query Anchors therefore never create
Semantic References or Linked References. Escaping the Query opener with the existing Authored Text
escape rule suppresses only Query Notation; the resulting ordinary Entry is then processed normally
by the other notation parsers, including any notation inside the escaped construct.

## Lexical and structural grammar

The DSL is S-expression-shaped but is not a Lisp reader. Keywords, predicate names, enum values,
and directions are lowercase ASCII and case-sensitive. ASCII space and LF separate tokens;
indentation is insignificant. Unknown forms, duplicate top-level clauses, misplaced clauses, extra
operands, and missing operands are Query Errors rather than ignored extensions.

Strings are double-quoted Unicode. They support only `\"`, `\\`, and `\n`; an unescaped quote, raw
newline, or any other escape is invalid. Dates use exact `YYYY-MM-DD` Journal Date syntax and must
denote a valid date. Limits are unsigned base-ten integers greater than zero. The Query model imposes
no arbitrary product cap, although an integer that cannot be represented by its portable unsigned
value is invalid. The language has no comments, commas, quoting forms, reader macros, user-defined
symbols, functions, aggregation, or scripting.

The top-level shape is:

```text
{{query
  (where predicate)
  (order-by sort-key direction)?
  (limit positive-integer)?
}}
```

`and` and `or` each require at least two predicate operands, `not` requires exactly one, and all
three may nest arbitrarily within the safe limits of the bounded Authored Text input:

```text
(and predicate predicate ...)
(or predicate predicate ...)
(not predicate)
```

There is no implicit conjunction. `(all)` matches every current Block; `(not (all))` supplies the
empty selection without a separate `none` form.

## Basic predicates

Intrinsic type has exactly two values:

```text
(type entry)
(type page)
```

There are no Query, Journal Entry, Page Entry, Named Page, or Journal Page Block types. Page context
is expressed separately and matches both a Page root of the given kind and every Entry whose
Containment chain ends at it:

```text
(page-context named)
(page-context journal)
```

Journal Date predicates likewise apply to the complete Journal Page Context. Named Page Contexts
never match. Ranges compose with Boolean operators:

```text
(journal-date = 2026-08-10)
(journal-date < 2026-08-10)
(journal-date <= 2026-08-10)
(journal-date > 2026-08-10)
(journal-date >= 2026-08-10)
```

`(text-contains "value")` performs a literal, case-sensitive, normalization-sensitive substring
test against exact Authored Text. It includes Property lines and authored notation text, while Pages
have no Authored Text and do not match. The empty needle is invalid. Linguistic normalization,
case-folding, tokenization, and relevance remain the separate full-text Search contract.

Properties expose existence and exact string equality only:

```text
(property-exists property-key)
(property-equals property-key "value")
```

The key uses the existing Page Name grammar. Equality is case- and normalization-sensitive and is
true when at least one duplicate-preserving occurrence has that exact value; the empty value is
valid. Duplicates do not otherwise change predicate truth. `not` expresses absence or lack of an
exact value.

## Query Anchors and relationships

A Query Anchor is one of:

```text
self
[[exact/page_name]]
[[block:550e8400-e29b-41d4-a716-446655440000]]
```

`self` denotes the Entry containing the Query. A Page-name anchor denotes the current Page with that
exact Page Name; while resolved, Page rename rewrites it as mechanical maintenance without advancing
the Query Entry's Block Update Time. Deletion leaves it unresolved and matching no relationship
endpoint, while later creation of the exact name resolves it again. A UUID anchor denotes only the
current Block with that identity and remains missing when none exists. A missing or contextually
inapplicable anchor makes its relationship predicate false rather than invalid.

Containment provides four directional proper-relationship predicates. None includes the anchor
itself, and v0 adds no depth, sibling-position, or structural-before/after predicate:

```text
(child-of anchor)
(descendant-of anchor)
(parent-of anchor)
(ancestor-of anchor)
```

`in-page-subtree` takes a Page-name anchor and applies the inclusive Page Hierarchy behavior from
ADR 0024:

```text
(in-page-subtree [[projects/hieda]])
```

It matches materialized Named Pages at or below the exact slash-segment-bounded root and Entries by
their containing Named Page. It matches no Journal Page Context and never returns Page Previews. The
root may be a materialized Page, a required Page Preview, or absent from the current hierarchy.

Reference-shaped anchors used directly as Boolean predicates provide the common authored-literal
tests:

```text
[[page_name]]
[[block:550e8400-e29b-41d4-a716-446655440000]]
```

The Page form selects candidate Entries containing that exact valid Page Link name whether resolved
or unresolved. The UUID form selects candidate Entries containing that exact valid Block Reference
UUID whether resolved or missing. These tests inspect derived ordinary authored-reference
occurrences, not Query Anchors inside opaque Query Notation; duplicate occurrences do not change
truth. `self` is not a standalone Boolean predicate.

Resolved Semantic Reference traversal is explicit:

```text
(page-links-to anchor)
(block-references anchor)
(linked-by source-anchor)
(block-referenced-by source-anchor)
```

`page-links-to` and `block-references` select candidate source Blocks with the corresponding
resolved outgoing Semantic Reference to the anchored target. `linked-by` and
`block-referenced-by` select candidate targets reached by the corresponding resolved reference from
one anchored source Block. A Page used as a source naturally matches nothing because Pages author no
references; a Page anchor is never expanded implicitly to all Entries in its Page Context. The two
reference kinds combine through `and`, `or`, and `not` rather than a generic alias.

Valid Page-name Query Anchors participate in the same all-or-nothing Page rename maintenance as Page
Links while remaining absent from Semantic and Linked References. Rewriting, reparsing, indexes,
Query invalidation, size validation, Notebook revision, and undo or redo effects commit atomically.
Malformed Query intent owns no executable Query or relationship endpoints until corrected.

## Ordering and limits

Without `order-by`, results sort by descending Block Update Time and then ascending stable Block
identity. Explicit ordering supports only:

```text
(order-by update-time asc|desc)
(order-by creation-time asc|desc)
(order-by journal-date asc|desc)
```

Creation and Update Time ties use ascending stable identity. Journal Date ordering places all
Journal Page Contexts before all Named Page Contexts regardless of direction. Dates follow the
requested direction; within each date, the Journal Page root comes first and matching Entries retain
their visible depth-first outline order. Descending date order does not reverse the outline inside a
day. Named-context results that remain after the dated results fall back to ascending stable
identity. Entry moves and reordering update this live secondary order.

`limit` applies after filtering and ordering. Omitting it leaves the logical Query unbounded. Query
Results are nevertheless delivered in deterministic batches of at most 100 owned rows through
opaque cursors bound to both the Query and producing Notebook revision. V0 does not require an exact
total count. Exhausting the Query or explicit limit yields no continuation cursor; any committed
revision change makes an old cursor stale and requires a fresh first batch.

## Errors and live execution

Committed Authored Text that begins with the reserved Query token but does not satisfy the grammar
produces one primary source-located Query Error with a plain-language message and, where useful, the
expected construct. The exact invalid source remains durable and editable. No partial expression or
previous valid Query executes, and invalid Query text never reaches a database-query or scripting
engine. Text without Query intent remains an ordinary Entry and receives no Query diagnostic.

Each Query Result batch observes one complete committed Notebook revision. Any committed change may
invalidate the batch in v0; dependency-aware invalidation is a future optimization. Expanded,
visible Queries restart automatically at the newest revision, while collapsed or off-screen Queries
do no execution work. The UI may retain old rows only when visibly marked as refreshing and never
present them as current.

Batch execution does not block the Qt UI thread. Expansion shows a loading state until the first
batch arrives; collapse cancels or discards outstanding work. Responses made obsolete by a newer
revision are discarded and rapid commits may be coalesced. The approximate 200-millisecond target
applies to representative first batches on the reference Notebook; it is not a timeout that turns a
slower correct Query into an error.

## Consequences

The S-expression grammar makes composition and error locations straightforward and avoids operator
precedence, at the cost of a less conventional syntax for non-Lisp users. Making a Query an ordinary
Entry avoids a new Block type, saved-query registry, and special structural home, while whole-Entry
opacity prevents its anchors and property operands from contaminating the authored relationship and
Property model. Revision-bound incremental results keep broad Queries usable without arbitrary
logical limits or exact-count scans. Additional predicates, sort keys, caching, dependency-aware
refresh, and richer result shaping remain compatible future extensions but are not v0 behavior.
