## 2026-10-22 - [Path Traversal in OSC 1337 File Download]
**Vulnerability:** The OSC 1337 inline file download handler (`TerminalView::OnFileDownload`) appended an unsanitized filename provided by the untrusted terminal process/remote host directly to the user's Downloads directory path.
**Learning:** This existed because the application implicitly trusted the file name parameter from the terminal escape sequence without performing any sanitization or path extraction, which is a common pitfall when processing remote content.
**Prevention:** Always treat filenames from external or untrusted sources as potentially malicious. Before using them in filesystem operations, extract only the base name (strip directory components) and remove or replace any characters that are illegal or dangerous on the target file system.

## 2024-10-23 - Prevent Named Pipe Squatting
**Vulnerability:** IPC pipes created via `CreateNamedPipeW` without `FILE_FLAG_FIRST_PIPE_INSTANCE` are vulnerable to pipe squatting, where a malicious local process pre-creates the pipe to intercept or manipulate data.
**Learning:** Windows allows multiple processes to create pipes with the same name. Without `FILE_FLAG_FIRST_PIPE_INSTANCE`, our application might unknowingly connect to a pipe owned by an attacker instead of creating its own. Adding `PIPE_REJECT_REMOTE_CLIENTS` provides defense-in-depth against remote access.
**Prevention:** Always use `FILE_FLAG_FIRST_PIPE_INSTANCE` in the `dwOpenMode` argument and `PIPE_REJECT_REMOTE_CLIENTS` in the `dwPipeMode` argument when creating named pipes with `CreateNamedPipeW`.
