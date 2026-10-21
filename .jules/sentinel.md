## 2024-05-30 - Command Injection in WMI Process Create
**Vulnerability:** Command Injection in PowerShell WMI script via unescaped path variable
**Learning:** `GetModuleFileNameW` returns a path that may contain single quotes (e.g. `C:\User's Files\exe.exe`). Passing this directly into a PowerShell script wrapped in single quotes allows command injection if an attacker crafts a path with single quotes.
**Prevention:** Sanitize the path by properly escaping single quotes for PowerShell (e.g., doubling them to `''`).
