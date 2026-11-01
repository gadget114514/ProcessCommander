## 2024-05-14 - Temporary String Allocations in Chained Operator+
**Learning:** When replacing `std::ostringstream` with a pre-allocated `std::string` to optimize serialization, chaining multiple `operator+` calls (e.g., `out += a + b + c`) undermines the optimization by creating temporary, dynamically allocated strings for intermediate results.
**Action:** Always use distinct, sequential `+=` statements or `.append()` calls when building up a string, avoiding chained `+` expressions.
