# Use versioned binary records in bounded LMDB databases

Status: accepted

The canonical Notebook uses LMDB with `MDB_NOSUBDIR`, default synchronous durability, one environment owner, a 64-bit process, an initial 8 GiB map, and at most sixteen named databases. Keys use explicit big-endian byte encodings and values use strictly bounded, versioned tag-length-value records. UUIDv4 values are stored as 16 bytes; text is exact UTF-8; timestamps are signed UTC microseconds. The schema separates metadata/settings, Blocks and type indexes, Containment in both directions, Semantic References in both directions, parsed properties and property indexes, and Page title/date indexes. All canonical and derived updates for one command share a write transaction.

Containment provisionally uses gapped 64-bit ranks. The physical `pages_by_title` database reserved
by schema v1 is used as the unique Page-name index from ticket 06 onward; display titles remain on
Page Block records and may duplicate. Retaining the original database name keeps existing schema-v1
Notebooks compatible without a migration. ADR 0019 settles hard deletion as the complete v0 Block
lifecycle; the early prototype removes its always-active record field rather than preserving a
meaningless compatibility placeholder. Schema v1 has no older supported application schema. When a later release
declares an older schema supported, it will migrate through a verified temporary sibling copy that
is atomically published while the original is retained as a pre-migration backup; newer schemas are
rejected without modification.
