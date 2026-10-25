## 2026-10-22 - [Path Traversal in OSC 1337 File Download]
**Vulnerability:** The OSC 1337 inline file download handler (`TerminalView::OnFileDownload`) appended an unsanitized filename provided by the untrusted terminal process/remote host directly to the user's Downloads directory path.
**Learning:** This existed because the application implicitly trusted the file name parameter from the terminal escape sequence without performing any sanitization or path extraction, which is a common pitfall when processing remote content.
**Prevention:** Always treat filenames from external or untrusted sources as potentially malicious. Before using them in filesystem operations, extract only the base name (strip directory components) and remove or replace any characters that are illegal or dangerous on the target file system.

## 2026-10-25 - Local Pipe Squatting & Unauthorized Remote Access in Windows Named Pipes
**Vulnerability:** Windows named pipes (`CreateNamedPipeW`) didn't enforce local-only connections or prevent pipe squatting by local unprivileged users.
**Learning:** By default, named pipes can accept connections from remote clients over the network. Furthermore, without `FILE_FLAG_FIRST_PIPE_INSTANCE`, an unprivileged user could create the pipe before the application starts, intercepting all local IPC traffic.
**Prevention:** Always use `FILE_FLAG_FIRST_PIPE_INSTANCE` when creating pipes meant to have a single instance (to avoid local pipe squatting) and `PIPE_REJECT_REMOTE_CLIENTS` to reject unauthorized remote access over the network.
