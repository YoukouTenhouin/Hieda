# Define Page Hierarchy behavior

Type: grilling
Status: resolved
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

Resolved by [ADR 0024](../../../docs/adr/0024-define-page-hierarchy-behavior.md). Hierarchy membership
comes exclusively from materialized Named Pages and their required missing-prefix previews; links
do not populate it. Lookup is exact, siblings use bytewise ASCII segment order, preview navigation
never materializes content, and Queries use inclusive slash-boundary subtree semantics over Blocks.
Creation, deletion, and rename cascades have atomic Notebook-wide undo/redo behavior, with final-
namespace collision validation and mechanical descendant name propagation. Lazy revision-bound
100-node enumeration and explicit reference-workload latency measurements bound normal reads while
leaving arbitrarily large rename cascades uncapped and atomic.
