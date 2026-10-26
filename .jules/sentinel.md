## 2026-10-22 - [Path Traversal in OSC 1337 File Download]
**Vulnerability:** The OSC 1337 inline file download handler (`TerminalView::OnFileDownload`) appended an unsanitized filename provided by the untrusted terminal process/remote host directly to the user's Downloads directory path.
**Learning:** This existed because the application implicitly trusted the file name parameter from the terminal escape sequence without performing any sanitization or path extraction, which is a common pitfall when processing remote content.
**Prevention:** Always treat filenames from external or untrusted sources as potentially malicious. Before using them in filesystem operations, extract only the base name (strip directory components) and remove or replace any characters that are illegal or dangerous on the target file system.

## 2026-10-26 - [Insecure Named Pipes]
**Vulnerability:** Named pipes created by the application were accessible to remote network clients, and PTY pipes were vulnerable to local pipe squatting attacks.
**Learning:** The default behavior of `CreateNamedPipeW` allows remote network connections unless explicitly restricted, and does not enforce exclusivity for single-instance pipes unless specified, which could allow malicious local processes to intercept data.
**Prevention:** Always use `PIPE_REJECT_REMOTE_CLIENTS` to prevent remote network access unless explicitly needed. Use `FILE_FLAG_FIRST_PIPE_INSTANCE` for single-instance pipes to prevent local pipe squatting.

## 2026-10-28 - [Arbitrary Code Execution via OSC 8 Hyperlinks]
**Vulnerability:** The terminal emulator allowed untrusted output to embed arbitrary URIs using the OSC 8 hyperlink escape sequence. When a user clicked on the link, the application invoked `ShellExecuteW` directly on the provided URL. If an attacker output a payload like `file:///c:/windows/system32/calc.exe` or `C:\malicious.bat`, the user clicking it would execute the local file.
**Learning:** `ShellExecuteW` uses the Windows shell to open files, URIs, and run executables indiscriminately. It relies on the caller to restrict what input is passed to it, which is dangerous when combining it with unvalidated URIs embedded by remote terminal output.
**Prevention:** Always validate and restrict the URI schemes passed to `ShellExecuteW`. Only permit explicit safe protocols, such as `http://`, `https://`, and `mailto:`, completely discarding `file://` or schemeless local path inputs when originating from untrusted sources.
