# Specify the authored text language

Type: prototype
Status: open

## Question

What minimal authored-text grammar should v0 expose for plain text, page links, direct block references, properties, escaping, incomplete input, and parsing errors, and which parsed structures become transactionally maintained relationships or attributes?

## Comments

Ticket 02 partially resolves the plain-text subset in [ADR 0007](../../../docs/adr/0007-flat-journal-contract.md): Journal Entry text is exact valid single-line Unicode, including empty and whitespace-only content. Links, references, properties, escaping, and incremental parsing remain open.
