# Present Linked References as bounded source-Block results

Status: accepted

Linked References present one read-only result per source Block, deduplicating repeated occurrences
and different reference kinds while retaining their counts and kinds. Results are grouped by the
source's containing Page and show its Containment path; self-references are included. Page groups
sort by their most recently user-edited matching source, newest first with stable-identity tie
breaks, while rows retain current outline order. Activating a row navigates to its structural home,
reveals collapsed ancestors, and selects the source rather than editing it in the reference view.

The Notebook interface returns deterministic batches of 100 source rows through opaque cursors
bound to the producing Notebook revision. A stale cursor forces a first-page refresh rather than
risk skips or duplicates. A response includes the exact total source-Block count; each row initially
returns at most three source-ordered occurrence snippets and its exact occurrence count, with
further snippets loaded incrementally. These bounds keep pathological fan-in and repeated
occurrences from violating interactive behavior or forcing the UI to materialize unusable result
sets.

Pages expose the view below their outline, collapsed by default with session-local expansion state;
Page Previews show their analogous Unresolved Page Link results expanded. Any Block can open the
same grouped view in an expanded side panel. The normal Page and panel views use Semantic
References, while a Page Preview uses name-matched unresolved occurrences rather than pretending
that a target Block exists.
