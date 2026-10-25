## 2026-10-22 - [Path Traversal in OSC 1337 File Download]
**Vulnerability:** The OSC 1337 inline file download handler (`TerminalView::OnFileDownload`) appended an unsanitized filename provided by the untrusted terminal process/remote host directly to the user's Downloads directory path.
**Learning:** This existed because the application implicitly trusted the file name parameter from the terminal escape sequence without performing any sanitization or path extraction, which is a common pitfall when processing remote content.
**Prevention:** Always treat filenames from external or untrusted sources as potentially malicious. Before using them in filesystem operations, extract only the base name (strip directory components) and remove or replace any characters that are illegal or dangerous on the target file system.

## 2026-10-25 - [Insecure Named Pipe Creation]
**Vulnerability:** The named pipes for ConPTY communication were created without `FILE_FLAG_FIRST_PIPE_INSTANCE` and `PIPE_REJECT_REMOTE_CLIENTS`.
**Learning:** Without `FILE_FLAG_FIRST_PIPE_INSTANCE`, an attacker could potentially squat the pipe name (if they win a race) causing the app to connect to their malicious pipe instead. Without `PIPE_REJECT_REMOTE_CLIENTS`, named pipes are potentially accessible over the network by default, which is unnecessary and insecure for local IPC.
**Prevention:** Always use `FILE_FLAG_FIRST_PIPE_INSTANCE` when creating the first (or only) instance of a named pipe to ensure you actually own it. Always use `PIPE_REJECT_REMOTE_CLIENTS` for local-only pipes to explicitly prevent remote connections.
