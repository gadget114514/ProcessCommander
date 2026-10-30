# Task Queue: Ordered Jobs, Dependencies and Retries

**[English](taskqueue.md) | [日本語 (Japanese)](taskqueue-ja.md)**

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
| `defer` | `{ "exitCodes": [75], "delaySec": 60, "limit": 60 }` (the default). An attempt that exits with one of `exitCodes` means "nothing ran, try again later" (75 = EX_TEMPFAIL, e.g. `ww-run` refusing an overlapping card): the attempt is recorded as `deferred`, the job goes back to pending and starts again after `delaySec`, without using up `maxAttempts`. After `limit` deferrals (`0` = no limit) the exit code is an ordinary failure. `{ "exitCodes": [] }` turns it off. The graph shows `retry_in_sec`. |
| `success` | `exitCode` (default 0) AND `filesNewer` (exist and newer than the attempt start) AND `outputRegex` (last 200 log lines). |
| `cwd`, `env`, `killExclude`, `priority` | Working directory, extra environment, image names excluded from tree kill, scheduling priority. |
| `resources` | `{ "write": [...], "read": [...] }`: paths/globs (relative to `cwd`; a bare file name `Foo.cs` means `**/Foo.cs`) or `@name` locks. Jobs whose sets overlap (write/write, write/read) never run together, across all queues; read/read may. On by default (Settings checkbox). See [resource-locks.md](resource-locks.md). |
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

**GUI:** click the view mode toggle button (leftmost toolbar button) or press **Ctrl+T** to cycle between **Two Rows** (Processes + Tasks), **Processes Only**, and **Tasks Only**. In Two Rows mode, drag the horizontal splitter bar to adjust relative heights. Running jobs are listed first and highlighted; failed rows use a muted red and timed-out rows a muted amber (theme colors).
