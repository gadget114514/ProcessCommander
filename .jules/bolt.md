## 2024-05-18 - JSON Escaping bottleneck
**Learning:** In C++, `std::ostringstream` can be significantly slower than `std::string` concatenation with pre-allocation (`reserve`) for operations like character-by-character appending, especially in hot paths like `JsonEscape`.
**Action:** Replaced `std::ostringstream` with `std::string` appending in `JsonEscape` (src/common.cpp), improving performance by avoiding stream overhead.

## 2024-10-21 - [Extracting Text on Large Screen Grids]
**Learning:** Terminal emulation often maintains 2D grids (like TerminalBuffer) pre-filled with empty/space characters. Converting these raw cell grids into strings blindly iterates over all allocated cells regardless of actual content. In high-frequency calls like `ExtractLastNonEmptyLine` (which runs per-process, per-tick for dashboards), doing this string allocation causes noticeable CPU overhead.
**Action:** When working with 2D character grids, always implement a fast-path scan to check for non-empty content before allocating objects or strings for that row.

## 2024-11-20 - [Optimize TerminalView::ExtractLastNonEmptyLine String Allocation]
**Learning:** Terminal emulation often maintains 2D grids (like TerminalBuffer) pre-filled with empty/space characters. Converting these raw cell grids into strings blindly iterates over all allocated cells regardless of actual content. In high-frequency calls like `ExtractLastNonEmptyLine` (which runs per-process, per-tick for dashboards), allocating the string and then trimming it caused noticeable CPU overhead. The right way is using backward scanning (bottom-to-top, right-to-left) and in-place bound checking to avoid O(N) string allocations and redundant trimming.
**Action:** When extracting strings from TerminalBuffer 2D character grids, use backward scanning (right-to-left) and in-place bound checking to find exact string boundaries, then use `std::wstring::reserve` to allocate exactly the required length, avoiding unnecessary space trimming and allocations.

## 2024-11-20 - [Avoid std::ostringstream in TaskQueueManager::JobToJson]
**Learning:** `std::ostringstream` is notably slower than `std::string` concatenation with `reserve` due to locale handling, virtual function calls, and multiple internal allocations. The `JobToJson` serialization logic creates significant overhead for large task queues as this method runs frequently per-job.
**Action:** When manually generating JSON strings for large data models like `TaskQueueJob`, use pre-allocated `std::string` objects (via `reserve`) and sequential `+=` or `.append()` operations instead of `std::ostringstream` to minimize stream overhead and heap allocations.
