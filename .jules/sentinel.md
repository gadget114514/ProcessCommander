## 2026-10-22 - [Path Traversal in OSC 1337 File Download]
**Vulnerability:** The OSC 1337 inline file download handler (`TerminalView::OnFileDownload`) appended an unsanitized filename provided by the untrusted terminal process/remote host directly to the user's Downloads directory path.
**Learning:** This existed because the application implicitly trusted the file name parameter from the terminal escape sequence without performing any sanitization or path extraction, which is a common pitfall when processing remote content.
**Prevention:** Always treat filenames from external or untrusted sources as potentially malicious. Before using them in filesystem operations, extract only the base name (strip directory components) and remove or replace any characters that are illegal or dangerous on the target file system.

## 2024-05-24 - Unrestricted Remote Access via Named Pipes
**Vulnerability:** IPC and ConPty named pipes allowed remote clients to connect and interact, which could lead to remote arbitrary process execution. Also, the PTY named pipes didn't prevent pipe squatting as they lacked `FILE_FLAG_FIRST_PIPE_INSTANCE`.
**Learning:** `PIPE_REJECT_REMOTE_CLIENTS` should be applied when creating named pipes to prevent remote connections. `FILE_FLAG_FIRST_PIPE_INSTANCE` prevents local attackers from squatting the pipe before the intended process creates it, but it cannot be used with pipes meant to accept multiple connections via a loop (like IPC).
**Prevention:** Always add `PIPE_REJECT_REMOTE_CLIENTS` to `CreateNamedPipeW` calls intended for local use. When creating a pipe for a single specific instance (like ConPty streams), also include `FILE_FLAG_FIRST_PIPE_INSTANCE`.
