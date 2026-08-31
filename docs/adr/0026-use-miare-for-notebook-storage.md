# Use Miare for Notebook storage

Status: accepted

## Context

Hieda's LMDB backend was an early prototype. It imposed a system LMDB dependency, exposed
backend-specific lifecycle assumptions inside the Notebook implementation, and maintained an
unused reverse Property index whose keys could contain complete authored values. Miare is now
ready for application testing and provides the ordered transactions, durable single-file storage,
exclusive-open enforcement, and explicit close result that Hieda needs.

## Decision

The canonical `.hieda` file is an unencrypted, Zstandard-compressed Miare database. Miare is an
MPL-2.0 source dependency pinned as a Git submodule; Zstandard remains the platform dependency.
Hieda's private storage adapter assigns stable prefixes to its logical keyspaces while the public
`NotebookSession` interface remains toolkit- and storage-neutral. Canonical records and derived
indexes continue to commit together. The used `properties_by_block` index remains, while the
unused value-bearing reverse Property index is removed.

Creation still initializes a temporary sibling and publishes it without overwrite. Miare owns the
data-file lock, while Hieda retains its adjacent open lock for publication coordination. Explicit
close returns a typed result; close failure retains the live session and prevents application exit
so callers can retry. Destruction remains best effort.

This is a clean format break. Prototype LMDB files are invalid input and receive no migration,
detection, or dual-backend path.

## Consequences

Notebook behavior remains tested through the public module using real temporary Miare files.
Write scans are materialized before mutation because Miare invalidates write cursors after key
changes. Miare's license is shipped with Hieda packages, and CI must initialize submodules and
provide Zstandard. The private adapter, logical prefixes, compression profile, and typed close
behavior are now compatibility commitments that future storage changes must address explicitly.
