# Support multiline desktop Journal input

Status: accepted

Ticket 05 expands Journal Entry authored text from one line to exact Unicode text containing LF line
breaks. Carriage returns and malformed UTF-8 remain invalid. Shift+Enter inserts a line break in the
current Entry, while plain Enter retains its structural meaning and splits the Entry at the caret.
Multiline paste remains one Entry. This supersedes the single-line restriction in ADR 0007 without
changing the Notebook schema because authored text already uses a length-delimited UTF-8 encoding.

Bare Up and Down move through an Entry's visual lines and cross to the adjacent visible Entry only
at the first or last visual line. Native text selection and clipboard commands remain local to one
editor. Persisted bullets additionally support whole-subtree selection: click selects one subtree,
Shift-click or Shift+Up/Down extends the range, Copy emits a human-readable indented bullet list,
and Cut removes all selected subtrees in one durable undoable Notebook transaction. Pasting that
plain-text representation does not reconstruct outline structure.

IME preedit remains transient Qt state. It cannot trigger a Notebook command or typing commit;
committed composition enters the normal typing group, and cancelled composition changes no
canonical text. Qt accessibility exposes the Journal list, selectable outline bullets, multiline
editable text, focus, selection state, hierarchy descriptions, and standard actions.
