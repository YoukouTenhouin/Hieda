# Define Page Hierarchy behavior

Type: grilling
Status: open
Blocked by: 05, 11

## Question

What are the exact lookup, creation, rename-cascade, collision, deletion, ordering, navigation,
query, history, transaction, and performance semantics for the slash-derived Page Hierarchy and
its non-materialized Page Preview ancestors?

## Comments

Issue 04 identified Page Hierarchy as a separate name-derived organization rather than Block
Containment. [ADR 0017](../../../docs/adr/0017-derive-page-hierarchy-from-names.md) establishes exact
slash-separated Page Names, visible Page Preview nodes for missing prefixes, and atomic descendant
prefix propagation when a Page is renamed. The remaining hierarchy-specific edge cases belong here
so the Block lifecycle ticket can settle identity, Containment, timestamps, and deletion without
conflating structural ownership with name-based organization.
