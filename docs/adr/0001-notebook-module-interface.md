# Keep Notebook behavior behind one toolkit-neutral session

Status: accepted

`NotebookSession` is a thread-safe, synchronous, non-copyable module that owns zero or one open Notebook. Expected operational failures return typed `Result` values; invariant violations and unexpected faults may throw domain exceptions. Its interface uses owned standard C++ values, adds capability-specific methods only with the ticket that needs them, and will publish committed changes through RAII callback subscriptions invoked after storage transactions and internal locks are released. Qt adapters perform thread marshalling and error presentation; Qt and LMDB types never cross the interface. This gives callers one high-leverage seam without a global singleton, generic command envelope, or persistence-shaped surface.

The lifecycle state model was validated by prototype commit `44bb318391bdc5d0930114a55f2c4298ccdfd9b1` on branch `prototype/notebook-session-interface`: a second open is rejected without disturbing the first, close returns to the empty state, and a later open succeeds.
