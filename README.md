# ProcessCommander

**[English](README.md) | [日本語 (Japanese)](README-ja.md)**

**ProcessCommander** is a native Win32 C++ Multi-Terminal ConPTY Process Manager and Dashboard designed for orchestrating, monitoring, and interacting with multiple parallel CLI agents (`claude`, `opencode`, build scripts, and interactive shells) via both **Model Context Protocol (MCP)** and **CLI commands**.

![ProcessCommander Dashboard](docs/images/dashboard.jpg)

---

## Quick Start for LLM Agents

```powershell
pcder.exe mcp install --client opencode   # or --client claude (writes .mcp.json)
pcder.exe help --json                     # machine-readable guide: see "agent.quick_start"
```

Rules of thumb (the same rules are encoded in `help --json`):

- **One command per job.** For multiple steps or dependencies use `taskqueue_add` with `dependsOn`; shell chains (`&&`, `||`, `&`, PowerShell `;`) and polling loops are rejected.
- **Timeouts are per job** (`timeoutSec`), never summed. Every job also needs a one-line `summary`.
- **Success is verified** by the exit code plus optional `filesNewer` / `outputRegex`, not merely "the process exited".
- **Watch the states**: `waiting` = queued and ready (waiting for a slot), `pending` = not queued yet (dependencies not satisfied), `blocked` = a dependency failed. Use `taskqueue_wait` / `taskqueue_graph`, and `taskqueue_retry` after fixing the cause.

---

## Project-Local Data Directory (git-managed)

Logs, process state, task queues and GUI config live in one **data directory**. Select it per invocation with `--data-dir <path>`:

```powershell
pcder.exe --data-dir .processcommander taskqueue add --file jobs.json
pcder.exe taskqueue list main --data-dir .processcommander
pcder.exe mcp install --data-dir .processcommander   # embed it in the project's MCP config
```

How consistency is guaranteed:

1. Pick one project-local directory, e.g. `.processcommander/`.
2. `pcder.exe mcp install --data-dir .processcommander` writes `--data-dir .processcommander` into `.mcp.json` / `opencode.json`, so every MCP tool call for that project uses the same directory.
3. Pass the same `--data-dir` on every direct `pcder.exe` call. It is **per invocation**, not a global setting.
4. Relative paths resolve against the caller's current directory; use an absolute path in the MCP config if the client's cwd is uncertain.
5. Different data directories are fully isolated: each gets its own dashboard instance, Named Pipe and mutex, so multiple projects can run at once without sharing processes or queues.
6. Verify with `pcder.exe --data-dir <path> help --json` (`effective_data_dir`) or the dashboard status bar, which shows the active data directory.

The environment variable `PROCESS_COMMANDER_LOG_DIR` is an equivalent fallback.

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

### 8. Task Queue: Ordering, Dependencies, Timeouts and Automatic Retries
- Register jobs with **one command each**; ProcessCommander decides when to run them (concurrency, dependencies, priority) and persists the queue across restarts (`logs/taskqueues/<name>.json`).
- Per-job `timeoutSec`, idle detection (output **and** CPU), success checks (`exitCode`, `filesNewer`, `outputRegex`), automatic retries (`maxAttempts`), and `blocked` dependents when a dependency fails.
- **ChainGuard** rejects shell chains and wait loops in `start_process` / `taskqueue_add`; use `dependsOn` instead. CLI `--allow-chain` is the only escape hatch (never exposed over MCP).
- GUI: click **"Show Task Queue"** (leftmost toolbar button) or press **Ctrl+T**. Running jobs are listed first and highlighted; failed rows are muted red and timed-out rows muted amber (theme-based, low saturation).
- See [Task Queue](#task-queue-ordered-jobs-dependencies-and-retries) for the data model and examples.

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

**Task queue (ordered/dependent jobs):**

```powershell
# jobs.json: one command per job, each with its own timeout
# [{"id":"A","command":"run A","summary":"Prepare","timeoutSec":1800},
#  {"id":"B","command":"run B","summary":"Build","timeoutSec":3600,"dependsOn":["A"]}]
pcder.exe taskqueue add --file jobs.json --taskqueue main --concurrency 1
pcder.exe taskqueue list main
pcder.exe taskqueue get A
pcder.exe taskqueue graph main
pcder.exe taskqueue wait main --since 0 --timeout-ms 30000
pcder.exe taskqueue retry A
```

**Register as an MCP server automatically (recommended for LLM agents):**

```powershell
pcder.exe mcp install                     # .mcp.json (Claude Code)
pcder.exe mcp install --client opencode   # opencode.json
pcder.exe mcp install --dry-run --json    # preview without writing
```

`pcder.exe help` lists all commands and options. `pcder.exe help --json` returns the same reference plus CLI and MCP setup instructions as machine-readable JSON (including the resolved `ProcessCommander.exe` path and a ready-to-use MCP config), making it easy for LLM agents to learn how to configure the tool. The legacy `ProcessCommander.exe <command>` form remains available for compatibility, but `pcder.exe` is the recommended console interface.

---


## Task Queue: Ordered Jobs, Dependencies and Retries

ProcessCommander can own the workflow: you declare jobs and dependencies; it decides what runs when, enforces per-job timeouts, verifies success, and retries automatically.

**One job = one command.** Do not write `A && B && C`. Register three jobs and declare dependencies:

```json
{ "taskqueue": "main", "concurrency": 1,
  "jobs": [
    { "id": "A", "command": "run A", "summary": "Prepare inputs", "timeoutSec": 1800 },
    { "id": "B", "command": "run B", "summary": "Build outputs", "timeoutSec": 3600, "dependsOn": ["A"] },
    { "id": "C", "command": "run C", "summary": "Publish", "timeoutSec": 900,
      "dependsOn": [{ "id": "B", "on": "success" }] }
  ] }
```

```powershell
pcder.exe taskqueue add --file jobs.json --taskqueue main
pcder.exe taskqueue list main
pcder.exe taskqueue graph main
pcder.exe taskqueue wait main --since 0 --timeout-ms 30000
```

**Job fields**

| Field | Meaning |
| :--- | :--- |
| `id`, `command` | Job id (unique across all queues) and exactly one command. |
| `summary` (required) | One line, <=200 chars. Substituted for `{summary}` and exported as `PC_JOB_SUMMARY`. |
| `timeoutSec` (required) | Per-job limit. Never summed. Must be <= the queue `maxTimeoutSec` (default 7200). |
| `dependsOn` | `["A"]` (same as `on: success`) or `[{ "id": "A", "on": "done" }]`. Cross-queue ids are allowed. |
| `idle` | `{ "outputSec": 720, "cpuSec": 720 }`; both must be set (output **and** CPU idle). |
| `maxAttempts`, `retryDelaySec` | Automatic retries (default 2) and delay between attempts. |
| `success` | `exitCode` (default 0) AND `filesNewer` (exist and newer than the attempt start) AND `outputRegex` (last 200 log lines). |
| `cwd`, `env`, `killExclude`, `priority` | Working directory, extra environment, image names excluded from tree kill, scheduling priority. |
| `instructionsFile` / `instructionsText` | Optional instructions captured into the job and materialized per revision. |

**States**

| State | Meaning |
| :--- | :--- |
| `waiting` | Queued and ready; waiting for a free slot (or the retry delay). |
| `pending` | Not in the run queue yet; dependencies are not satisfied. |
| `blocked` | A success-dependency failed/cancelled. Fix it and `taskqueue_retry` the dependency; blocked dependents are requeued automatically. |
| `running` | An attempt (one managed process) is executing. |
| `succeeded` / `failed` / `cancelled` | Terminal states. Failed jobs auto-retry up to `maxAttempts`. |

**ChainGuard:** `start_process`, `taskqueue_add` and `taskqueue_update` reject `&&`, `||`, a single `&`, PowerShell `;`, newline chains, and polling loops (`while` + `Test-Path`, `goto` loops). Splitting commands into jobs is the intended fix. CLI `--allow-chain` bypasses the guard for a single process; MCP never exposes it.

**Events and environment:** each attempt receives `PC_JOB_ID`, `PC_ATTEMPT`, `PC_JOB_SUMMARY`, `PC_JOB_INSTRUCTIONS`, `PC_JOB_INSTRUCTIONS_REV`, plus `PC_PREV_REASON`, `PC_PREV_EXIT`, `PC_PREV_TAIL` on retries. Events are appended to `logs/taskqueue-<name>.events.jsonl`; `notifyFile` is updated for `jobFailed`, `taskqueueStalled` and `taskqueueDrained`.

**GUI:** click **Show Task Queue** (leftmost toolbar button) or press **Ctrl+T**. Running jobs are listed first and highlighted; failed rows use a muted red and timed-out rows a muted amber (theme colors).

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

Or let `pcder.exe` write the config for you (merges with existing servers, idempotent):

```powershell
pcder.exe mcp install                     # .mcp.json (Claude Code)
pcder.exe mcp install --client opencode   # opencode.json
pcder.exe mcp install --dry-run --json    # preview
```

You can also run the server directly with `ProcessCommander.exe mcp`. It connects to the dashboard over the Named Pipe and automatically launches `ProcessCommander.exe gui` if the dashboard is not running.

### Provided MCP Tools

**Process tools**

| Tool | Description |
| :--- | :--- |
| `start_process` | Start one command (`cmd`, `name`, `mode` [`pty`\|`pipe`\|`console`], `pty`, `open_stdin`, `cwd`, `log_file`, `done_file`, `timeout_sec`, `idle_timeout_sec`, `estimate_end`, `wait`, `wait_ms`). Chained commands are rejected. |
| `list_processes` | List managed processes (`id` optional). |
| `get_process_output` | Read output (`id`, `tail`, `wait_ms`). |
| `send_process_input` | Send input to stdin/ConPTY (`id`, `input`, `newline`, `wait_ms`). |
| `wait_process` | Wait for exit (`id`, `wait_ms`). |
| `kill_process` | Kill a process tree (`id`; `0` = all). A killed task-queue attempt is retried by the scheduler. |

**Task queue tools**

| Tool | Description |
| :--- | :--- |
| `taskqueue_add` | Register jobs (`taskqueue`, `jobs[]`, `concurrency?`, `notifyFile?`). Each job needs `id`, `command`, `summary`, `timeoutSec`. |
| `taskqueue_list` | List jobs (`taskqueue?`, `state?`). |
| `taskqueue_get` | Full job, attempts and instructions (`id`, `includeInstructions?`, `rev?`). |
| `taskqueue_retry` | Retry failed/blocked/cancelled (`id`, `resetAttempts?`, `refreshInstructions?`); blocked dependents are requeued. |
| `taskqueue_update` | Update a pending/failed/blocked job. |
| `taskqueue_cancel` | Cancel a job (kills a running attempt, no retry). |
| `taskqueue_pause` / `taskqueue_resume` | Stop/restart starting new jobs in a queue. |
| `taskqueue_wait` | Long-poll for a queue version change (`taskqueue`, `sinceVersion`, `timeoutMs`). |
| `taskqueue_purge` | Remove finished jobs (`taskqueue`, `states[]`). |
| `taskqueue_link` / `taskqueue_unlink` | Add/remove one dependency (`job`, `dependsOn`, `on` = `success`\|`done`). |
| `taskqueue_graph` | Dependency graph with `ready` / `waiting_for` / `dependents`. |

---

## Appendix: `ProcessCommander.exe --help`

```text
ProcessCommander - Native Win32 Multi-LLM Process & ConPTY Manager + Live GUI Dashboard

USAGE:
  ProcessCommander.exe [gui] [--data-dir <path>]
      Launch the live Win32 Dashboard GUI and Named Pipe server.

  ProcessCommander.exe mcp [--data-dir <path>]
      Run as a Model Context Protocol (MCP) stdio server for Claude.
      Automatically connects to (or launches) the Win32 GUI Dashboard.

  ProcessCommander.exe --help
      Show this help.

OPTIONS (per launch):
  --data-dir <path>   Store logs, state, task queues and config in <path>.
                      Use a project-local directory to git-manage them. Each
                      data dir gets its own dashboard instance (separate pipe).
                      Equivalent environment variable: PROCESS_COMMANDER_LOG_DIR.
                      Consistency: pass the same --data-dir on every call, or embed
                      it once with `pcder.exe mcp install --data-dir <path>`.
                      Different data dirs are isolated dashboard instances.

WORKING MODEL (LLM orchestration):
  ProcessCommander is the execution layer for a supervising LLM.

    Supervisor LLM (Claude Code, opencode, ...)
      |  start_process   -> one long-running worker command
      |  taskqueue_add   -> ordered/dependent worker jobs
      v
    ProcessCommander
      |  owns processes, timeouts, logs, retries, state
      v
    Worker LLM / build command
      -> one job command; writes artifacts and logs

  Assumed structure:
    1. The supervisor plans and declares jobs. It does not run the work itself,
       does not chain commands, and does not fabricate success.
    2. Each unit of work is one job with a one-line summary, its own timeoutSec,
       and an optional success check (filesNewer / outputRegex). Steps or
       dependencies become separate jobs linked with dependsOn.
    3. Workers are ordinary processes and may themselves be LLM CLIs. Pass
       detailed instructions via instructionsFile/instructionsText; workers read
       {instructions} / PC_JOB_INSTRUCTIONS and write artifacts.
    4. The supervisor observes with taskqueue_wait / taskqueue_list /
       taskqueue_graph and reads logs; it never polls .done files.
    5. On failure, fix the cause and call taskqueue_retry. Blocked dependents
       resume automatically.
    6. Keep the supervisor context small: read artifacts, not raw transcripts.

CONSOLE CLIENT / CLI SETUP:
  Use pcder.exe for CLI commands (start, list, logs, send, wait, kill, kill-all, clear).
  It auto-starts the GUI dashboard and locates ProcessCommander.exe in its folder.
  Run `pcder.exe help` for human-readable usage, or `pcder.exe help --json` for
  machine-readable commands and setup instructions for LLMs.

COMPLETION NOTIFICATION / BACKGROUND TASKS:
  `pcder.exe start ... --wait --wait-ms 0 --json` keeps pcder.exe alive until the
  managed process exits. `--wait-ms 0` means unlimited wait. Exit code is 0 on
  completion, the child exit code on failure, and 124 if the wait times out.
  For an existing process: `pcder.exe wait --id <id> --wait-ms 0 --json`.
  ProcessCommander also writes a .done JSON file on exit.

MCP SERVER SETUP:
  Register this executable in .mcp.json (Claude Code) or claude_desktop_config.json:
    {
      "mcpServers": {
        "process-commander": {
          "command": "<absolute path to ProcessCommander.exe>",
          "args": ["mcp"]
        }
      }
    }
  Auto-register (recommended for LLM agents):
    pcder.exe mcp install                    (writes .mcp.json in the current directory)
    pcder.exe mcp install --client opencode  (writes/updates opencode.json)

  PRECONDITIONS (LLM agents):
    - Windows 10/11 x64, same user session as the dashboard; no admin required.
    - The dashboard auto-starts on first use and owns the Named Pipe server.
    - Logs, state and task queues live under logs/ (override: PROCESS_COMMANDER_LOG_DIR).

  MCP tools (19):
    Process (6):
      start_process, list_processes, get_process_output, send_process_input,
      wait_process, kill_process
    Task queue (13):
      taskqueue_add, taskqueue_list, taskqueue_get, taskqueue_retry,
      taskqueue_update, taskqueue_cancel, taskqueue_pause, taskqueue_resume,
      taskqueue_wait, taskqueue_purge, taskqueue_link, taskqueue_unlink,
      taskqueue_graph

  HOW TO USE (decision guide):
    - One command, e.g. a build script, CLI agent or shell -> start_process
    - Multiple steps, order, dependencies or retries      -> taskqueue_add + dependsOn
    - Never write &&, ||, a single &, PowerShell ';', newline chains or polling
      loops. ChainGuard rejects them: one job = one command with its own timeoutSec.
    - Required taskqueue job fields: id, command, summary, timeoutSec.
      Optional: cwd, env, dependsOn, idle, maxAttempts, retryDelaySec, success,
      killExclude, priority, instructionsFile / instructionsText.
    - Success is verified: exit code AND optional filesNewer / outputRegex.
    - States: waiting = queued and ready; pending = dependencies not satisfied;
      blocked = a dependency failed; running; succeeded; failed; cancelled.
    - Observe with taskqueue_wait / taskqueue_list / taskqueue_graph; after fixing
      the cause use taskqueue_retry (blocked dependents resume automatically).

  For the full machine-readable guide (agent.quick_start, decision, states):
    pcder.exe help --json
```
