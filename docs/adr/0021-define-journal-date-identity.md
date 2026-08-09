# Treat Journal Date as immutable calendar identity

Status: accepted

A Journal Date is a timezone-free proleptic Gregorian date from year 1 through 9999 and identifies
at most one materialized Journal Page in a Notebook. Journal kind and date are immutable for that
Page identity. “Today” is resolved from the operating system's current local date when navigation
or rollover occurs; timezone changes never rewrite existing Journal Pages.

A date with no durable content remains virtual and has no Block identity. Adding or moving durable
content into that date atomically materializes its Journal Page. Explicit Journal Page deletion is
absent under ADR 0019, while undo of the original materializing action may restore the virtual
state.
