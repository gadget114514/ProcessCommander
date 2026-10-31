#include "cli.h"
#include "common.h"
#include "ipc_client.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace pc {

namespace {

std::string ResolveGuiExePathUtf8() {
    wchar_t exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0) return "ProcessCommander.exe";

    std::wstring current(exePath, len);
    auto slash = current.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        std::wstring candidate = current.substr(0, slash + 1) + L"ProcessCommander.exe";
        if (_wcsicmp(candidate.c_str(), current.c_str()) == 0) {
            return WideToUtf8(current);
        }
        DWORD attr = GetFileAttributesW(candidate.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            return WideToUtf8(candidate);
        }
    }
    return WideToUtf8(current);
}


struct McpInstallOutcome {
    bool ok = false;
    bool changed = false;
    bool dryRun = false;
    std::string error;
    std::string targetPath;
    std::string entryJson;
};

bool FindJsonContainerBounds(const std::string& json, const std::string& key, size_t& outOpen, size_t& outClose) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = json.find(pattern, pos)) != std::string::npos) {
        size_t after = pos + pattern.size();
        while (after < json.size() && std::isspace(static_cast<unsigned char>(json[after]))) after++;
        if (after < json.size() && json[after] == ':') {
            after++;
            while (after < json.size() && std::isspace(static_cast<unsigned char>(json[after]))) after++;
            if (after < json.size() && json[after] == '{') {
                int depth = 0;
                bool inStr = false;
                bool esc = false;
                for (size_t i = after; i < json.size(); ++i) {
                    char c = json[i];
                    if (inStr) {
                        if (esc) esc = false;
                        else if (c == '\\') esc = true;
                        else if (c == '"') inStr = false;
                        continue;
                    }
                    if (c == '"') { inStr = true; continue; }
                    if (c == '{') depth++;
                    else if (c == '}') {
                        depth--;
                        if (depth == 0) {
                            outOpen = after;
                            outClose = i;
                            return true;
                        }
                    }
                }
            }
        }
        pos += pattern.size();
    }
    return false;
}

bool JsonContainerHasKey(const std::string& json, size_t open, size_t close, const std::string& name) {
    std::string pattern = "\"" + name + "\"";
    size_t pos = open + 1;
    while (true) {
        size_t found = json.find(pattern, pos);
        if (found == std::string::npos || found >= close) return false;
        size_t after = found + pattern.size();
        while (after < close && std::isspace(static_cast<unsigned char>(json[after]))) after++;
        if (after < close && json[after] == ':') return true;
        pos = found + pattern.size();
    }
}

std::string InsertJsonEntry(const std::string& json, size_t open, size_t close,
                            const std::string& name, const std::string& entryBody) {
    size_t p = open + 1;
    while (p < close && std::isspace(static_cast<unsigned char>(json[p]))) p++;
    bool empty = (p >= close);
    std::string insertion = "\n    \"" + name + "\": " + entryBody;
    if (!empty) insertion += ",";
    std::string out = json;
    out.insert(open + 1, insertion);
    return out;
}

McpInstallOutcome InstallMcpConfig(const std::string& client, const std::string& explicitPath,
                                   const std::string& serverName, bool dryRun, const std::string& exePath,
                                   const std::string& dataDir) {
    McpInstallOutcome out;
    out.dryRun = dryRun;

    bool opencode = (client == "opencode");
    std::string container = opencode ? "mcp" : "mcpServers";
    std::string dataArgs;
    if (!dataDir.empty()) {
        dataArgs = ",\"--data-dir\",\"" + JsonEscape(dataDir) + "\"";
    }
    out.entryJson = opencode
        ? "{\"type\":\"local\",\"command\":[\"" + JsonEscape(exePath) + "\",\"mcp\"" + dataArgs + "],\"enabled\":true}"
        : "{\"command\":\"" + JsonEscape(exePath) + "\",\"args\":[\"mcp\"" + dataArgs + "]}";

    std::string target = explicitPath;
    if (target.empty()) {
        if (opencode) {
            if (GetFileAttributesW(L"opencode.json") != INVALID_FILE_ATTRIBUTES) target = "opencode.json";
            else if (GetFileAttributesW(L"opencode.jsonc") != INVALID_FILE_ATTRIBUTES) target = "opencode.jsonc";
            else target = "opencode.json";
        } else {
            target = ".mcp.json";
        }
    }

    std::wstring targetW = ResolveAbsolutePathWide(Utf8ToWide(target));
    out.targetPath = WideToUtf8(targetW);

    std::string content = ReadFileUtf8Wide(targetW);
    if (content.empty()) {
        std::string fresh = "{\n  \"" + container + "\": {\n    \"" + serverName + "\": " +
                            out.entryJson + "\n  }\n}\n";
        if (!dryRun) WriteFileAtomicWide(targetW, fresh);
        out.ok = true;
        out.changed = true;
        return out;
    }

    size_t open = 0;
    size_t close = 0;
    if (!FindJsonContainerBounds(content, container, open, close)) {
        out.error = "config file exists but has no \"" + container + "\" object: " + out.targetPath +
                    " -- add this entry manually: \"" + serverName + "\": " + out.entryJson;
        return out;
    }
    if (JsonContainerHasKey(content, open, close, serverName)) {
        out.ok = true;
        out.changed = false;
        return out;
    }
    std::string updated = InsertJsonEntry(content, open, close, serverName, out.entryJson);
    if (!dryRun) WriteFileAtomicWide(targetW, updated);
    out.ok = true;
    out.changed = true;
    return out;
}

} // namespace

void PrintCliUsage() {
    std::cout <<
"pcder " PC_VERSION " - ProcessCommander console client\n"
R"(
NAMING:
  PCR = ProcessCommander. "PCR" in docs, prompts or job notes means ProcessCommander
  (the GUI dashboard / MCP server, ProcessCommander.exe); pcder.exe is its console client.

USAGE:
  pcder [--json] <command> [options]

GLOBAL OPTIONS:
  --json, -j               Print machine-readable JSON instead of human-friendly text.
  -h, --help               Show this help.
  -V, --version            Show the version (also: pcder version).

DATA DIRECTORY (per invocation):
  --data-dir <path>        Store logs, state, task queues and config in <path> for
                           THIS pcder call. May be placed before or after the command.
                           Point it at a project folder to git-manage the data:
                             pcder.exe --data-dir .processcommander taskqueue add --file jobs.json
                             pcder.exe taskqueue list main --data-dir .processcommander
                           Each data dir gets its own dashboard instance, so different
                           projects can run at the same time.

  CONSISTENCY (one data dir per project):
    --data-dir is per invocation, not a global setting. To keep a project's
    logs/state/task queues consistent:
      1. Choose one project-local directory, e.g. .processcommander in the project root.
      2. Register the MCP server once with it:
           pcder.exe mcp install --data-dir .processcommander
         This writes "--data-dir .processcommander" into the project's MCP config,
         so every MCP tool call for the project uses the same directory.
      3. Pass the same --data-dir on every direct pcder call.
      4. Relative paths resolve against the caller's current directory. MCP clients
         normally start the server in the project root; if the cwd is uncertain,
         put an absolute path in the MCP config.
      5. Different data dirs are isolated (own dashboard instance, Named Pipe and
         mutex); they never share processes or queues.
      6. Verify: pcder.exe --data-dir <path> help --json prints
         "effective_data_dir", and the dashboard status bar shows the same path.

PCDER CONCEPTS:
  Process IDs : Numeric IDs assigned by the dashboard (#1, #2, ...). Use them with
                list --id, logs --id, send --id, wait --id and kill --id.
  Modes       : pipe    = anonymous pipes (default; non-interactive CLI agents)
                pty     = Win32 ConPTY (interactive TUI: opencode, claude)
                console = separate visible console window
  stdin       : pipe mode keeps stdin writable (pcder send) unless --close-stdin;
                close it for programs that wait for EOF.
  Logs        : logs/proc_<id>_<name>_<timestamp>.log (or --log-file).
  Done file   : atomic JSON on exit (logs/proc_<id>.done or --done-file) with
                state, exit_code, reason, log_file and done_file.
  cwd / env   : --cwd defaults to the caller's directory; PWD and INIT_CWD are
                set for the child and OLDPWD is removed.
  JSON output : --json / -j before or after the command prints one JSON object.
  Lifecycle   : start returns immediately unless --wait; use wait --id or the
                .done file to detect completion.

EXIT CODES:
  0    success / completed
  1    CLI or configuration error (also: process failure without --wait)
  124  wait timed out while the process is still running
       start --wait / wait return the managed process's exit code

ENVIRONMENT:
  PROCESS_COMMANDER_LOG_DIR   Override the logs directory (state, logs, queues).

EXAMPLES:
  pcder.exe list --json
  pcder.exe start --name build --cmd "cmd /c build.cmd" --timeout 1800 --wait --wait-ms 0 --json
  pcder.exe start --name tui --pty --cmd "opencode"
  pcder.exe logs --id 1 --tail 80
  pcder.exe send --id 1 --input "status"
  pcder.exe taskqueue add --file jobs.json --taskqueue main
  pcder.exe mcp install --client opencode

COMMANDS:
  list [--id <id>]
      List all managed processes and their live metrics.

  logs --id <id> [--tail <lines>] [--wait-ms <ms>] [--raw]
      Fetch process output logs (ANSI codes stripped by default unless --raw).

  start [options] -- <command...>
      Start a managed process (e.g. claude, opencode) under a Win32 Job Object.
      Options:
        --cmd "<command>"        Command line to execute
        --name <label>           Display name on dashboard (e.g. "opencode-worker")
        --mode <pty|pipe|console>
                                 pty     = Win32 ConPTY interactive session
                                 pipe    = Anonymous pipes (default; no ConPTY)
                                 console = Separate visible console window
        --pty                    Shortcut for --mode pty (enable ConPTY).
        --open-stdin             Pipe mode only (default): keep stdin writable for pcder send.
        --close-stdin            Pipe mode only: close stdin after launch so non-interactive
                                 programs (e.g. opencode run) receive EOF and can finish.
        --cwd <dir>              Working directory (default: the caller's current directory)
        --log-file <path>        Custom path for live output log
        --done-file <path>       Custom completion marker file created on exit
        --timeout <sec>          Max runtime timeout in seconds (0 = unlimited)
        --idle-timeout <sec>     Hang timeout if no output for N seconds (0 = unlimited)
        --eta <time>             Estimated end time shown in gray while running
                                 (e.g. "18:30" or "2026-10-22 18:30:00").
                                 If omitted, --timeout shows "remaining N seconds".
         --estimate-end <time>    Alias for --eta
         --agent-type <claude|agy|antigravity|opencode|codex|aider|ollama>
                                  Hint agent type for JSON stream parsing (auto-detected if omitted).
         --json-stream            Enable JSONL stream parsing and write <log>.stream.jsonl for LLM agent output.
         --wait                   Block until process exits (unlimited unless --wait-ms is set)
        --wait-ms <ms>           Max wait when --wait is set; initial output wait otherwise
                                 (default: 500; 0 = unlimited with --wait)

  send --id <id> --input "<text>" [--no-newline] [--wait-ms <ms>]
      Send input text to a running process's stdin or ConPTY session.

  wait --id <id> [--wait-ms <ms>]
      Wait until the specified process exits (or wait-ms elapses).

  kill --id <id>
      Forcefully kill the entire process tree (Job Object) for <id>.

  kill-all
      Forcefully kill all running managed process trees.

  clear
      Remove finished processes from the dashboard list.

  agent status <id>
      Show current agent status, active tool, thoughts, message, and supervisor context.

  agent stream <id> [--tail <n>] [--type <type>] [--since <seq>] [--summary]
      View captured JSON streaming events for claude, agy, or opencode agents.

  agent info <id> [--notes "<notes>"] [--goal "<goal>"]
      Update supervisor notes or goal for an ongoing agent.

  mcp install [--client claude|opencode] [--path <file>] [--name <name>] [--data-dir <path>] [--dry-run]
      Register ProcessCommander as an MCP server automatically. Writes or updates
      .mcp.json (Claude Code, default) or opencode.json (--client opencode).
      --data-dir embeds "--data-dir <path>" in the generated MCP server args so the
      project keeps its logs/state/task queues inside that directory.
      --json prints the machine-readable result; --dry-run previews without writing.

TASK QUEUE:
  taskqueue add --file <jobs.json> [--taskqueue <name>] [--concurrency <n>] [--notify-file <path>]
      Register jobs (array, or an object with "jobs"/"taskqueue"/"concurrency").
      Each job needs id, command, summary, timeoutSec; dependsOn replaces '&&'.
      Optional: cwd, env, dependsOn, idle, maxAttempts, retryDelaySec, success,
      killExclude, resources {write,read}, defer, priority, instructionsFile/instructionsText.
      resources: jobs whose write/read sets overlap (write/write, write/read) never
      run together, across all queues; read/read may. Paths/globs resolve against
      cwd; a bare file name (Foo.cs) means **/Foo.cs; "@name" is a named lock.
      Disable with Settings / config.json resource_locks.
      defer {exitCodes,delaySec,limit} (default [75], 60, 60): an attempt that exits
      with one of exitCodes ("nothing ran, try again later", e.g. ww-run refusing an
      overlapping card) goes back to pending after delaySec without using up
      maxAttempts; after limit deferrals (0 = no limit) it is an ordinary failure.

  taskqueue list [<name>] [--state <state>]
      List jobs with state, queue, summary and blockedBy.

  taskqueue get <id> [--rev <n>] [--no-instructions]
  taskqueue retry <id>
  taskqueue cancel <id>
  taskqueue pause|resume <name>
  taskqueue graph [<name>] [--root <id>]     (ready, waiting_for, resource_wait, retry_in_sec, blocked)
  taskqueue wait <name> [--since <version>] [--timeout-ms <ms>]
  taskqueue purge <name> [--states succeeded,failed,cancelled]

  start/run ... --allow-chain
      Allow a chained command (&&, ||, &, ';') as a single process. Task-queue
      jobs never allow chains; use dependsOn instead.

COMPLETION / BACKGROUND TASKS:
  Use --wait to keep pcder.exe alive until the managed process exits. Add
  --wait-ms 0 for an unlimited wait (or a positive value for a maximum).
  The exit code is 0 on completion, the child exit code on failure, and 124
  if the wait times out.

    pcder.exe start --name task --wait --wait-ms 0 --json --cmd "opencode run ..."
    pcder.exe wait --id <id> --wait-ms 0 --json

  ProcessCommander also writes a .done JSON file on exit. Without --wait,
  start returns immediately; poll the .done file or call wait.

SETUP FOR LLM AGENTS:
  CLI:
    Use pcder.exe (recommended) or the legacy "ProcessCommander.exe <command>" form.
    Run "pcder.exe help --json" to retrieve this help as machine-readable JSON.
    pcder.exe auto-starts the GUI dashboard on first use if it is not running.

  MCP (stdio):
    Register ProcessCommander.exe in .mcp.json (Claude Code) or
    claude_desktop_config.json (Claude Desktop):
)";
    std::cout <<
"      {\n"
"        \"mcpServers\": {\n"
"          \"process-commander\": {\n"
"            \"command\": \"" << JsonEscape(ResolveGuiExePathUtf8()) << "\",\n"
"            \"args\": [\"mcp\"]\n"
"          }\n"
"        }\n"
"      }\n"
"\n"
"    AUTO-REGISTER (for LLM agents; no manual JSON editing):\n"
"      pcder.exe mcp install                    # .mcp.json (Claude Code)\n"
"      pcder.exe mcp install --client opencode  # opencode.json\n"
"      pcder.exe mcp install --path <file> --json --dry-run\n"
"\n"
"    ORCHESTRATION MODEL (how an LLM should use this):\n"
"      A supervising LLM uses ProcessCommander as its execution layer to run\n"
"      child-process worker LLMs/CLIs as managed jobs.\n"
"\n"
"        Supervisor LLM  ->  ProcessCommander  ->  Worker LLM / build command\n"
"        plans + declares    timeouts, logs,        one job command;\n"
"        jobs                retries, state         writes artifacts\n"
"\n"
"      - The supervisor keeps its context small: it plans and declares jobs; it\n"
"        does not run the work itself, chain commands, or fabricate success.\n"
"      - One job = one command. Use start_process for a single worker; use\n"
"        taskqueue_add with dependsOn for multiple steps or dependencies.\n"
"      - Each job needs a one-line summary and its own timeoutSec. Pass detailed\n"
"        instructions via instructionsFile/instructionsText; the worker reads\n"
"        {instructions} / PC_JOB_INSTRUCTIONS and writes artifacts.\n"
"      - Workers may be LLM CLIs (opencode run, claude -p, ...) or build scripts.\n"
"      - Role clarity: when the supervisor and worker use the same engine\n"
"        (e.g. Claude supervising `claude -p`, or opencode supervising\n"
"        `opencode run`), roles can blur and cause confusion; prefer a\n"
"        different worker engine for workers, or give workers a clearly\n"
"        distinct role/prompt and avoid nested supervision.\n"
"      - Verify success with exit code + filesNewer/outputRegex; observe with\n"
"        taskqueue_wait / taskqueue_list / taskqueue_graph and logs; retry with\n"
"        taskqueue_retry. Blocked dependents resume automatically.\n"
"      - Never poll .done files; use taskqueue_wait or wait_process.\n"
"      - Delegating to a worker LLM: run_worker blocks until the worker exits and\n"
"        returns only its final result; the raw stream stays in stream_file.\n"
"      - Your harness does not see ProcessCommander as a native sub-agent. To have\n"
"        it shown as one, define a thin custom agent (Claude Code:\n"
"        .claude/agents/<name>.md, cheap model, this tool only) that calls run_worker\n"
"        and returns the result. The wrapper costs extra tokens; calling run_worker\n"
"        directly is cheapest.\n"
"\n"
"    MCP tools (24):\n"
"      Process:    start_process, list_processes, get_process_output,\n"
"                  send_process_input, wait_process, kill_process\n"
"      Worker:     run_worker, wait_worker, get_agent_stream, get_agent_state,\n"
"                  update_agent_info\n"
"      Task queue: taskqueue_add, taskqueue_list, taskqueue_get, taskqueue_retry,\n"
"                  taskqueue_update, taskqueue_cancel, taskqueue_pause, taskqueue_resume,\n"
"                  taskqueue_wait, taskqueue_purge, taskqueue_link, taskqueue_unlink,\n"
"                  taskqueue_graph\n"
"\n"
"NOTE:\n"
"  The GUI dashboard is provided by ProcessCommander.exe, which is auto-started\n"
"  on first use if it is not already running.\n";
}

namespace {

void PrintCliUsageJson() {
    const std::string guiExe = JsonEscape(ResolveGuiExePathUtf8());
    const std::string dataDir = JsonEscape(WideToUtf8(GetEffectiveDataDirectoryWide()));
    std::cout << R"({
  "ok": true,
  "program": "pcder",
  "version": ")" PC_VERSION R"(",
  "aliases": {"PCR": "ProcessCommander (the GUI dashboard / MCP server, ProcessCommander.exe); pcder is its console client."},
  "description": "ProcessCommander console client. Sends commands to the running ProcessCommander GUI dashboard over a named pipe.",
  "usage": "pcder [--json] <command> [options]",
  "effective_data_dir": ")" << dataDir << R"(",
  "global_options": [
    {"name": "--json", "alias": "-j", "description": "Print the raw machine-readable JSON response. May be placed before or after the command."},
    {"name": "--help", "alias": "-h", "description": "Show usage. Combine with --json to get this machine-readable help."},
    {"name": "--version", "alias": "-V", "description": "Show the version. Combine with --json for {\"ok\":true,\"version\":...}."}
  ],
  "data_dir_option": {
    "name": "--data-dir",
    "value": "<path>",
    "per_invocation": true,
    "description": "Store logs, state, task queues and config in <path> for this invocation only. May be placed before or after the command. Use a project-local directory to git-manage the data. Each data dir gets its own dashboard instance (separate Named Pipe and mutex), so different projects can run concurrently.",
    "examples": [
      "pcder.exe --data-dir .processcommander taskqueue add --file jobs.json",
      "pcder.exe taskqueue list main --data-dir .processcommander",
      "pcder.exe mcp install --data-dir .processcommander"
    ]
  },
  "concepts": {
    "process_ids": "Numeric IDs assigned by the dashboard (#1, #2, ...); use them with list/logs/send/wait/kill.",
    "data_dir": "Logs, state, task queues and config live in one data directory. Pass --data-dir <path> on every pcder invocation (or embed it with pcder mcp install --data-dir) to keep that data inside the current project for git. Each data directory has its own dashboard instance and Named Pipe.",
    "modes": {
      "pipe": "Default. Anonymous pipes for stdout/stdin; best for non-interactive programs and LLM CLIs. Use --close-stdin for programs that wait for EOF.",
      "pty": "Win32 ConPTY; interactive TUI (opencode/claude). --pty is a shortcut.",
      "console": "Separate visible console window."
    },
    "stdin": "Pipe mode keeps stdin writable by default (pcder send). --close-stdin closes it after launch so readers receive EOF.",
    "logs": "Each process writes a live log; default logs/proc_<id>_<name>_<timestamp>.log. Use --log-file for a custom path.",
    "done_files": "On exit an atomic JSON .done file is written (default logs/proc_<id>.done) with state, exit_code, reason, log_file and done_file.",
    "cwd": "Defaults to the caller's current directory (resolved to an absolute path). PWD and INIT_CWD are set for the child; OLDPWD is removed.",
    "wait": "start --wait blocks until exit; --wait-ms 0 = unlimited. wait --id blocks for an existing process. Exit code 124 means the wait timed out.",
    "json": "Pass --json (or -j) before or after the command for a single JSON object. Errors set ok=false and exit code 1.",
    "lifecycle": "start returns immediately unless --wait; use wait --id or the .done file to detect completion."
  },
  "cli_exit_codes": {
    "0": "success / completed",
    "1": "CLI or configuration error; also a process failure without --wait",
    "124": "wait timed out while the process is still running",
    "child_exit_code": "start --wait / wait return the managed process exit code"
  },
  "environment_variables": {
    "PROCESS_COMMANDER_LOG_DIR": "Override the logs directory (state, logs, task queues)."
  },
  "common_examples": [
    "pcder.exe list --json",
    "pcder.exe start --name build --cmd \"cmd /c build.cmd\" --timeout 1800 --wait --wait-ms 0 --json",
    "pcder.exe start --name tui --pty --cmd \"opencode\"",
    "pcder.exe logs --id 1 --tail 80",
    "pcder.exe send --id 1 --input \"status\"",
    "pcder.exe taskqueue add --file jobs.json --taskqueue main",
    "pcder.exe mcp install --client opencode"
  ],
  "commands": [
    {
      "name": "list",
      "aliases": ["status"],
      "description": "List managed processes and their live metrics.",
      "usage": "pcder list [--id <id>]",
      "options": [
        {"name": "--id", "value": "<id>", "description": "Show only the specified process ID."}
      ]
    },
    {
      "name": "logs",
      "aliases": ["output"],
      "description": "Fetch process output logs. ANSI/VT100 escape codes are stripped by default.",
      "usage": "pcder logs --id <id> [--tail <lines>] [--wait-ms <ms>] [--raw]",
      "options": [
        {"name": "--id", "value": "<id>", "description": "Process ID."},
        {"name": "--tail", "value": "<lines>", "description": "Return only the last N lines (0 = all)."},
        {"name": "--wait-ms", "value": "<ms>", "description": "Wait up to N ms before reading output."},
        {"name": "--raw", "description": "Keep ANSI/VT100 escape codes."}
      ]
    },
    {
      "name": "start",
      "aliases": ["run"],
      "description": "Start a managed process under a Win32 Job Object and display it in the dashboard. Use '--' to pass the command line verbatim. Add --wait (with --wait-ms 0) to block until completion.",
      "usage": "pcder start [options] [--wait] [--wait-ms <ms>] -- <command...>",
      "options": [
        {"name": "--cmd", "value": "<command>", "description": "Command line to execute."},
        {"name": "--name", "value": "<label>", "description": "Display name on the dashboard."},
        {"name": "--mode", "value": "<pty|pipe|console>", "values": ["pty", "pipe", "console"], "description": "Execution mode. Default: pipe (no ConPTY)."},
        {"name": "--pty", "description": "Enable ConPTY. Shortcut for --mode pty. Without this flag or an explicit --mode pty, no ConPTY is used."},
        {"name": "--open-stdin", "description": "Pipe mode only (default): keep stdin writable for pcder send."},
        {"name": "--close-stdin", "description": "Pipe mode only: close stdin after launch so non-interactive programs (e.g. opencode run) receive EOF and can finish."},
        {"name": "--cwd", "value": "<dir>", "description": "Working directory. Default: the caller's current directory."},
        {"name": "--log-file", "aliases": ["--log"], "value": "<path>", "description": "Custom live-output log path."},
        {"name": "--done-file", "aliases": ["--done"], "value": "<path>", "description": "Custom completion marker path, written when the process exits."},
        {"name": "--timeout", "value": "<sec>", "description": "Maximum runtime in seconds (0 = unlimited)."},
        {"name": "--idle-timeout", "value": "<sec>", "description": "Kill if no output for N seconds (0 = unlimited)."},
        {"name": "--eta", "aliases": ["--estimate-end"], "value": "<time>", "description": "Estimated end time shown in gray in the End Time column while the process is running (e.g. '18:30' or '2026-10-22 18:30:00'). If omitted, --timeout causes the column to show remaining seconds."},
        {"name": "--agent-type", "value": "<claude|agy|antigravity|opencode|codex|aider|ollama>", "description": "Hint agent type for JSON stream parsing (auto-detected if omitted)."},
        {"name": "--json-stream", "description": "Enable JSONL stream parsing and write <log>.stream.jsonl for LLM agent output."},
        {"name": "--wait", "description": "Block until the process exits (unlimited unless --wait-ms is set)."},
        {"name": "--wait-ms", "value": "<ms>", "description": "With --wait: maximum wait (0 = unlimited). Without --wait: initial output wait. Default: 500."}
      ]
    },
    {
      "name": "send",
      "aliases": ["write"],
      "description": "Send text input to a running process's stdin or ConPTY session.",
      "usage": "pcder send --id <id> --input \"<text>\" [--no-newline] [--wait-ms <ms>]",
      "options": [
        {"name": "--id", "value": "<id>", "description": "Process ID."},
        {"name": "--input", "value": "<text>", "description": "Text to send."},
        {"name": "--no-newline", "description": "Do not append a newline (default: append)."},
        {"name": "--wait-ms", "value": "<ms>", "description": "Wait up to N ms for output after sending. Default: 400."}
      ]
    },
    {
      "name": "wait",
      "description": "Wait until the specified process exits or the timeout elapses.",
      "usage": "pcder wait --id <id> [--wait-ms <ms>]",
      "options": [
        {"name": "--id", "value": "<id>", "description": "Process ID."},
        {"name": "--wait-ms", "value": "<ms>", "description": "Maximum wait time. Default: 30000."}
      ]
    },
    {
      "name": "kill",
      "description": "Forcefully kill the entire process tree (Job Object) for a process ID.",
      "usage": "pcder kill --id <id>",
      "options": [
        {"name": "--id", "value": "<id>", "description": "Process ID."}
      ]
    },
    {
      "name": "kill-all",
      "description": "Forcefully kill all running managed process trees.",
      "usage": "pcder kill-all",
      "options": []
    },
    {
      "name": "clear",
      "description": "Remove finished processes from the dashboard history.",
      "usage": "pcder clear",
      "options": []
    },
    {
      "name": "mcp install",
      "description": "Register ProcessCommander as an MCP server by writing or updating a client config file (.mcp.json for Claude Code, opencode.json for opencode).",
      "usage": "pcder mcp install [--client claude|opencode] [--path <file>] [--name <name>] [--data-dir <path>] [--dry-run] [--json]",
      "options": [
        {"name": "--client", "value": "<claude|opencode>", "description": "Target client. Default: claude."},
        {"name": "--path", "value": "<file>", "description": "Config file to write. Default: .mcp.json (claude) or opencode.json (opencode)."},
        {"name": "--name", "value": "<name>", "description": "MCP server name. Default: process-commander."},
        {"name": "--data-dir", "value": "<path>", "description": "Embed --data-dir <path> in the generated MCP server args so this project keeps its logs/state/task queues in that directory."},
        {"name": "--dry-run", "description": "Report the change without writing the file."}
      ]
    },
    {
      "name": "taskqueue",
      "description": "Manage the persistent task queue: ordered jobs, dependencies, per-job timeouts, success checks and automatic retries.",
      "usage": "pcder taskqueue <add|list|get|retry|cancel|pause|resume|graph|wait|purge> ...",
      "subcommands": [
        {"name": "add", "usage": "pcder taskqueue add --file <jobs.json> [--taskqueue <name>] [--concurrency <n>] [--notify-file <path>]", "description": "Register jobs. Each job needs id, command, summary and timeoutSec; dependsOn replaces shell chaining."},
        {"name": "list", "usage": "pcder taskqueue list [<name>] [--state <state>]", "description": "List jobs with state, queue, summary and blockedBy."},
        {"name": "get", "usage": "pcder taskqueue get <id> [--rev <n>] [--no-instructions]", "description": "Show one job with attempts and instructions."},
        {"name": "retry", "usage": "pcder taskqueue retry <id>", "description": "Requeue a failed/blocked/cancelled job; blocked dependents resume automatically."},
        {"name": "cancel", "usage": "pcder taskqueue cancel <id>", "description": "Cancel a job; a running attempt is killed and is not retried."},
        {"name": "pause", "usage": "pcder taskqueue pause <name>", "description": "Stop starting new jobs in the queue; running jobs continue."},
        {"name": "resume", "usage": "pcder taskqueue resume <name>", "description": "Resume starting jobs in the queue."},
        {"name": "graph", "usage": "pcder taskqueue graph [<name>] [--root <id>]", "description": "Dependency graph with ready, waiting_for, resource_wait, retry_in_sec, blocked and dependents."},
        {"name": "wait", "usage": "pcder taskqueue wait <name> [--since <version>] [--timeout-ms <ms>]", "description": "Long-poll until the queue version changes."},
        {"name": "purge", "usage": "pcder taskqueue purge <name> [--states succeeded,failed,cancelled]", "description": "Remove finished jobs."}
      ],
      "options": []
    },
    {
      "name": "agent",
      "description": "Inspect and supervise LLM agents (Claude, agy, opencode, codex, aider, ollama) running under ProcessCommander. JSON stream events are parsed into <log>.stream.jsonl and visible in the GUI (LLM JSON Stream dialog). Use pcder/ProcessCommander to supervise and watch streams.",
      "usage": "pcder agent <status|stream|info> <id> [options]",
      "subcommands": [
        {"name": "status", "usage": "pcder agent status <id>", "description": "Display current agent status, model, active tool, thoughts, message, and supervisor context."},
        {"name": "stream", "usage": "pcder agent stream <id> [--tail <n>] [--type <type>] [--since <seq>] [--summary]", "description": "View captured JSON streaming events from <log>.stream.jsonl (parsed tool_call/tool_result/thinking/message)."},
        {"name": "info", "usage": "pcder agent info <id> [--notes <notes>] [--goal <goal>]", "description": "Update supervisor instructions, notes, or goals."}
      ],
      "options": [
        {"name": "--tail", "value": "<n>", "description": "For stream: number of recent events."},
        {"name": "--type", "value": "<type>", "description": "For stream: filter by event type (tool_call/tool_result/thinking/message/...)."},
        {"name": "--since", "value": "<seq>", "description": "For stream: only events with seq > N."},
        {"name": "--summary", "description": "For stream: show summary view."},
        {"name": "--notes", "value": "<text>", "description": "For info: supervisor notes."},
        {"name": "--goal", "value": "<text>", "description": "For info: supervisor goal."}
      ]
    },
    {
      "name": "help",
      "description": "Show command usage.",
      "usage": "pcder help [--json]",
      "options": []
    },
    {
      "name": "version",
      "description": "Show the pcder version.",
      "usage": "pcder version [--json]",
      "options": []
    }
  ],
  "completion": {
    "description": "Use --wait to keep pcder.exe alive until the managed process exits. The .done file is written atomically on exit. Without --wait, start returns immediately.",
    "blocking_start": "pcder.exe start ... --wait --wait-ms 0",
    "blocking_wait": "pcder.exe wait --id <id> --wait-ms 0",
    "wait_option": "--wait",
    "wait_ms_option": "--wait-ms <ms>; 0 means unlimited with --wait",
    "exit_codes": {
      "0": "completed",
      "child_exit_code": "failed",
      "124": "wait timed out while the process is still running"
    },
    "done_file": "JSON file written on process exit with state, exit_code, reason, log_file, and done_file"
  },
  "setup": {
    "cli": {
      "recommended_binary": "pcder.exe",
      "description": "Use pcder.exe for command-line control. It locates and auto-starts the ProcessCommander GUI dashboard if needed.",
      "invocation": "pcder.exe [--json] <command> [options]",
      "help_command": "pcder.exe help --json"
    },
    "mcp": {
      "description": "Register ProcessCommander.exe as an MCP stdio server so LLM clients can call its tools directly.",
      "transport": "stdio",
      "config_files": [".mcp.json", "claude_desktop_config.json"],
      "command": ")" << guiExe << R"(",
      "args": ["mcp"],
      "auto_install": {
        "description": "Writes or updates the MCP server entry automatically. Recommended for LLM agents.",
        "argv": [")" << guiExe << R"(", "mcp", "install"],
        "command": ")" << guiExe << R"( mcp install",
        "clients": {
          "claude": {"default_path": ".mcp.json", "container": "mcpServers"},
          "opencode": {"default_path": "opencode.json", "container": "mcp"}
        }
      },
      "config_example": {
        "mcpServers": {
          "process-commander": {
            "command": ")" << guiExe << R"(",
            "args": ["mcp"]
          }
        }
      },
      "tools": ["start_process", "list_processes", "get_process_output", "send_process_input", "wait_process", "kill_process", "run_worker", "wait_worker", "get_agent_stream", "get_agent_state", "update_agent_info", "taskqueue_add", "taskqueue_list", "taskqueue_get", "taskqueue_retry", "taskqueue_update", "taskqueue_cancel", "taskqueue_pause", "taskqueue_resume", "taskqueue_wait", "taskqueue_purge", "taskqueue_link", "taskqueue_unlink", "taskqueue_graph"]
    }
  },
  "agent": {
    "purpose": "Reliable execution of CLI agents and build jobs: one OS command per job with timeouts, tree kill, logs, completion markers, and a persistent dependency-aware task queue.",
    "llm_stream_setup": {
      "description": "To see live agent state/stream in the GUI and parsed events, enable JSONL streaming for LLM CLIs.",
      "pcder_start_example": "pcder start --name opencode --cmd \"opencode run ...\" --json-stream --agent-type opencode --wait --wait-ms 0 --json",
      "notes": [
        "ProcessCommander auto-detects common agents (claude, agy/antigravity, opencode, codex, aider, ollama) and enables JSON stream parsing if detected.",
        "Parsed events are written to <logfile>.stream.jsonl. Use GUI context menu 'LLM JSON Stream Events...' or 'pcder agent stream <id>'.",
        "Model is captured from stream fields (model/modelId/llm.model/metadata.model) when present; hover the agent icon in the stream dialog to see it.",
        "Format reference: doc/LLM-jsonstream-format.md"
      ]
    },
    "working_model": {
      "summary": "A supervising LLM uses ProcessCommander as its execution layer to run child-process worker LLMs/CLIs as managed jobs.",
      "roles": {
        "supervisor": "The LLM the user talks to. Plans work, declares jobs and dependencies, observes results and retries. It does not run the work itself, chain commands or fabricate success.",
        "process_commander": "Owns processes: timeouts, tree kill, logs, completion markers, persistence and retries.",
        "worker": "A child process, often another LLM CLI such as opencode or claude, that performs one job command and writes artifacts and logs."
      },
      "flow": [
        "The supervisor decides the steps and the success condition for each step.",
        "The supervisor registers one job per step with summary and timeoutSec, using dependsOn for order.",
        "ProcessCommander launches workers as managed processes when dependencies and slots allow.",
        "Workers write files and logs; ProcessCommander verifies exit code + filesNewer/outputRegex.",
        "The supervisor observes with taskqueue_wait, taskqueue_list and taskqueue_graph, and reads logs and artifacts.",
        "After fixing a failure the supervisor calls taskqueue_retry; blocked dependents resume automatically."
      ],
      "rules": [
        "One job = one command; never use &&, ||, &, PowerShell ';', newlines or wait loops.",
        "Give each job its own timeoutSec; never sum timeouts.",
        "Prefer filesNewer/outputRegex when exit code 0 does not prove the work was done.",
        "Keep the supervisor context small: pass detailed instructions to workers and read artifacts, not raw transcripts.",
        "Use taskqueue_wait instead of polling .done files.",
        "To delegate one task to a worker LLM, use run_worker: it blocks until the worker exits and returns only the final result; the raw stream stays in stream_file.",
        "Harnesses do not see ProcessCommander as a native sub-agent. To have it shown as one, define a thin custom agent (Claude Code: .claude/agents/<name>.md, cheap model, this tool only) that calls run_worker and returns the result. The wrapper costs extra tokens; calling run_worker directly is cheapest.",
        "Never force success; ProcessCommander verifies it."
      ],
      "example": {
        "note": "A supervisor registers worker jobs; each worker is an ordinary process, often an LLM CLI, that writes artifacts.",
        "jobs": [
          {"id": "research", "command": "tools\\run-worker.cmd {instructions} {summary}", "summary": "Research and write artifacts/research.md", "timeoutSec": 1800, "instructionsFile": "tasks/research.md", "success": {"filesNewer": ["artifacts/research.md"]}},
          {"id": "build", "command": "tools\\run-worker.cmd {instructions} {summary}", "summary": "Build using the research", "timeoutSec": 3600, "dependsOn": ["research"], "instructionsFile": "tasks/build.md", "success": {"filesNewer": ["dist/app.exe"]}}
        ]
      }
    },
    "preconditions": [
      "Windows 10/11 x64; same user session as the ProcessCommander dashboard",
      "The dashboard auto-starts on first use; administrator rights are not required",
      "State, logs and task queues live under the logs directory. Override it via PROCESS_COMMANDER_LOG_DIR.",
      "One dashboard instance per Windows session owns the Named Pipe server",
      "A job is exactly one command; shell chains and polling loops are rejected"
    ],
    "cli": {
      "recommended_binary": "pcder.exe",
      "legacy": "ProcessCommander.exe <command> has the same implementation",
      "json_mode": "Add --json or -j (before or after the command) for one JSON object.",
      "data_dir": "Pass --data-dir <path> on each pcder invocation (or embed it with pcder mcp install --data-dir <path>) to keep logs/state/task queues inside the current project. Each data dir gets its own dashboard instance, so different projects can run concurrently.",
      "auto_start": "pcder.exe auto-starts the GUI dashboard when it is not running."
    },
    "register_mcp": {
      "command": "pcder.exe mcp install",
      "opencode": "pcder.exe mcp install --client opencode",
      "preview": "pcder.exe mcp install --dry-run --json",
      "note": "Run once per project/client; restart or reload the LLM client afterwards."
    },
    "decision": {
      "single_command": "start_process",
      "multiple_commands_or_dependencies": "taskqueue_add",
      "never": ["&&", "||", "single &", "powershell ;", "newline chains", "polling loops such as Test-Path or goto"],
      "reason": "One job = one command, so each job keeps its own timeout, success check and retry; chains are rejected."
    },
    "taskqueue_job": {
      "required": ["id", "command", "summary", "timeoutSec"],
      "optional": ["cwd", "env", "dependsOn", "idle", "maxAttempts", "retryDelaySec", "defer", "success", "killExclude", "resources", "priority", "instructionsFile", "instructionsText"],
      "summary": "One line, max 200 chars; exported as PC_JOB_SUMMARY and substituted for {summary}.",
      "timeoutSec": "Per job; > 0 and <= queue maxTimeoutSec (default 7200). Never summed.",
      "success": "exitCode (default 0) AND filesNewer (exist and newer than the attempt start) AND outputRegex (last 200 log lines).",
      "idle": "outputSec and cpuSec must both be set; kill only when output AND CPU are idle.",
      "defer": "{exitCodes (default [75]), delaySec (60), limit (60, 0 = no limit)}: exit codes meaning nothing ran; the job is re-run after delaySec without using up maxAttempts. {\"exitCodes\": []} turns it off."
    },
    "states": {
      "waiting": "Queued and ready; waiting for a free slot or the retry delay.",
      "pending": "Not in the run queue yet; dependencies are not satisfied.",
      "blocked": "A success-dependency failed/cancelled; fix and retry the dependency.",
      "running": "An attempt is executing.",
      "succeeded": "Success conditions met.",
      "failed": "Attempts exhausted or launch failed.",
      "cancelled": "Cancelled; not retried."
    },
    "quick_start": [
      {"step": "register_mcp", "command": "pcder.exe mcp install"},
      {"step": "add_jobs", "tool": "taskqueue_add", "args": {"taskqueue": "main", "jobs": [{"id": "A", "command": "run A", "summary": "Prepare inputs", "timeoutSec": 1800}, {"id": "B", "command": "run B", "summary": "Build outputs", "timeoutSec": 3600, "dependsOn": ["A"]}]}},
      {"step": "observe", "tool": "taskqueue_wait", "args": {"taskqueue": "main", "sinceVersion": 0, "timeoutMs": 30000}},
      {"step": "inspect", "tools": ["taskqueue_list", "taskqueue_graph", "taskqueue_get"]},
      {"step": "recover", "tool": "taskqueue_retry", "args": {"id": "A", "refreshInstructions": true}}
    ],
    "environment_variables": {
      "PC_JOB_ID": "Job id",
      "PC_ATTEMPT": "1-based attempt number",
      "PC_JOB_SUMMARY": "Job summary",
      "PC_JOB_INSTRUCTIONS": "Path to the materialized instructions revision",
      "PC_JOB_INSTRUCTIONS_REV": "Instructions revision",
      "PC_PREV_REASON": "Previous attempt reason for retries",
      "PC_PREV_EXIT": "Previous exit code for retries",
      "PC_PREV_TAIL": "Path to the previous attempt's last 50 log lines"
    },
    "data_dir_consistency": {
      "problem": "--data-dir is per invocation, so different calls could accidentally use different directories.",
      "guarantee": [
        "Choose one project-local directory, for example <project>/.processcommander.",
        "Register the MCP server once with it: pcder.exe mcp install --data-dir .processcommander. This embeds --data-dir into the project's MCP config, so every MCP tool call for that project uses the same directory.",
        "Pass the same --data-dir on every direct pcder invocation.",
        "Relative paths resolve against the caller's current directory; use an absolute path in the MCP config if the cwd is uncertain.",
        "Different data directories are isolated: each has its own dashboard instance, Named Pipe and mutex, and they never share processes or queues."
      ],
    "verify": "pcder.exe --data-dir <path> help --json returns effective_data_dir; the dashboard status bar shows the same path.",
    "llm_stream": {
      "help": "Run 'pcder help --json' and look under agent.llm_stream_setup for instructions to enable JSONL streaming and view the stream window."
    }
  },
    "notes": [
      "Use filesNewer when exit code 0 does not prove the work was done.",
      "Use dependsOn instead of waiting for .done files or polling.",
      "taskqueue_wait is a long poll; it returns when the queue version changes.",
      "kill_process on a task-queue attempt means 'killed attempt' and is retried (use taskqueue_cancel to stop it).",
      "The project may change between calls: pass --data-dir <project>/.processcommander on every pcder invocation, or embed it once with pcder mcp install --data-dir .processcommander."
    ]
  },
  "notes": [
    "The GUI dashboard (ProcessCommander.exe) is started automatically if it is not already running.",
    "Process IDs are shown in the dashboard and returned by the start and list commands.",
    "With --json, every response is a single JSON object. Errors have \"ok\": false and set exit code 1.",
    "Run 'pcder help --json' at any time to retrieve this machine-readable usage and setup information."
  ]
})" << "\n";
}

bool FindJsonValuePos(const std::string& json, const std::string& key, size_t& outPos) {
    const std::string pattern = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = json.find(pattern, pos)) != std::string::npos) {
        size_t afterKey = pos + pattern.size();
        while (afterKey < json.size() && std::isspace(static_cast<unsigned char>(json[afterKey]))) {
            afterKey++;
        }
        if (afterKey < json.size() && json[afterKey] == ':') {
            afterKey++;
            while (afterKey < json.size() && std::isspace(static_cast<unsigned char>(json[afterKey]))) {
                afterKey++;
            }
            outPos = afterKey;
            return true;
        }
        pos += pattern.size();
    }
    return false;
}

std::string ExtractJsonObject(const std::string& json, const std::string& key) {
    size_t pos = 0;
    if (!FindJsonValuePos(json, key, pos) || pos >= json.size() || json[pos] != '{') {
        return "";
    }

    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (size_t i = pos; i < json.size(); ++i) {
        char c = json[i];
        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) {
                return json.substr(pos, i - pos + 1);
            }
        }
    }
    return "";
}

std::vector<std::string> ExtractJsonObjectArray(const std::string& json, const std::string& key) {
    std::vector<std::string> objects;
    size_t pos = 0;
    if (!FindJsonValuePos(json, key, pos) || pos >= json.size() || json[pos] != '[') {
        return objects;
    }

    size_t i = pos + 1;
    while (i < json.size()) {
        while (i < json.size() && (std::isspace(static_cast<unsigned char>(json[i])) || json[i] == ',')) {
            i++;
        }
        if (i >= json.size() || json[i] == ']') break;
        if (json[i] != '{') {
            i++;
            continue;
        }

        size_t start = i;
        int depth = 0;
        bool inString = false;
        bool escaped = false;
        for (; i < json.size(); ++i) {
            char c = json[i];
            if (inString) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') inString = false;
                continue;
            }
            if (c == '"') {
                inString = true;
            } else if (c == '{') {
                depth++;
            } else if (c == '}') {
                depth--;
                if (depth == 0) {
                    objects.push_back(json.substr(start, i - start + 1));
                    i++;
                    break;
                }
            }
        }
    }
    return objects;
}

std::string UpperAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

std::string ProcessStateLabel(const std::string& procJson) {
    std::string state = UpperAscii(JsonGetString(procJson, "state", "unknown"));
    if (state == "RUNNING" && JsonGetInt(procJson, "idle_ms", 0) >= 10000) {
        state = "IDLE";
    }
    int exitCode = static_cast<int>(JsonGetInt(procJson, "exit_code", STILL_ACTIVE));
    if (state != "RUNNING" && state != "IDLE" && exitCode != STILL_ACTIVE) {
        state += " exit=" + std::to_string(exitCode);
    }
    return state;
}

void PrintProcessLine(const std::string& procJson, const std::string& indent = "  ") {
    std::cout << indent << "#" << JsonGetInt(procJson, "id", 0);
    std::string name = JsonGetString(procJson, "name", "");
    if (!name.empty()) std::cout << " " << name;
    std::cout << " [" << ProcessStateLabel(procJson) << "]"
              << " pid=" << JsonGetInt(procJson, "pid", 0)
              << " procs=" << JsonGetInt(procJson, "tree_procs", 0)
              << " mode=" << JsonGetString(procJson, "mode", "")
              << " uptime=" << JsonGetString(procJson, "uptime_str", "")
              << " cpu=" << JsonGetString(procJson, "cpu_time_str", "")
              << " mem=" << JsonGetString(procJson, "memory_str", "");

    std::string state = JsonGetString(procJson, "state", "");
    if (state == "running") {
        uint64_t idleMs = static_cast<uint64_t>(JsonGetInt(procJson, "idle_ms", 0));
        std::cout << " idle=" << FormatDurationSec(idleMs);
    }
    std::string agentType = JsonGetString(procJson, "agent_type", "");
    if (!agentType.empty()) {
        std::cout << " [" << agentType << "]";
    }
    std::cout << "\n";
}

void PrintProcessDetails(const std::string& procJson, const std::string& indent = "       ") {
    std::string command = JsonGetString(procJson, "command", "");
    std::string cwd = JsonGetString(procJson, "cwd", "");
    std::string start = JsonGetString(procJson, "start_time", "");
    std::string end = JsonGetString(procJson, "end_time", "");
    std::string cpu = JsonGetString(procJson, "cpu_time_str", "");
    std::string logFile = JsonGetString(procJson, "log_file", "");
    std::string doneFile = JsonGetString(procJson, "done_file", "");
    std::string lastLine = JsonGetString(procJson, "last_line", "");

    if (!command.empty()) std::cout << indent << "cmd : " << command << "\n";
    if (!cwd.empty()) std::cout << indent << "cwd : " << cwd << "\n";
    if (!start.empty()) std::cout << indent << "start: " << start << "\n";
    if (!end.empty()) std::cout << indent << "end  : " << end << "\n";
    if (!cpu.empty()) std::cout << indent << "cpu  : " << cpu << "\n";
    if (!logFile.empty()) std::cout << indent << "log : " << logFile << "\n";
    if (!doneFile.empty()) std::cout << indent << "done: " << doneFile << "\n";
    if (!lastLine.empty()) std::cout << indent << "last: " << lastLine << "\n";

    std::string agentType = JsonGetString(procJson, "agent_type", "");
    if (!agentType.empty()) {
        std::string agentSt = JsonGetString(procJson, "agent_status", "");
        std::cout << indent << "agent: [" << agentType << "] " << (agentSt.empty() ? "active" : agentSt) << "\n";
        std::string curTool = JsonGetString(procJson, "current_tool", "");
        if (!curTool.empty()) {
            std::string curAction = JsonGetString(procJson, "current_tool_action", "");
            std::cout << indent << "tool : " << curTool;
            if (!curAction.empty()) std::cout << " (" << curAction << ")";
            std::cout << "\n";
        }
        std::string goal = JsonGetString(procJson, "supervisor_goal", "");
        if (!goal.empty()) std::cout << indent << "goal : " << goal << "\n";
    }
}

void PrintOutput(const std::string& text, const std::string& label = "Output") {
    if (text.empty()) {
        std::cout << label << ": (empty)\n";
        return;
    }
    std::cout << label << ":\n" << text;
    if (text.back() != '\n') std::cout << "\n";
}

std::string BuildTransportError(const std::string& message) {
    return "{\"ok\":false,\"error\":\"" + JsonEscape(message) + "\"}";
}

int CompletionExitCode(const std::string& procJson) {
    std::string state = JsonGetString(procJson, "state", "");
    if (state == "completed") return 0;
    if (state == "running") return 124;
    int64_t ec = JsonGetInt(procJson, "exit_code", 1);
    if (ec == 0 || ec == STILL_ACTIVE) return 1;
    return static_cast<int>(ec);
}

int PrintResponse(const std::string& action, const std::string& respJson, bool jsonOutput,
                  int requestedId = 0, bool waitForCompletion = false) {
    if (jsonOutput) {
        std::cout << respJson << "\n";
        bool ok = JsonGetBool(respJson, "ok", false);
        if (!ok) return 1;
        std::string proc = ExtractJsonObject(respJson, "process");
        if (action == "start" && waitForCompletion && !proc.empty()) {
            return CompletionExitCode(proc);
        }
        if (action == "wait") {
            if (!JsonGetBool(respJson, "finished", false)) return 124;
            if (!proc.empty()) return CompletionExitCode(proc);
        }
        return 0;
    }

    if (!JsonGetBool(respJson, "ok", false)) {
        std::cerr << "Error: " << JsonGetString(respJson, "error", "Unknown error.") << "\n";
        return 1;
    }

    if (action.rfind("taskqueue_", 0) == 0) {
        if (action == "taskqueue_list") {
            auto jobs = ExtractJsonObjectArray(respJson, "jobs");
            if (jobs.empty()) {
                std::cout << "No task queue jobs found.\n";
                return 0;
            }
            std::cout << "Task queue jobs (" << jobs.size() << "):\n\n";
            for (const auto& j : jobs) {
                std::cout << "  " << JsonGetString(j, "id", "")
                          << " [" << JsonGetString(j, "state", "") << "]"
                          << " queue=" << JsonGetString(j, "taskqueue", "")
                          << " max_attempts=" << JsonGetInt(j, "max_attempts", 0)
                          << " :: " << JsonGetString(j, "summary", "");
                std::string notes = JsonGetString(j, "notes", "");
                if (!notes.empty()) std::cout << "  notes=" << notes;
                std::string blockedBy = JsonGetString(j, "blocked_by", "");
                if (!blockedBy.empty()) std::cout << " (blocked by " << blockedBy << ")";
                std::cout << "\n";
            }
            return 0;
        }
        std::cout << respJson << "\n";
        return 0;
    }

    if (action == "list") {
        auto processes = ExtractJsonObjectArray(respJson, "processes");
        if (processes.empty()) {
            if (requestedId > 0) {
                std::cout << "Process #" << requestedId << " not found.\n";
            } else {
                std::cout << "No managed processes found.\n";
            }
            return 0;
        }
        std::cout << "Managed processes (" << processes.size() << "):\n\n";
        for (const auto& proc : processes) {
            PrintProcessLine(proc);
            PrintProcessDetails(proc);
            std::cout << "\n";
        }
        return 0;
    }

    std::string proc = ExtractJsonObject(respJson, "process");

    if (action == "start") {
        std::cout << "Started process";
        if (!proc.empty()) {
            PrintProcessLine(proc, " ");
            PrintProcessDetails(proc);
        } else {
            std::cout << "\n";
        }
        std::string logFile = JsonGetString(respJson, "log_file", "");
        if (!logFile.empty()) std::cout << "       log : " << logFile << "\n";
        PrintOutput(JsonGetString(respJson, "output", ""));
        if (waitForCompletion && !proc.empty()) {
            return CompletionExitCode(proc);
        }
        return 0;
    }

    if (action == "logs") {
        if (!proc.empty()) {
            PrintProcessLine(proc);
            PrintProcessDetails(proc);
            std::cout << "\n";
        }
        PrintOutput(JsonGetString(respJson, "output", ""));
        return 0;
    }

    if (action == "send") {
        std::cout << "Sent input to process #" << JsonGetInt(respJson, "id", 0) << ".\n";
        if (!proc.empty()) {
            PrintProcessLine(proc);
            std::cout << "\n";
        }
        PrintOutput(JsonGetString(respJson, "recent_output", ""), "Recent output");
        return 0;
    }

    if (action == "wait") {
        int id = proc.empty() ? 0 : static_cast<int>(JsonGetInt(proc, "id", 0));
        bool finished = JsonGetBool(respJson, "finished", false);
        if (finished) {
            std::cout << "Process #" << id << " finished.\n";
        } else {
            std::cout << "Process #" << id << " is still running (wait timed out).\n";
        }
        if (!proc.empty()) {
            PrintProcessLine(proc);
            PrintProcessDetails(proc);
            std::cout << "\n";
        }
        PrintOutput(JsonGetString(respJson, "output", ""));
        if (!finished) return 124;
        if (!proc.empty()) return CompletionExitCode(proc);
        return 0;
    }

    if (action == "kill") {
        std::cout << "Killed process #" << JsonGetInt(respJson, "killed_id", 0) << ".\n";
        return 0;
    }

    if (action == "kill_all") {
        std::cout << "Killed " << JsonGetInt(respJson, "killed_count", 0) << " running process(es).\n";
        return 0;
    }

    if (action == "clear") {
        std::cout << "Cleared " << JsonGetInt(respJson, "cleared_count", 0) << " finished process(es).\n";
        return 0;
    }

    if (action == "get_agent_stream") {
        std::string summary = JsonGetString(respJson, "summary", "");
        if (!summary.empty()) {
            std::cout << summary << "\n";
            return 0;
        }
        std::string stateObj = ExtractJsonObject(respJson, "state");
        if (!stateObj.empty()) {
            std::cout << "Agent State: [" << JsonGetString(stateObj, "agent_type", "") << "] "
                      << JsonGetString(stateObj, "status", "") << "\n";
            std::string curTool = JsonGetString(stateObj, "current_tool", "");
            if (!curTool.empty()) {
                std::cout << "  Active Tool: " << curTool << " (" << JsonGetString(stateObj, "current_tool_action", "") << ")\n";
            }
        }
        auto events = ExtractJsonObjectArray(respJson, "events");
        std::cout << "Events (" << events.size() << "):\n";
        for (const auto& ev : events) {
            std::cout << "  #" << JsonGetInt(ev, "seq", 0) << " [" << JsonGetString(ev, "type", "") << "]";
            std::string tn = JsonGetString(ev, "tool_name", "");
            if (!tn.empty()) std::cout << " tool=" << tn;
            std::string ta = JsonGetString(ev, "tool_action", "");
            if (!ta.empty()) std::cout << " (" << ta << ")";
            std::string cnt = JsonGetString(ev, "content", "");
            if (!cnt.empty()) {
                if (cnt.size() > 80) cnt = cnt.substr(0, 77) + "...";
                std::cout << " : " << cnt;
            }
            std::cout << "\n";
        }
        return 0;
    }

    if (action == "get_agent_state") {
        std::cout << "Agent #" << JsonGetInt(respJson, "id", 0)
                  << " [" << JsonGetString(respJson, "agent_type", "generic") << "] "
                  << "Status: " << JsonGetString(respJson, "status", "idle") << "\n";
        std::string curTool = JsonGetString(respJson, "current_tool", "");
        if (!curTool.empty()) {
            std::cout << "  Active Tool : " << curTool << " (" << JsonGetString(respJson, "current_tool_action", "") << ")\n";
        }
        std::string lastThought = JsonGetString(respJson, "last_thought", "");
        if (!lastThought.empty()) {
            std::cout << "  Thinking    : " << lastThought << "\n";
        }
        std::string lastMsg = JsonGetString(respJson, "last_message", "");
        if (!lastMsg.empty()) {
            std::cout << "  Message     : " << lastMsg << "\n";
        }
        std::cout << "  Steps       : " << JsonGetInt(respJson, "step_count", 0) << "\n"
                  << "  Events      : " << JsonGetInt(respJson, "event_count", 0) << "\n";
        std::string streamFile = JsonGetString(respJson, "stream_file", "");
        if (!streamFile.empty()) {
            std::cout << "  Stream File : " << streamFile << "\n";
        }
        std::string goal = JsonGetString(respJson, "supervisor_goal", "");
        if (!goal.empty()) {
            std::cout << "  Goal        : " << goal << "\n";
        }
        std::string notes = JsonGetString(respJson, "supervisor_notes", "");
        if (!notes.empty()) {
            std::cout << "  Notes       : " << notes << "\n";
        }
        return 0;
    }

    if (action == "update_agent_info") {
        std::cout << "Updated supervisor info for agent #" << JsonGetInt(respJson, "id", 0) << ".\n";
        return 0;
    }

    std::cout << respJson << "\n";
    return 0;
}

} // namespace

int RunCli(const std::vector<std::string>& rawArgs) {
    std::vector<std::string> args;
    args.push_back(rawArgs.empty() ? "pcder" : rawArgs[0]);

    bool jsonOutput = false;
    bool waitForCompletion = false;
    std::string dataDirArg;
    for (size_t i = 1; i < rawArgs.size(); ++i) {
        if (rawArgs[i] == "--json" || rawArgs[i] == "-j") {
            jsonOutput = true;
        } else if (rawArgs[i] == "--data-dir" && i + 1 < rawArgs.size()) {
            dataDirArg = rawArgs[++i];
        } else if (rawArgs[i].rfind("--data-dir=", 0) == 0) {
            dataDirArg = rawArgs[i].substr(11);
        } else {
            args.push_back(rawArgs[i]);
        }
    }
    if (!dataDirArg.empty()) {
        SetDataDirectoryOverride(Utf8ToWide(dataDirArg));
    }

    std::string subcmd = (args.size() >= 2) ? args[1] : "";
    if (subcmd.empty()) {
        if (jsonOutput) PrintCliUsageJson();
        else PrintCliUsage();
        return 1;
    }
    if (subcmd == "--help" || subcmd == "-h" || subcmd == "help") {
        if (jsonOutput) PrintCliUsageJson();
        else PrintCliUsage();
        return 0;
    }
    if (subcmd == "--version" || subcmd == "-V" || subcmd == "version") {
        if (jsonOutput) std::cout << "{\"ok\":true,\"program\":\"pcder\",\"version\":\"" PC_VERSION "\"}" << std::endl;
        else std::cout << "pcder " PC_VERSION << std::endl;
        return 0;
    }

    std::ostringstream reqJson;
    int requestedId = 0;

    if (subcmd == "start" || subcmd == "run") {
        std::string name;
        std::string cmd;
        std::string cwd;
        std::string logFile;
        std::string doneFile;
        std::string mode = "pipe";
        bool openStdin = true;
        int timeoutSec = 0;
        int idleTimeoutSec = 0;
        std::string estimateEnd;
        std::string agentType;
        bool jsonStream = false;
        std::string supervisorNotes;
        std::string supervisorGoal;
        bool waitFinish = false;
        bool waitMsExplicit = false;
        bool allowChain = false;
        int waitMs = 500;

        for (size_t i = 2; i < args.size(); ++i) {
            const std::string& a = args[i];
            if (a == "--name" && i + 1 < args.size()) {
                name = args[++i];
            } else if (a == "--cmd" && i + 1 < args.size()) {
                cmd = args[++i];
            } else if (a == "--cwd" && i + 1 < args.size()) {
                cwd = args[++i];
            } else if ((a == "--log-file" || a == "--log") && i + 1 < args.size()) {
                logFile = args[++i];
            } else if ((a == "--done-file" || a == "--done") && i + 1 < args.size()) {
                doneFile = args[++i];
            } else if (a == "--mode" && i + 1 < args.size()) {
                mode = args[++i];
            } else if (a == "--pty") {
                mode = "pty";
            } else if (a == "--open-stdin") {
                openStdin = true;
            } else if (a == "--close-stdin") {
                openStdin = false;
            } else if (a == "--timeout" && i + 1 < args.size()) {
                timeoutSec = std::stoi(args[++i]);
            } else if (a == "--idle-timeout" && i + 1 < args.size()) {
                idleTimeoutSec = std::stoi(args[++i]);
            } else if ((a == "--eta" || a == "--estimate-end") && i + 1 < args.size()) {
                estimateEnd = args[++i];
            } else if (a == "--agent" && i + 1 < args.size()) {
                agentType = args[++i];
            } else if (a == "--stream") {
                jsonStream = true;
            } else if (a == "--notes" && i + 1 < args.size()) {
                supervisorNotes = args[++i];
            } else if (a == "--goal" && i + 1 < args.size()) {
                supervisorGoal = args[++i];
            } else if (a == "--wait") {
                waitFinish = true;
                waitForCompletion = true;
            } else if (a == "--allow-chain") {
                allowChain = true;
            } else if (a == "--wait-ms" && i + 1 < args.size()) {
                waitMs = std::stoi(args[++i]);
                waitMsExplicit = true;
            } else if (a == "--") {
                std::ostringstream cmdOss;
                for (size_t j = i + 1; j < args.size(); ++j) {
                    if (j > i + 1) cmdOss << " ";
                    if (args[j].find(' ') != std::string::npos) {
                        cmdOss << "\"" << args[j] << "\"";
                    } else {
                        cmdOss << args[j];
                    }
                }
                cmd = cmdOss.str();
                break;
            } else if (cmd.empty() && a[0] != '-') {
                cmd = a;
            }
        }

        if (cmd.empty()) {
            if (jsonOutput) {
                std::cout << "{\"ok\":false,\"error\":\"Missing --cmd or command arguments.\"}\n";
            } else {
                std::cerr << "Error: Missing --cmd or command arguments.\n";
            }
            return 1;
        }

        cwd = ResolveAbsolutePathUtf8(cwd);
        if (waitFinish && !waitMsExplicit) {
            waitMs = 0;
        }

        reqJson << "{\"action\":\"start\""
                << ",\"name\":\"" << JsonEscape(name) << "\""
                << ",\"cmd\":\"" << JsonEscape(cmd) << "\""
                << ",\"cwd\":\"" << JsonEscape(cwd) << "\""
                << ",\"log_file\":\"" << JsonEscape(logFile) << "\""
                << ",\"done_file\":\"" << JsonEscape(doneFile) << "\""
                << ",\"mode\":\"" << JsonEscape(mode) << "\""
                << ",\"open_stdin\":" << (openStdin ? "true" : "false")
                << ",\"timeout_sec\":" << timeoutSec
                << ",\"idle_timeout_sec\":" << idleTimeoutSec
                << ",\"estimate_end\":\"" << JsonEscape(estimateEnd) << "\""
                << ",\"agent_type\":\"" << JsonEscape(agentType) << "\""
                << ",\"json_stream\":" << (jsonStream ? "true" : "false")
                << ",\"supervisor_notes\":\"" << JsonEscape(supervisorNotes) << "\""
                << ",\"supervisor_goal\":\"" << JsonEscape(supervisorGoal) << "\""
                << ",\"wait\":" << (waitFinish ? "true" : "false")
                << ",\"wait_ms\":" << waitMs
                << ",\"allow_chain\":" << (allowChain ? "true" : "false") << "}";
    }
    else if (subcmd == "list" || subcmd == "status") {
        int id = 0;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--id" && i + 1 < args.size()) id = std::stoi(args[++i]);
            else if (id == 0 && args[i][0] != '-') { try { id = std::stoi(args[i]); } catch (...) {} }
        }
        reqJson << "{\"action\":\"list\",\"id\":" << id << "}";
        requestedId = id;
        subcmd = "list";
    }
    else if (subcmd == "logs" || subcmd == "output") {
        int id = 0;
        int tail = 0;
        int waitMs = 0;
        bool raw = false;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--id" && i + 1 < args.size()) id = std::stoi(args[++i]);
            else if (args[i] == "--tail" && i + 1 < args.size()) tail = std::stoi(args[++i]);
            else if (args[i] == "--wait-ms" && i + 1 < args.size()) waitMs = std::stoi(args[++i]);
            else if (args[i] == "--raw") raw = true;
            else if (id == 0 && args[i][0] != '-') { try { id = std::stoi(args[i]); } catch (...) {} }
        }
        reqJson << "{\"action\":\"logs\",\"id\":" << id
                << ",\"tail\":" << tail
                << ",\"wait_ms\":" << waitMs
                << ",\"raw\":" << (raw ? "true" : "false") << "}";
        requestedId = id;
        subcmd = "logs";
    }
    else if (subcmd == "send" || subcmd == "write") {
        int id = 0;
        std::string input;
        bool newline = true;
        int waitMs = 400;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--id" && i + 1 < args.size()) id = std::stoi(args[++i]);
            else if (args[i] == "--input" && i + 1 < args.size()) input = args[++i];
            else if (args[i] == "--no-newline") newline = false;
            else if (args[i] == "--wait-ms" && i + 1 < args.size()) waitMs = std::stoi(args[++i]);
            else if (id == 0 && args[i][0] != '-') { try { id = std::stoi(args[i]); } catch (...) {} }
            else if (input.empty() && args[i][0] != '-') input = args[i];
        }
        reqJson << "{\"action\":\"send\",\"id\":" << id
                << ",\"input\":\"" << JsonEscape(input) << "\""
                << ",\"newline\":" << (newline ? "true" : "false")
                << ",\"wait_ms\":" << waitMs << "}";
        requestedId = id;
        subcmd = "send";
    }
    else if (subcmd == "wait") {
        int id = 0;
        int waitMs = 30000;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--id" && i + 1 < args.size()) id = std::stoi(args[++i]);
            else if (args[i] == "--wait-ms" && i + 1 < args.size()) waitMs = std::stoi(args[++i]);
            else if (id == 0 && args[i][0] != '-') { try { id = std::stoi(args[i]); } catch (...) {} }
        }
        reqJson << "{\"action\":\"wait\",\"id\":" << id << ",\"wait_ms\":" << waitMs << "}";
        requestedId = id;
    }
    else if (subcmd == "kill") {
        int id = 0;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--id" && i + 1 < args.size()) id = std::stoi(args[++i]);
            else if (id == 0 && args[i][0] != '-') { try { id = std::stoi(args[i]); } catch (...) {} }
        }
        reqJson << "{\"action\":\"kill\",\"id\":" << id << "}";
        requestedId = id;
    }
    else if (subcmd == "kill-all") {
        reqJson << "{\"action\":\"kill_all\"}";
        subcmd = "kill_all";
    }
    else if (subcmd == "clear") {
        reqJson << "{\"action\":\"clear\"}";
    }
    else if (subcmd == "mcp") {
        std::string op = (args.size() >= 3) ? args[2] : "";
        if (op != "install") {
            std::string msg = "usage: pcder mcp install [--client claude|opencode] [--path <file>] [--name <name>] [--dry-run]";
            if (jsonOutput) std::cout << "{\"ok\":false,\"error\":\"" << JsonEscape(msg) << "\"}\n";
            else std::cerr << msg << "\n";
            return 1;
        }
        std::string client = "claude";
        std::string installPath;
        std::string serverName = "process-commander";
        bool dryRun = false;
        for (size_t i = 3; i < args.size(); ++i) {
            if ((args[i] == "--client" || args[i] == "-c") && i + 1 < args.size()) client = args[++i];
            else if ((args[i] == "--path" || args[i] == "-p") && i + 1 < args.size()) installPath = args[++i];
            else if (args[i] == "--name" && i + 1 < args.size()) serverName = args[++i];
            else if (args[i] == "--dry-run") dryRun = true;
        }
        if (client != "claude" && client != "opencode") {
            std::string msg = "unknown client '" + client + "' (expected claude or opencode)";
            if (jsonOutput) std::cout << "{\"ok\":false,\"error\":\"" << JsonEscape(msg) << "\"}\n";
            else std::cerr << "Error: " << msg << "\n";
            return 1;
        }
        McpInstallOutcome outcome = InstallMcpConfig(client, installPath, serverName, dryRun,
                                                     ResolveGuiExePathUtf8(), dataDirArg);
        if (!outcome.ok) {
            if (jsonOutput) {
                std::cout << "{\"ok\":false,\"error\":\"" << JsonEscape(outcome.error) << "\"}\n";
            } else {
                std::cerr << "Error: " << outcome.error << "\n";
            }
            return 1;
        }
        if (jsonOutput) {
            std::cout << "{\"ok\":true"
                      << ",\"client\":\"" << JsonEscape(client) << "\""
                      << ",\"path\":\"" << JsonEscape(outcome.targetPath) << "\""
                      << ",\"server\":\"" << JsonEscape(serverName) << "\""
                      << ",\"changed\":" << (outcome.changed ? "true" : "false")
                      << ",\"dry_run\":" << (outcome.dryRun ? "true" : "false")
                      << ",\"entry\":" << outcome.entryJson
                      << "}\n";
        } else {
            if (outcome.dryRun) {
                std::cout << "[dry-run] Would " << (outcome.changed ? "register" : "keep")
                          << " MCP server '" << serverName << "' in " << outcome.targetPath << "\n";
            } else if (outcome.changed) {
                std::cout << "Registered MCP server '" << serverName << "' in " << outcome.targetPath << "\n";
            } else {
                std::cout << "MCP server '" << serverName << "' is already registered in " << outcome.targetPath << "\n";
            }
            std::cout << "Entry: " << outcome.entryJson << "\n";
            if (outcome.changed && !outcome.dryRun) {
                std::cout << "Restart your LLM client (or reload its MCP servers) to pick it up.\n";
            }
        }
        return 0;
    }
    else if (subcmd == "taskqueue") {
        std::string op = (args.size() >= 3) ? args[2] : "";
        auto argVal = [&args](const std::string& flag, const std::string& def) -> std::string {
            for (size_t i = 3; i + 1 < args.size(); ++i) {
                if (args[i] == flag) return args[i + 1];
            }
            return def;
        };
        auto firstPositional = [&args](size_t startIdx) -> std::string {
            for (size_t i = startIdx; i < args.size(); ++i) {
                if (!args[i].empty() && args[i][0] != '-') return args[i];
            }
            return "";
        };
        auto failWith = [&](const std::string& msg) {
            if (jsonOutput) std::cout << "{\"ok\":false,\"error\":\"" << JsonEscape(msg) << "\"}\n";
            else std::cerr << "Error: " << msg << "\n";
            return 1;
        };

        if (op == "add") {
            std::string file = argVal("--file", "");
            std::string tqName = argVal("--taskqueue", "main");
            int concurrency = 0;
            std::string concStr = argVal("--concurrency", "");
            if (!concStr.empty()) {
                try { concurrency = std::stoi(concStr); } catch (...) { concurrency = 0; }
            }
            std::string notifyFile = argVal("--notify-file", "");
            if (file.empty()) return failWith("--file is required.");
            std::string content = ReadFileUtf8Wide(ResolveAbsolutePathWide(Utf8ToWide(file)));
            if (content.empty()) return failWith("cannot read jobs file: " + file);

            std::string jobsRaw;
            size_t first = content.find_first_not_of(" \t\r\n");
            if (first != std::string::npos && content[first] == '[') {
                jobsRaw = content.substr(first);
            } else {
                auto objs = ExtractJsonObjectArray(content, "jobs");
                if (objs.empty()) return failWith("jobs array not found in " + file);
                std::ostringstream ja;
                ja << "[";
                for (size_t k = 0; k < objs.size(); ++k) {
                    if (k) ja << ",";
                    ja << objs[k];
                }
                ja << "]";
                jobsRaw = ja.str();
                if (notifyFile.empty()) notifyFile = JsonGetString(content, "notify_file", "");
                if (tqName == "main") {
                    std::string t = JsonGetString(content, "taskqueue", "");
                    if (!t.empty()) tqName = t;
                }
                if (concurrency == 0) concurrency = static_cast<int>(JsonGetInt(content, "concurrency", 0));
            }
            reqJson << "{\"action\":\"taskqueue_add\",\"taskqueue\":\"" << JsonEscape(tqName) << "\""
                    << ",\"jobs\":" << jobsRaw
                    << ",\"concurrency\":" << concurrency
                    << ",\"notify_file\":\"" << JsonEscape(notifyFile) << "\"}";
            subcmd = "taskqueue_add";
        } else if (op == "list") {
            std::string tqName = firstPositional(3);
            std::string state = argVal("--state", "");
            reqJson << "{\"action\":\"taskqueue_list\",\"taskqueue\":\"" << JsonEscape(tqName)
                    << "\",\"state\":\"" << JsonEscape(state) << "\"}";
            subcmd = "taskqueue_list";
        } else if (op == "get") {
            std::string id = firstPositional(3);
            int rev = 0;
            std::string revStr = argVal("--rev", "");
            if (!revStr.empty()) { try { rev = std::stoi(revStr); } catch (...) {} }
            bool includeInstr = true;
            for (size_t i = 3; i < args.size(); ++i) {
                if (args[i] == "--no-instructions") includeInstr = false;
            }
            reqJson << "{\"action\":\"taskqueue_get\",\"id\":\"" << JsonEscape(id)
                    << "\",\"include_instructions\":" << (includeInstr ? "true" : "false")
                    << ",\"rev\":" << rev << "}";
            subcmd = "taskqueue_get";
        } else if (op == "retry") {
            std::string id = firstPositional(3);
            reqJson << "{\"action\":\"taskqueue_retry\",\"id\":\"" << JsonEscape(id)
                    << "\",\"reset_attempts\":true,\"refresh_instructions\":true}";
            subcmd = "taskqueue_retry";
        } else if (op == "cancel") {
            std::string id = firstPositional(3);
            reqJson << "{\"action\":\"taskqueue_cancel\",\"id\":\"" << JsonEscape(id) << "\"}";
            subcmd = "taskqueue_cancel";
        } else if (op == "pause" || op == "resume") {
            std::string tqName = firstPositional(3);
            reqJson << "{\"action\":\"taskqueue_" << op << "\",\"taskqueue\":\"" << JsonEscape(tqName) << "\"}";
            subcmd = "taskqueue_" + op;
        } else if (op == "graph") {
            std::string tqName = firstPositional(3);
            std::string root = argVal("--root", "");
            reqJson << "{\"action\":\"taskqueue_graph\",\"taskqueue\":\"" << JsonEscape(tqName)
                    << "\",\"root\":\"" << JsonEscape(root) << "\"}";
            subcmd = "taskqueue_graph";
        } else if (op == "wait") {
            std::string tqName = firstPositional(3);
            int since = 0;
            int timeoutMs = 30000;
            std::string sinceStr = argVal("--since", "");
            if (!sinceStr.empty()) { try { since = std::stoi(sinceStr); } catch (...) {} }
            std::string tmoStr = argVal("--timeout-ms", "");
            if (!tmoStr.empty()) { try { timeoutMs = std::stoi(tmoStr); } catch (...) {} }
            reqJson << "{\"action\":\"taskqueue_wait\",\"taskqueue\":\"" << JsonEscape(tqName)
                    << "\",\"since_version\":" << since << ",\"timeout_ms\":" << timeoutMs << "}";
            subcmd = "taskqueue_wait";
        } else if (op == "purge") {
            std::string tqName = firstPositional(3);
            std::string statesStr = argVal("--states", "succeeded,failed,cancelled");
            std::ostringstream ja;
            ja << "[";
            size_t pos = 0;
            bool firstState = true;
            while (pos <= statesStr.size()) {
                size_t comma = statesStr.find(',', pos);
                std::string st = statesStr.substr(pos, (comma == std::string::npos) ? std::string::npos : comma - pos);
                if (!st.empty()) {
                    if (!firstState) ja << ",";
                    firstState = false;
                    ja << "\"" << JsonEscape(st) << "\"";
                }
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            ja << "]";
            reqJson << "{\"action\":\"taskqueue_purge\",\"taskqueue\":\"" << JsonEscape(tqName)
                    << "\",\"states\":" << ja.str() << "}";
            subcmd = "taskqueue_purge";
        } else {
            return failWith("unknown taskqueue command: " + op +
                            " (add, list, get, retry, cancel, pause, resume, graph, wait, purge)");
        }
    }
    else if (subcmd == "agent") {
        std::string op = (args.size() >= 3 && args[2][0] != '-') ? args[2] : "status";
        size_t startIdx = (args.size() >= 3 && args[2][0] != '-') ? 3 : 2;
        int id = 0;
        int tail = 50;
        std::string typeFilter;
        uint64_t sinceSeq = 0;
        std::string format = "json";
        std::string notes;
        std::string goal;

        for (size_t i = startIdx; i < args.size(); ++i) {
            const std::string& a = args[i];
            if (a == "--id" && i + 1 < args.size()) {
                id = std::stoi(args[++i]);
            } else if (a == "--tail" && i + 1 < args.size()) {
                tail = std::stoi(args[++i]);
            } else if (a == "--type" && i + 1 < args.size()) {
                typeFilter = args[++i];
            } else if (a == "--since" && i + 1 < args.size()) {
                sinceSeq = static_cast<uint64_t>(std::stoull(args[++i]));
            } else if (a == "--summary") {
                format = "summary";
            } else if (a == "--format" && i + 1 < args.size()) {
                format = args[++i];
            } else if (a == "--notes" && i + 1 < args.size()) {
                notes = args[++i];
            } else if (a == "--goal" && i + 1 < args.size()) {
                goal = args[++i];
            } else if (id == 0 && a[0] != '-') {
                try { id = std::stoi(a); } catch (...) {}
            }
        }

        if (id <= 0) {
            if (jsonOutput) {
                std::cout << "{\"ok\":false,\"error\":\"Process ID required for agent command (e.g. pcder agent status 1).\"}\n";
            } else {
                std::cerr << "Error: Process ID required for agent command (e.g. pcder agent status 1).\n";
            }
            return 1;
        }

        requestedId = id;
        if (op == "status" || op == "state") {
            reqJson << "{\"action\":\"get_agent_state\",\"id\":" << id << "}";
            subcmd = "get_agent_state";
        } else if (op == "stream" || op == "events") {
            reqJson << "{\"action\":\"get_agent_stream\",\"id\":" << id
                    << ",\"tail\":" << tail
                    << ",\"type\":\"" << JsonEscape(typeFilter) << "\""
                    << ",\"since_seq\":" << sinceSeq
                    << ",\"format\":\"" << JsonEscape(format) << "\"}";
            subcmd = "get_agent_stream";
        } else if (op == "info" || op == "update") {
            reqJson << "{\"action\":\"update_agent_info\",\"id\":" << id
                    << ",\"supervisor_notes\":\"" << JsonEscape(notes) << "\""
                    << ",\"supervisor_goal\":\"" << JsonEscape(goal) << "\"}";
            subcmd = "update_agent_info";
        } else {
            if (jsonOutput) {
                std::cout << "{\"ok\":false,\"error\":\"Unknown agent command: " << JsonEscape(op) << " (status, stream, info)\"}\n";
            } else {
                std::cerr << "Error: Unknown agent command: " << op << " (status, stream, info)\n";
            }
            return 1;
        }
    }
    else {
        if (jsonOutput) {
            std::cout << "{\"ok\":false,\"error\":\"Unknown command: " << JsonEscape(subcmd)
                      << "\",\"hint\":\"Run 'pcder help --json' to list commands and setup instructions.\"}\n";
        } else {
            std::cerr << "Unknown command: " << subcmd << "\n\n";
            PrintCliUsage();
        }
        return 1;
    }

    std::string respJson;
    std::string err;
    if (!SendIpcRequest(reqJson.str(), respJson, err, true)) {
        if (jsonOutput) {
            std::cout << BuildTransportError(err) << "\n";
        } else {
            std::cerr << "Error: " << err << "\n";
        }
        return 1;
    }

    return PrintResponse(subcmd, respJson, jsonOutput, requestedId, waitForCompletion);
}

} // namespace pc
