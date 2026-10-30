# Key Features

**[English](features.md) | [日本語 (Japanese)](features-ja.md)**

## 1. Native Win32 + Direct2D Multi-Terminal Dashboard GUI
- **14 Top KPI Summary Cards (Two Rows: Processes & Task Queues)**:
  - **Compact Horizontal Layout**: Labels and numbers are placed side-by-side on a single line, minimizing header height to maximize vertical space for process lists and terminals.
  - **Top Row (Processes)**: Real-time metrics for Total Terminals, Active Running, Waiting/Idle (no output or CPU activity for `>= 10s`), Completed (`Exit 0`), Failed/Killed, **Shortest Remaining Time** (live countdown of nearest active process timeout), and Total Job Object Memory & Process Count.
  - **Bottom Row (Tasks)**: Real-time metrics for Total Tasks, Active Running, Waiting/Pending, Succeeded, Failed, **Shortest Remaining Time** (live countdown of nearest active task timeout), and Active Queues / Concurrency summary.
  - Hover over any card for detailed tooltips and breakdown counts.
- **Two-Row Stacked Layout with Draggable Splitter**:
  - View both the **Process Summary Table** (top) and **Task Queue Table** (bottom) simultaneously in a dual-row view.
  - Interactive **draggable horizontal splitter bar** between the two tables allows smooth resizing of their relative heights.
  - **View Mode Cycling (`Ctrl+T` / View Toggle Button)**: Cycle between **Two Rows** (Processes + Tasks), **Processes Only**, and **Tasks Only** at any time.
- **Sortable Process Summary Table (Newest on Top)**:
  - Newly launched processes are automatically added to the **top of the list** (`ID ▼` descending by default).
  - Click **any column header** (`ID`, `Agent / Name`, `Status`, `Start Time`, `End Time`, `Uptime`, `PID (Tree)`, `Memory`, `CPU Time`, `Idle`, `Latest Output Preview`, `Command Line`) to toggle **Ascending (`▲`) / Descending (`▼`)** sort order.
  - **CPU-Aware Idle Detection**: While a process's CPU time keeps increasing, it stays `RUNNING` instead of being shown as `IDLE / WAITING`, and it is not killed by `--idle-timeout`. CPU time includes all processes in the Win32 Job Object tree.
- **Tab-Switched Full-Size Direct2D Terminal (`>= 80x25`)**:
  - Embedded hardware-accelerated Direct2D / DirectWrite / IME / Sixel VT terminal emulator (`TerminalView` + `ConPtySession`).
  - Switch between active terminals using the **Terminal Tab Bar** while always preserving at least **80 columns × 25 rows** of terminal viewport so TUI apps (`claude`, `opencode`, `vim`, `htop`) never wrap or corrupt their layout.
  - Type directly into the terminal (full keyboard input, `Ctrl+C`, IME composition, mouse wheel scrollback, and text selection).
- **Up to 16 Parallel Terminals in Live Grid View (`[▦]`)**:
  - Click **`[▦]`** on the toolbar to open a live DWM thumbnail & snapshot grid (up to **16 parallel terminals** in a `4×4` matrix). Click any cell or use arrow keys + Enter to jump directly to that terminal tab.
- **Compact Icon Buttons with Hover Tooltips**:
  - **`[▦]`** Grid View toggle
  - **`[☰]`** Hide/Show Process List toggle
  - **`[>_]`** Hide/Show Terminal Panel toggle
  - **`[⤓]`** Minimize to System Tray
  - All toolbar icon buttons provide instant status-aware hover tooltips.

## 2. Multi-Shell Launcher & `[Config]` Dialog (`cmd` / `powershell` / `bash`)
- Click **`[+ Launch (...)]`** on the toolbar to immediately spawn a new interactive ConPTY terminal tab.
- Open **`[Settings]`** → **`[Shell Config...]`** to select the default shell (`cmd`, `powershell`, or `bash` — auto-detecting Git Bash / WSL Bash), customize startup command lines, or launch any shell on the spot. Settings are persisted in `logs/config.json`.

![Shell Config Dialog](../docs/images/config_dialog.png)

## 3. History Management, Task Details & LLM JSON Tree Viewer
- **Hide Ended Processes** (Settings dialog): A convenient checkbox in the Settings dialog lets you toggle hiding/showing finished processes so you can focus exclusively on currently running tasks without deleting history.
- **`[Clear Ended]`** & **`[Reset Process IDs]`** (Settings dialog): Clear all completed/failed process history or renumber processes sequentially from `#1`.
- **Right-Click Context Menu & Task Details Dialog**: Right-click any process row or terminal tab (or double-click a row) to terminate the process tree (**`Kill Tree`** with confirmation), inspect full command lines, working directory, PID/Job metrics, log paths, `.done` files, and output preview, or copy commands to the clipboard.
- **LLM JSON Output Streaming & Interactive Tree Viewer**:
  - Native streaming event capture for major AI coding agents (`claude`, `agy`, `opencode`, `codex`).
  - Automatically captures structured event streams into `<logfile>.stream.jsonl` in addition to raw console logs.
  - The Task Details dialog features an embedded **JSON Tree Viewer** (powered by `nlohmann/json`): expand, collapse, and inspect hierarchical JSON payloads, tool inputs/outputs, thought blocks, and token usage without having to manually read raw JSONL streams.

## 4. System Tray Integration
- Click **`[⤓]`** (Tray icon button), minimize the window, or press the window close button (`[X]`) to hide ProcessCommander into the **Windows System Tray** while keeping all background ConPTY processes and IPC/MCP services running.
- Left-click or double-click the tray icon (or right-click to open the tray menu) to restore and expand the dashboard at any time.

## 5. Persistent Logs, State Recovery & `.done` Completion Marker Files
- **Real-Time Per-Process Logs**: Every process's stdout/ConPTY stream is continuously written to `logs/proc_<id>_<name>_<YYYYMMDD_HHMMSS>.log` (or a custom `--log-file <path>`).
- **Completion Marker Files (`.done`) for AI Agents**: On process exit, ProcessCommander atomically writes a JSON completion file (`logs/proc_<id>.done` or custom `--done-file <path>`), allowing Claude or automation scripts to detect task completion via simple file-existence checks.
- **State Persistence Across Restarts**: Process metadata and history are saved in `logs/processes_state.json` and automatically restored when ProcessCommander starts.

## 6. Reliable Win32 Job Object Process Tree Control
- Each process runs inside a dedicated Win32 `Job Object` (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`), tracking all child/grandchild processes and memory usage and guaranteeing clean tree termination on `kill`, `--timeout`, or `--idle-timeout`.

## 7. Settings Dialog, Theme & Localization
- Click **`[Settings]`** to open the settings dialog.
- **Language**: switch between **Japanese / English / Español** buttons; the dashboard, dialogs, menus, and status bar update immediately.
- **Theme (colors)**: **Windows** (default; follows the Windows light/dark mode and personalization accent color), **Dark**, **Light**, or **High Contrast**. Buttons, list headers (both process list and job queue list), scrollbars, and tab labels follow the active theme colors.
- **`[Shell Config...]`**, **`[Reset Process IDs]`**, **`[Clear Ended]`**, and **Hide Ended Processes** checkbox are available here (keeping the toolbar clean and focused).
- Settings, language, theme, list/terminal visibility, and layout are **auto-saved** to `logs/config.json` and restored on the next start.

![Settings Dialog](../docs/images/settings_dialog.png)

## 8. Task Queue: Ordering, Dependencies, Timeouts and Automatic Retries
- Register jobs with **one command each**; ProcessCommander decides when to run them (concurrency, dependencies, priority) and persists the queue across restarts (`logs/taskqueues/<name>.json`).
- Per-job `timeoutSec`, idle detection (output **and** CPU), success checks (`exitCode`, `filesNewer`, `outputRegex`), automatic retries (`maxAttempts`), and `blocked` dependents when a dependency fails.
- **ChainGuard** rejects shell chains and wait loops in `start_process` / `taskqueue_add`; use `dependsOn` instead. CLI `--allow-chain` is the only escape hatch (never exposed over MCP).
- GUI: click the leftmost view mode toggle button or press **Ctrl+T** to cycle between **Two Rows** (Processes + Tasks), **Processes Only**, and **Tasks Only**. In Two Rows mode, drag the horizontal splitter bar to adjust relative heights. Running jobs are listed first and highlighted; failed rows are muted red and timed-out rows muted amber (theme-based, low saturation).
- See [Task Queue](taskqueue.md) for the data model and examples.
