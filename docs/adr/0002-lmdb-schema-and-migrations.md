# Use versioned binary records in bounded LMDB databases

Status: accepted

The canonical Notebook uses LMDB with `MDB_NOSUBDIR`, default synchronous durability, one environment owner, a 64-bit process, an initial 8 GiB map, and at most sixteen named databases. Keys use explicit big-endian byte encodings and values use strictly bounded, versioned tag-length-value records. UUIDv4 values are stored as 16 bytes; text is exact UTF-8; timestamps are signed UTC microseconds. The schema separates metadata/settings, Blocks and type indexes, Containment in both directions, Semantic References in both directions, parsed properties and property indexes, and Page title/date indexes. All canonical and derived updates for one command share a write transaction.

Containment provisionally uses gapped 64-bit ranks, title indexes permit duplicate titles, and Block records reserve lifecycle state without deciding later deletion behavior. These choices keep the schema usable while the lifecycle and authored-text tickets remain open. Schema v1 has no older supported application schema. When a later release declares an older schema supported, it will migrate through a verified temporary sibling copy that is atomically published while the original is retained as a pre-migration backup; newer schemas are rejected without modification.
