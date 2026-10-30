# Usage 2: MCP Server for Claude Code / Claude Desktop

**[English](mcp.md) | [日本語 (Japanese)](mcp-ja.md)**

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

## Provided MCP Tools

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
