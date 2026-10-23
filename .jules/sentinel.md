## 2026-10-22 - [Path Traversal in OSC 1337 File Download]
**Vulnerability:** The OSC 1337 inline file download handler (`TerminalView::OnFileDownload`) appended an unsanitized filename provided by the untrusted terminal process/remote host directly to the user's Downloads directory path.
**Learning:** This existed because the application implicitly trusted the file name parameter from the terminal escape sequence without performing any sanitization or path extraction, which is a common pitfall when processing remote content.
**Prevention:** Always treat filenames from external or untrusted sources as potentially malicious. Before using them in filesystem operations, extract only the base name (strip directory components) and remove or replace any characters that are illegal or dangerous on the target file system.

## 2026-10-23 - [Insecure Named Pipe Creation allows Remote Access]
**Vulnerability:** The IPC server created a named pipe (`CreateNamedPipeW`) without `PIPE_REJECT_REMOTE_CLIENTS`. Since `PIPE_UNLIMITED_INSTANCES` is used, and default DACLs might be permissive depending on context, remote machines could connect to the IPC pipe and send commands (like starting or killing processes) remotely.
**Learning:** This existed because `PIPE_REJECT_REMOTE_CLIENTS` was omitted from the pipe mode parameter. Windows named pipes by default can accept connections over the network (SMB) if the ACL permits, which is rarely intended for local-only IPC.
**Prevention:** Always add `PIPE_REJECT_REMOTE_CLIENTS` to `dwPipeMode` when creating named pipes intended exclusively for local IPC.
