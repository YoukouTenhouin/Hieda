# 05 — Support production desktop text input

**Parent spec:** [Notebook v0](../../notebook-v0/spec.md)

**What to build:** Make the Qt Quick journal editor dependable for normal desktop use, including focus, selection, clipboard, IME composition, pointer use, and accessibility. UI-specific transient state must remain in the Qt Quick adapter without changing Notebook semantics.

**Blocked by:** 03 — Edit nested journal outlines; [Prototype the Qt Quick journal experience](../../notebook-v0/issues/12-prototype-the-qt-quick-journal-experience.md)

**Status:** ready-for-agent

- [ ] Focus and cursor position behave predictably while creating, selecting, moving, and navigating Journal Entries.
- [ ] Text selection and clipboard cut, copy, and paste preserve Unicode text and agreed outline behavior.
- [ ] IME composition can begin, update, commit, and cancel without premature canonical edits or lost text.
- [ ] Essential editing, selection, and navigation operations are usable with a pointer.
- [ ] Journal structure, editable text, focus, and available actions are exposed through the selected accessibility facilities.
- [ ] Qt-focused tests cover keyboard routing, focus transitions, selection, composition, clipboard, pointer interaction, and accessibility output.

