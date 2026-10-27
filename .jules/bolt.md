## 2026-10-27 - [std::ostringstream overhead in JobToJson]
**Learning:** `std::ostringstream` brings significant overhead in hot paths compared to pre-allocated `std::string` concatenation. While this was already learned for `JsonEscape`, it was also causing overhead in complex serialization routines like `JobToJson`.
**Action:** Always prefer `std::string` with `reserve()` over `std::ostringstream` for JSON construction logic.
