## 2026-10-22 - [Path Traversal in OSC 1337 File Download]
**Vulnerability:** The OSC 1337 inline file download handler (`TerminalView::OnFileDownload`) appended an unsanitized filename provided by the untrusted terminal process/remote host directly to the user's Downloads directory path.
**Learning:** This existed because the application implicitly trusted the file name parameter from the terminal escape sequence without performing any sanitization or path extraction, which is a common pitfall when processing remote content.
**Prevention:** Always treat filenames from external or untrusted sources as potentially malicious. Before using them in filesystem operations, extract only the base name (strip directory components) and remove or replace any characters that are illegal or dangerous on the target file system.

## 2026-10-26 - [Insecure Named Pipes]
**Vulnerability:** Named pipes created by the application were accessible to remote network clients, and PTY pipes were vulnerable to local pipe squatting attacks.
**Learning:** The default behavior of `CreateNamedPipeW` allows remote network connections unless explicitly restricted, and does not enforce exclusivity for single-instance pipes unless specified, which could allow malicious local processes to intercept data.
**Prevention:** Always use `PIPE_REJECT_REMOTE_CLIENTS` to prevent remote network access unless explicitly needed. Use `FILE_FLAG_FIRST_PIPE_INSTANCE` for single-instance pipes to prevent local pipe squatting.
## 2024-10-27 - Arbitrary Code Execution in Hyperlinks
**Vulnerability:** `ShellExecuteW` was used to open URIs from OSC 8 hyperlinks without any validation of the URI scheme, leading to potential Arbitrary Code Execution.
**Learning:** Untrusted URIs executed via `ShellExecuteW` must have their scheme validated against an explicitly safe allowlist (like `http://`, `https://`, `mailto:`, `file://`) before any execution or custom command substitution logic.
**Prevention:** Always validate the URI scheme before executing untrusted inputs using `ShellExecuteW`.
