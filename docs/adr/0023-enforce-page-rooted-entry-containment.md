# Enforce Page-rooted Entry Containment

Status: accepted

Every materialized Page is a Containment root, and every Entry has exactly one parent that is a Page
or another Entry in the same Notebook. Parent chains are acyclic and terminate at exactly one Page;
Pages cannot contain Pages, siblings have one explicit total order, and committed state permits no
orphan, duplicate-parent, cross-Notebook, or cyclic relationship. Page Hierarchy remains a separate
name-derived organization under ADR 0017.

An Entry subtree may move between any Named or Journal Pages without changing Block type, identity,
content, descendants, or creation times. Each move atomically applies detachment, attachment,
ordering, Block Update Times, and indexes; moving into a virtual Journal Date also materializes its
Journal Page in that transaction. Invalid or failed moves change no acknowledged state or Notebook
revision.
