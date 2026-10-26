## 2024-05-18 - JSON Escaping bottleneck
**Learning:** In C++, `std::ostringstream` can be significantly slower than `std::string` concatenation with pre-allocation (`reserve`) for operations like character-by-character appending, especially in hot paths like `JsonEscape`.
**Action:** Replaced `std::ostringstream` with `std::string` appending in `JsonEscape` (src/common.cpp), improving performance by avoiding stream overhead.

## 2024-10-21 - [Extracting Text on Large Screen Grids]
**Learning:** Terminal emulation often maintains 2D grids (like TerminalBuffer) pre-filled with empty/space characters. Converting these raw cell grids into strings blindly iterates over all allocated cells regardless of actual content. In high-frequency calls like `ExtractLastNonEmptyLine` (which runs per-process, per-tick for dashboards), doing this string allocation causes noticeable CPU overhead.
**Action:** When working with 2D character grids, always implement a fast-path scan to check for non-empty content before allocating objects or strings for that row.

## 2024-11-20 - ExtractLastNonEmptyLine String Allocation Bottleneck
**Learning:** `ExtractLastNonEmptyLine` in TerminalView was allocating a string for the entire row of terminal cells, including all trailing spaces, only to immediately truncate them using `pop_back()` loops. In high-frequency UI updates this creates unnecessary CPU and memory allocator thrashing.
**Action:** Replaced the blind `hasContent` scan and post-trimming allocation with a two-pointer bounds scan (right-to-left for end, left-to-right for start), ensuring only the exact required string capacity is reserved and allocated once.
