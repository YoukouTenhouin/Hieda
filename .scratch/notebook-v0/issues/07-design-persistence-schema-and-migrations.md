# Design persistence schema and migrations

Type: grilling
Status: resolved
Blocked by: 01, 04, 05

## Question

How should blocks, typed relationships, order, parsed attributes, metadata, notebook settings, indexes, and schema versions be represented transactionally over LMDB while keeping storage replaceable and migrations recoverable?

## Comments

Resolved by [ADR 0002](../../../docs/adr/0002-lmdb-schema-and-migrations.md). The schema uses bounded LMDB databases, explicit portable TLV records, UUIDv4 identity, paired relationship indexes, and verified shadow-copy migrations. Lifecycle and authored-text choices not required by the schema remain in their own tickets.
