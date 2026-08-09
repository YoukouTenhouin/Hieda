# Keep Authored Text canonical under permissive notation

Status: accepted

Journal Entries and Page Entries share one lightweight Authored Text Notation. Authored Text remains
the exact canonical source: notation is parsed only after a durable edit commits, and malformed or
incomplete notation remains ordinary text rather than preventing capture. This keeps transient
typing safe while allowing committed text to maintain Properties and Semantic References
transactionally.

## Source contract

Authored Text is valid UTF-8 containing at most 1 MiB. LF is its only permitted control character;
CR, Tab, NUL, DEL, and the remaining C0 and C1 controls are invalid at the Notebook boundary. The Qt
adapter normalizes pasted CRLF and CR to LF, expands Tabs to the next two-column stop measured in
Unicode scalar values from the surrounding logical-line start, and silently drops other forbidden
controls before submission.

## Grammar

- `[[page_name]]` is a Page Link candidate. Its body uses the existing exact Page-name grammar
  `[a-z][a-z0-9_-]{0,63}` rather than a duplicate-capable display title.
- `[[block:550e8400-e29b-41d4-a716-446655440000]]` is a Block Reference candidate. Only the
  canonical lowercase, hyphenated UUIDv4 spelling is recognized.
- `key::value` is a Property only at the beginning of a logical line. The key uses the Page-name
  grammar; every code point after `::` is the exact string value, including whitespace, and the
  empty value is valid. Repeated keys and equal values retain their authored order and duplicates.
- A valid Property consumes its line and its value is opaque to link parsing. Prefixing a valid
  Property with `\` suppresses only the Property meaning; Page Links and Block References in its
  former value remain active.
- A backslash escapes one notation construct and is omitted from parsed presentation while
  remaining in canonical Authored Text. Consecutive backslashes use odd/even parity, and an escaped
  `[[` opener takes effect even when its candidate is incomplete or invalid.

All constructs are line-local. The leftmost unescaped `[[` pairs atomically with the next `]]` on
that line; an invalid body makes that candidate plain text and prevents nested-looking content
inside it from acquiring meaning. Notation parsing is total and emits no diagnostics: only source
encoding, character, and size validation can reject Authored Text.

## Derived meaning

The Notebook owns a pure syntax parser and does not expose it as a public draft-parsing API. A
successful commit reparses the complete Entry and atomically replaces its text, derived Properties,
forward and reverse relationship indexes, timestamp, revision, and undo state. Pending drafts keep
the last committed derived state; a failed commit changes no durable state.

Parser results retain source-ordered typed occurrences with UTF-8 byte ranges. Equal link
occurrences collapse to one Semantic Reference for each source Block, target Block, and reference
kind, while Properties retain their ordered duplicate values. Syntax recognition does not consult
the Notebook: missing targets, Page renames, dangling relationships, and presentation labels remain
link-resolution decisions. Search and Query consumers may interpret exact Property strings without
changing their canonical values.

The grammar and state transitions were validated by the in-memory terminal prototype at commit
`25a1de7` on branch `prototype/authored-text-notation`. The prototype remains off `master`; this ADR
records its accepted result without adding production parsing behavior.
