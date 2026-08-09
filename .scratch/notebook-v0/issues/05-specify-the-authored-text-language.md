# Specify the authored text language

Type: prototype
Status: resolved

## Question

What minimal authored-text grammar should v0 expose for plain text, page links, direct block references, properties, escaping, incomplete input, and parsing errors, and which parsed structures become transactionally maintained relationships or attributes?

## Comments

Ticket 02 partially resolved the original plain-text subset in
[ADR 0007](../../../docs/adr/0007-flat-journal-contract.md): Journal Entry text was exact valid
single-line Unicode, including empty and whitespace-only content. Links, references, properties,
escaping, and parsing were left for this ticket; subsequent editing tickets added LF and ordinary
Page Entries before this decision was completed.

Resolved as **Authored Text Notation** by
[ADR 0012](../../../docs/adr/0012-permissive-authored-text-notation.md). Authored Text remains
canonical under a permissive, line-local grammar for Page Links, Block References, Properties, and
escaping; malformed notation remains text, and committed derived meaning is replaced
transactionally. The decision was validated by the in-memory terminal prototype at commit
`25a1de7` on branch `prototype/authored-text-notation`.
