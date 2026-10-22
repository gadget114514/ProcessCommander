## 2024-05-18 - JSON Escaping bottleneck
**Learning:** In C++, `std::ostringstream` can be significantly slower than `std::string` concatenation with pre-allocation (`reserve`) for operations like character-by-character appending, especially in hot paths like `JsonEscape`.
**Action:** Replaced `std::ostringstream` with `std::string` appending in `JsonEscape` (src/common.cpp), improving performance by avoiding stream overhead.

## 2024-10-21 - [Extracting Text on Large Screen Grids]
**Learning:** Terminal emulation often maintains 2D grids (like TerminalBuffer) pre-filled with empty/space characters. Converting these raw cell grids into strings blindly iterates over all allocated cells regardless of actual content. In high-frequency calls like `ExtractLastNonEmptyLine` (which runs per-process, per-tick for dashboards), doing this string allocation causes noticeable CPU overhead.
**Action:** When working with 2D character grids, always implement a fast-path scan to check for non-empty content before allocating objects or strings for that row.

## 2024-10-22 - O(N^2) Trimming in std::vector
**Learning:** In C++, using `vector.erase(vector.begin())` repeatedly in a `while` loop (or implicitly tracking large changes one element at a time) is an O(N^2) operation because all subsequent elements are shifted each time. This creates a severe performance bottleneck when shrinking large history buffers (e.g., `setMaxHistoryLines`).
**Action:** Replaced element-by-element trimming with a block erase `history_.erase(history_.begin(), history_.begin() + overflow)`. This turns the O(N^2) operation into a much faster O(N) operation by shifting elements only once. When possible, always use iterator ranges to bulk-erase from a vector.
