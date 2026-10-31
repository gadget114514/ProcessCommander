# Usage 1: Console CLI (`pcder.exe`)

**[English](cli.md) | [日本語 (Japanese)](cli-ja.md)**

The console client `pcder.exe` communicates with the running GUI Dashboard via Named Pipe (`\\.\pipe\ProcessCommander_IPC_v1`). If the dashboard is not yet running, `ProcessCommander.exe` is located in the same directory and launched automatically.

> **Naming:** **PCR** is short for **ProcessCommander** (the GUI dashboard / MCP server, `ProcessCommander.exe`). When docs, prompts or job notes say "PCR", they mean ProcessCommander; `pcder.exe` is its console client. Both help outputs state this in a `NAMING` section, and `help --json` exposes it as `aliases.PCR`.

## Version (`--version` / `-V` / `version`)

```powershell
pcder.exe --version              # pcder 2.0.1
pcder.exe version --json         # {"ok":true,"program":"pcder","version":"2.0.1"}
ProcessCommander.exe --version   # ProcessCommander 2.0.1
ProcessCommander.exe version --json  # {"ok":true,"program":"ProcessCommander","version":"2.0.1"}
```

The version also appears in the first line of both help outputs, as `version` in `pcder.exe help --json`, and as `serverInfo.version` in the MCP `initialize` response.

## JSON Output (`--json` / `-j`)

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

While a process is running, the **End Time** column shows an estimated finish time in gray. Pass `--eta "18:30"` (or `--estimate-end`) to set it explicitly; otherwise `--timeout` makes the column show remaining seconds (e.g. `120s left`).

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

**Install the `pc-runner` subagent (Claude Code):**

```powershell
pcder.exe agent install-template                  # writes .claude\agents\pc-runner.md
pcder.exe agent install-template --path C:\proj --dry-run
```

`pc-runner` runs long builds and tests through ProcessCommander's MCP tools and returns only a short summary. The file is never overwritten.

`pcder.exe help` is a complete, self-contained usage guide. It covers **PCDER CONCEPTS** (process IDs, modes, stdin, logs, `.done` files, cwd/env), **exit codes**, **environment variables**, **examples**, the **per-invocation `--data-dir`** and how to keep one project's data consistent, the **task queue**, **MCP auto-registration**, the **LLM orchestration model**, and the full **19-tool MCP list**.

`pcder.exe help --json` returns the same material as machine-readable JSON:

| Key | Contents |
| :--- | :--- |
| `version`, `aliases` | pcder version and the `PCR` = ProcessCommander alias |
| `concepts` | Process IDs, modes, stdin, logs, `.done` files, cwd/env, data directory, lifecycle |
| `cli_exit_codes`, `environment_variables` | Exit codes and `PROCESS_COMMANDER_LOG_DIR` |
| `data_dir_option`, `effective_data_dir` | Per-invocation `--data-dir`, its resolved absolute path for this call, and project consistency rules |
| `common_examples` | Ready-to-run `pcder.exe` commands |
| `commands` | Every command and option, including `taskqueue add/list/get/retry/cancel/pause/resume/graph/wait/purge` and `mcp install` |
| `setup.mcp` | MCP registration, a ready-to-use config, and `auto_install` (`argv` + `command`) |
| `agent` | `working_model` (supervisor → ProcessCommander → worker LLM), `data_dir_consistency`, `decision`, `states`, `quick_start` |

This makes it possible for an LLM to read `help --json`, register the MCP server, pick the right execution model, and start using the tool without extra instructions. The legacy `ProcessCommander.exe <command>` form remains available for compatibility, but `pcder.exe` is the recommended console interface. `ProcessCommander.exe --help` documents the GUI/MCP server, the working model, and the same per-launch `--data-dir` option (see the [appendix](../README.md#appendix-processcommanderexe---help)).
