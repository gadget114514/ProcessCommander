## 2024-05-18 - JSON Escaping bottleneck
**Learning:** In C++, `std::ostringstream` can be significantly slower than `std::string` concatenation with pre-allocation (`reserve`) for operations like character-by-character appending, especially in hot paths like `JsonEscape`.
**Action:** Replaced `std::ostringstream` with `std::string` appending in `JsonEscape` (src/common.cpp), improving performance by avoiding stream overhead.

## 2024-10-21 - [Extracting Text on Large Screen Grids]
**Learning:** Terminal emulation often maintains 2D grids (like TerminalBuffer) pre-filled with empty/space characters. Converting these raw cell grids into strings blindly iterates over all allocated cells regardless of actual content. In high-frequency calls like `ExtractLastNonEmptyLine` (which runs per-process, per-tick for dashboards), doing this string allocation causes noticeable CPU overhead.
**Action:** When working with 2D character grids, always implement a fast-path scan to check for non-empty content before allocating objects or strings for that row.

## 2024-11-20 - [Optimize TerminalView::ExtractLastNonEmptyLine String Allocation]
**Learning:** Terminal emulation often maintains 2D grids (like TerminalBuffer) pre-filled with empty/space characters. Converting these raw cell grids into strings blindly iterates over all allocated cells regardless of actual content. In high-frequency calls like `ExtractLastNonEmptyLine` (which runs per-process, per-tick for dashboards), allocating the string and then trimming it caused noticeable CPU overhead. The right way is using backward scanning (bottom-to-top, right-to-left) and in-place bound checking to avoid O(N) string allocations and redundant trimming.
**Action:** When extracting strings from TerminalBuffer 2D character grids, use backward scanning (right-to-left) and in-place bound checking to find exact string boundaries, then use `std::wstring::reserve` to allocate exactly the required length, avoiding unnecessary space trimming and allocations.

## 2024-12-04 - Optimize TaskQueueJob JSON Serialization
**Learning:** In hot paths generating large JSON objects like `TaskQueueManager::JobToJson`, relying on `std::ostringstream` causes significant heap allocation and virtual dispatch overhead. Pre-allocating `std::string` buffers and performing direct character/string appends reduces execution time significantly for large task objects.
**Action:** When serializing objects to JSON strings or building large text buffers in C++, bypass `std::ostringstream` in favor of `std::string::reserve` and direct `+=` string concatenations (coupled with `std::to_string` for primitives) to avoid temporary string allocations and stream processing overhead.
