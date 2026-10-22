# ProcessCommander

**[English](README.md) | [日本語 (Japanese)](README-ja.md)**

**ProcessCommander** is a native Win32 C++ Multi-Terminal ConPTY Process Manager and Dashboard designed for orchestrating, monitoring, and interacting with multiple parallel CLI agents (`claude`, `opencode`, build scripts, and interactive shells) via both **Model Context Protocol (MCP)** and **CLI commands**.

![ProcessCommander Dashboard](docs/images/dashboard.png)

---

## Key Features

### 1. Native Win32 + Direct2D Multi-Terminal Dashboard GUI
- **6 Top KPI Summary Cards**: Real-time metrics for Total Terminals, Active Running, Waiting/Idle (no output or CPU activity for `>= 10s`), Completed (`Exit 0`), Failed/Killed, and Total Job Object Memory & Process Count.
- **Sortable Process Summary Table (Newest on Top)**:
  - Newly launched processes are automatically added to the **top of the list** (`ID ▼` descending by default).
  - Click **any column header** (`ID`, `Agent / Name`, `Status`, `Start Time`, `End Time`, `Uptime`, `PID (Tree)`, `Memory`, `CPU Time`, `Idle`, `Latest Output Preview`, `Command Line`) to toggle **Ascending (`▲`) / Descending (`▼`)** sort order.
  - **CPU-Aware Idle Detection**: While a process's CPU time keeps increasing, it stays `RUNNING` instead of being shown as `IDLE / WAITING`, and it is not killed by `--idle-timeout`. CPU time includes all processes in the Win32 Job Object tree.
- **Tab-Switched Full-Size Direct2D Terminal (`>= 80x25`)**:
  - Embedded hardware-accelerated Direct2D / DirectWrite / IME / Sixel VT terminal emulator (`TerminalView` + `ConPtySession`).
  - Switch between active terminals using the **Terminal Tab Bar** while always preserving at least **80 columns × 25 rows** of terminal viewport so TUI apps (`claude`, `opencode`, `vim`, `htop`) never wrap or corrupt their layout.
  - Type directly into the terminal (full keyboard input, `Ctrl+C`, IME composition, mouse wheel scrollback, and text selection).
- **Up to 16 Parallel Terminals in Live Grid View (`[Grid View]`)**:
  - Click **`[Grid View]`** to open a live DWM thumbnail & snapshot grid (up to **16 parallel terminals** in a `4×4` matrix). Click any cell or use arrow keys + Enter to jump directly to that terminal tab.

![Terminal Grid View](docs/images/grid_view.png)

### 2. Multi-Shell Launcher & `[Config]` Dialog (`cmd` / `powershell` / `bash`)
- Click **`[+ Launch (...)]`** on the toolbar to immediately spawn a new interactive ConPTY terminal tab.
- Open **`[Settings]`** → **`[Shell Config...]`** to select the default shell (`cmd`, `powershell`, or `bash` — auto-detecting Git Bash / WSL Bash), customize startup command lines, or launch any shell on the spot. Settings are persisted in `logs/config.json`.

![Shell Config Dialog](docs/images/config_dialog.png)

### 3. History Management & Task Details Dialog
- **`[終了履歴を消去 (Clear Ended)]`**: Deletes all completed/killed/failed process entries from history and updates `logs/processes_state.json`.
- **`[終了履歴を非表示 (Hide Ended)]`**: Toggles hiding/showing finished processes so you can focus exclusively on currently running tasks without deleting history.
- **`[Reset Process IDs]`** (Settings dialog): Renumbers finished processes sequentially from `#1` and resets the next process ID. All running processes must be stopped first.
- **Right-Click Context Menu & Task Details Dialog**: Right-click any process row or terminal tab (or double-click a row) to inspect the full command line, working directory, PID/Job metrics, log file path, `.done` file path, and output preview, or copy the command line to the clipboard.

### 4. System Tray Integration
- Click **`[トレイに格納 (Tray)]`**, minimize the window, or press the window close button (`[X]`) to hide ProcessCommander into the **Windows System Tray** while keeping all background ConPTY processes and IPC/MCP services running.
- Left-click or double-click the tray icon (or right-click to open the tray menu) to restore and expand the dashboard at any time.

### 5. Persistent Logs, State Recovery & `.done` Completion Marker Files
- **Real-Time Per-Process Logs**: Every process's stdout/ConPTY stream is continuously written to `logs/proc_<id>_<name>_<YYYYMMDD_HHMMSS>.log` (or a custom `--log-file <path>`).
- **Completion Marker Files (`.done`) for AI Agents**: On process exit, ProcessCommander atomically writes a JSON completion file (`logs/proc_<id>.done` or custom `--done-file <path>`), allowing Claude or automation scripts to detect task completion via simple file-existence checks.
- **State Persistence Across Restarts**: Process metadata and history are saved in `logs/processes_state.json` and automatically restored when ProcessCommander starts.

### 6. Reliable Win32 Job Object Process Tree Control
- Each process runs inside a dedicated Win32 `Job Object` (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`), tracking all child/grandchild processes and memory usage and guaranteeing clean tree termination on `kill`, `--timeout`, or `--idle-timeout`.

### 7. Settings Dialog, Theme & Localization
- Click **`[Settings]`** to open the settings dialog.
- **Language**: switch between **日本語 / English / Español** buttons; the dashboard, dialogs, menus, and status bar update immediately.
- **Theme (colors)**: **Windows** (default; follows the Windows light/dark mode and personalization accent color), **Dark**, **Light**, or **High Contrast**. Buttons, list headers, scrollbars, and tab labels follow the dark theme.
- **`[Shell Config...]`** and **`[Reset Process IDs]`** are available here (previously toolbar buttons).
- Settings, language, theme, list/terminal visibility, and layout are **auto-saved** to `logs/config.json` and restored on the next start.

![Settings Dialog](docs/images/settings_dialog.png)

---

## Build Instructions

Requires Windows 10/11 (x64), Visual Studio 2022 (C++17), and CMake 3.20+:

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output binaries:
- `build/Release/ProcessCommander.exe` — GUI dashboard, Named Pipe IPC server, and MCP stdio server (alongside `conpty.dll` and `OpenConsole.exe`)
- `build/Release/pcder.exe` — Console CLI client

---

## Executables: `ProcessCommander.exe` vs. `pcder.exe`

| Executable | Role | Typical commands |
| :--- | :--- | :--- |
| `ProcessCommander.exe` | GUI dashboard, Named Pipe IPC server, and MCP stdio server. Owns all managed ConPTY processes. | `ProcessCommander.exe gui`<br>`ProcessCommander.exe mcp`<br>`ProcessCommander.exe help` |
| `pcder.exe` | Console client that sends requests to the running dashboard. No GUI of its own. | `pcder.exe list`<br>`pcder.exe start -- cmd.exe`<br>`pcder.exe help --json` |

- **`ProcessCommander.exe`** starts the dashboard with `gui` (the default if no command is given). If the dashboard is already running, `gui` restores it from the System Tray instead of starting a second instance. Use `mcp` to expose ProcessCommander as an MCP stdio server (it auto-starts the dashboard when needed).
- **`pcder.exe`** is the recommended console interface for humans, scripts, and LLM agents. It communicates with the dashboard over `\\.\pipe\ProcessCommander_IPC_v1`, auto-starts `ProcessCommander.exe` when the dashboard is not running, and locates the GUI binary in the same directory. Keep both `.exe` files together.
- **Legacy CLI compatibility**: `ProcessCommander.exe <command> [options]` forwards `start`, `list`, `logs`, `send`, `wait`, `kill`, `kill-all`, and `clear` to the same handler as `pcder.exe`. Prefer `pcder.exe` for new usage.
- **LLM discovery**: `pcder.exe help --json` returns the full command reference plus ready-to-use CLI and MCP setup instructions as JSON.

---

## Usage 1: Console CLI (`pcder.exe`)

The console client `pcder.exe` communicates with the running GUI Dashboard via Named Pipe (`\\.\pipe\ProcessCommander_IPC_v1`). If the dashboard is not yet running, `ProcessCommander.exe` is located in the same directory and launched automatically.

### JSON Output (`--json` / `-j`)

By default, `pcder.exe` prints human-friendly text. Add the global `--json` (or `-j`) flag — before or after the command — to receive the raw machine-readable JSON response instead. This is intended for scripts, CI pipelines, and LLM agents:

```powershell
# List all managed processes as JSON
pcder.exe list --json

# The flag can also precede the command
pcder.exe --json logs --id 1 --tail 80

# Short form
pcder.exe wait --id 1 --wait-ms 30000 -j
```

Every response is a single JSON object written to stdout. Successful responses contain `"ok": true` (for example, `list` returns an `ok` flag and a `processes` array); errors contain `"ok": false` plus an `error` message and set the process exit code to `1`. With `--json`, errors are also emitted as JSON on stdout:

```json
{"ok":false,"error":"Process ID not found."}
```

Example `list` response (fields abbreviated):

```json
{"ok":true,"processes":[{"id":1,"name":"agent-task","command":"opencode","mode":"pty","state":"running","pid":12345,"exit_code":0,"elapsed_ms":5230,"tree_procs":3,"memory_bytes":48234496,"log_file":"C:/logs/task42.log","done_file":"C:/logs/task42.done","last_line":"..."}]}
```

The examples below show the default human-friendly output:

```powershell
# Launch the GUI Dashboard (or restore from System Tray if already running)
ProcessCommander.exe gui

# Start a process with ConPTY enabled (use --pty; pipe mode is the default)
pcder.exe start --name "agent-task" --pty --cmd "opencode run \"Fix issue #42\"" --log-file "C:/logs/task42.log" --done-file "C:/logs/task42.done"

# List all managed processes (human-friendly table)
pcder.exe list

# Read the latest 80 lines of clean output from process #1
pcder.exe logs --id 1 --tail 80

# Send input to process #1's ConPTY / stdin
pcder.exe send --id 1 --input "y"

# Wait for process #1 to finish (up to 30 seconds)
pcder.exe wait --id 1 --wait-ms 30000

# Kill a process tree or all running processes
pcder.exe kill --id 1
pcder.exe kill-all

# Clear finished process history
pcder.exe clear
```

By default, `pcder.exe start` uses **pipe mode**, which launches the process directly without ConPTY. Add `--pty` (or `--mode pty`) to enable ConPTY for interactive terminals, or use `--mode console` to launch a separate visible console window. The MCP `start_process` tool follows the same rule: `mode` defaults to `pipe`, and `pty: true` enables ConPTY.

In pipe mode, stdin remains writable by default so `pcder.exe send` can feed input. Non-interactive programs such as `opencode run` may wait forever for stdin EOF; pass `--close-stdin` to close stdin immediately after launch so they can finish:

```powershell
# Keep stdin open for interactive input via pcder.exe send
pcder.exe start --name "pipe-shell" --cmd "cmd.exe"

# Non-interactive program that reads stdin: close it so EOF is delivered
pcder.exe start --name "oc-run" --close-stdin --cmd "opencode run OK" --timeout 90
```

For an interactive TUI such as `opencode` (without `run`), prefer `--pty`. The MCP equivalent of `--close-stdin` is `open_stdin: false`.

While a process is running, the **End Time** column shows an estimated finish time in gray. Pass `--eta "18:30"` (or `--estimate-end`) to set it explicitly; otherwise `--timeout` makes the column show remaining seconds (e.g. `残り 120 秒` / `120s left`).

If `--cwd` is omitted, `pcder.exe` uses the directory where it was invoked and resolves it to an absolute path. ProcessCommander sets the child's actual working directory and its `PWD` / `INIT_CWD` environment variables to that path (removing `OLDPWD`), so tools such as opencode that resolve their project directory from `PWD` operate on the intended folder.

**Completion notification:** ProcessCommander writes a `.done` JSON file atomically when a process exits. To block until completion, use `pcder.exe start --wait` (optionally with `--wait-ms` as a maximum), MCP `start_process` with `"wait": true`, or MCP `wait_process`. Without `wait`, `start` returns immediately and callers should poll the `.done` file or call `wait`.

**Claude Code background tasks:** launch a blocking command with `run_in_background`:

```powershell
pcder.exe start --name "task" --cwd C:\ws\Survivors --close-stdin --timeout 3600 --wait --wait-ms 0 --json --cmd C:\path\run_task.cmd
```

`pcder.exe` stays alive until the managed process exits, so Claude Code emits its `<task-notification>` when `pcder.exe` exits. The exit code is `0` on completed, the child's exit code on failure, and `124` if the wait timeout expires. The captured output file contains the final JSON with `state`, `exit_code`, `log_file`, and `done_file`.

`pcder.exe help` lists all commands and options. `pcder.exe help --json` returns the same reference plus CLI and MCP setup instructions as machine-readable JSON (including the resolved `ProcessCommander.exe` path and a ready-to-use MCP config), making it easy for LLM agents to learn how to configure the tool. The legacy `ProcessCommander.exe <command>` form remains available for compatibility, but `pcder.exe` is the recommended console interface.

---

## Usage 2: MCP Server for Claude Code / Claude Desktop

The MCP server is provided by **`ProcessCommander.exe mcp`** (not `pcder.exe`). Add it to your `.mcp.json` or `claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "process-commander": {
      "command": "C:/wsgit/ProcessCommander/build/Release/ProcessCommander.exe",
      "args": ["mcp"]
    }
  }
}
```

You can also run the server directly with `ProcessCommander.exe mcp`. It connects to the dashboard over the Named Pipe and automatically launches `ProcessCommander.exe gui` if the dashboard is not running.

### Provided MCP Tools
| Tool Name | Description |
| :--- | :--- |
| `start_process` | Start a process (`cmd`, `name`, `mode` [`pty`\|`pipe`\|`console`, default `pipe`], `pty`, `open_stdin`, `cwd`, `log_file`, `done_file`, `timeout_sec`, `idle_timeout_sec`, `estimate_end`, `wait`, `wait_ms`) |
| `list_processes` | List all managed processes with status, start time, uptime, PID tree count, memory, log/done paths, and latest output line (`id` optional) |
| `get_process_output` | Retrieve ANSI-stripped or raw terminal output for a process (`id`, `tail`, `wait_ms`) |
| `send_process_input` | Send text input or commands to a running process's ConPTY / stdin (`id`, `input`, `newline`, `wait_ms`) |
| `wait_process` | Wait for a process to exit (`id`, `wait_ms`) |
| `kill_process` | Terminate a specific process tree (`id`), or all running processes when `id` is `0` |
