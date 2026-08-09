# Hard-delete Blocks with session-local undo

Status: accepted

Named Page deletion removes the Page and its complete contained Entry forest in one durable
transaction. It does not remove Page Hierarchy descendants because Page Hierarchy is not
Containment. Incoming references transition under ADRs 0014 and 0015, while one session-local undo
action can restore the complete deleted state—including identities, timestamps, Containment, and
resolution. V0 adds no persisted trash or standalone restore command.

Journal Pages expose no explicit deletion command. Deleting their final Entry leaves the
materialized date Page and its identity in place. Only session-local undo of the action that first
materialized a Journal Page may restore that date to its prior virtual state with no Page identity.

V0 has no soft-deleted, inactive, archived, or tombstoned Block state. A Block is either present in
canonical Notebook state or hard-deleted; only ephemeral Editing History can retain a restorable
copy. Because the project is still a prototype with no compatibility obligation, Block records
remove the existing constant `active = 1` field instead of reserving meaningless lifecycle state.

Ordinary creation always generates a fresh UUIDv4, even when recreating a deleted Block's former
kind, name, date, content, or location. Only undo or redo may reinstate an identity retained in
Editing History, ensuring Missing Block References cannot reconnect through identity reuse.
