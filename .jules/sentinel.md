## 2026-10-22 - [Path Traversal in OSC 1337 File Download]
**Vulnerability:** The OSC 1337 inline file download handler (`TerminalView::OnFileDownload`) appended an unsanitized filename provided by the untrusted terminal process/remote host directly to the user's Downloads directory path.
**Learning:** This existed because the application implicitly trusted the file name parameter from the terminal escape sequence without performing any sanitization or path extraction, which is a common pitfall when processing remote content.
**Prevention:** Always treat filenames from external or untrusted sources as potentially malicious. Before using them in filesystem operations, extract only the base name (strip directory components) and remove or replace any characters that are illegal or dangerous on the target file system.

## 2024-05-20 - [Unrestricted Remote Access in IPC Named Pipe]
**Vulnerability:** The IPC server named pipe (`\\\\.\\pipe\\ProcessCommanderIPC`) was created without `PIPE_REJECT_REMOTE_CLIENTS`, meaning any remote machine on the same network could potentially connect and issue IPC commands (like `start`, `kill`) if they had the right permissions.
**Learning:** For Windows named pipes (`CreateNamedPipeW`), `FILE_FLAG_FIRST_PIPE_INSTANCE` is often recommended to prevent pipe squatting, but it cannot be used inside an accept loop creating unlimited instances. In these scenarios, `PIPE_REJECT_REMOTE_CLIENTS` is crucial to secure local IPC against unauthorized remote connections.
**Prevention:** Always include `PIPE_REJECT_REMOTE_CLIENTS` in the `dwPipeMode` parameter when creating named pipes that are intended exclusively for local inter-process communication.
