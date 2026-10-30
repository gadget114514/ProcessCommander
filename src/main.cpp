#include "common.h"
#include "process_manager.h"
#include "ipc_server.h"
#include "taskqueue.h"
#include "mcp_server.h"
#include "gui_dashboard.h"
#include "cli.h"
#include <shellapi.h>
#include <iostream>
#include <vector>

using namespace pc;

static void PrintUsage() {
    std::cout <<
"ProcessCommander " PC_VERSION R"( - Native Win32 Multi-LLM Process & ConPTY Manager + Live GUI Dashboard

NAMING:
  PCR = ProcessCommander. "PCR" in docs, prompts or job notes means ProcessCommander
  (this executable); pcder.exe is its console client.

USAGE:
  ProcessCommander.exe [gui] [--data-dir <path>]
      Launch the live Win32 Dashboard GUI and Named Pipe server.

  ProcessCommander.exe quit
      Quit the running GUI, including one hidden in the system tray (its tray icon is removed).

  ProcessCommander.exe mcp [--data-dir <path>]
      Run as a Model Context Protocol (MCP) stdio server for Claude.
      Automatically connects to (or launches) the Win32 GUI Dashboard.

  ProcessCommander.exe --help
      Show this help.

  ProcessCommander.exe --version
      Show the version.

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
       Role clarity: when supervisor and worker share the same engine
       (Claude/Claude, opencode/opencode), roles can blur; prefer a different
       worker engine or a clearly distinct worker role/prompt.
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
      killExclude, resources {write,read}, priority, instructionsFile / instructionsText.
    - resources: jobs whose write/read sets overlap never run together (read/read is
      fine). Paths/globs resolve against cwd; "@name" is a named lock.
    - Success is verified: exit code AND optional filesNewer / outputRegex.
    - States: waiting = queued and ready; pending = dependencies not satisfied;
      blocked = a dependency failed; running; succeeded; failed; cancelled.
    - Observe with taskqueue_wait / taskqueue_list / taskqueue_graph; after fixing
      the cause use taskqueue_retry (blocked dependents resume automatically).

  For the full machine-readable guide (agent.quick_start, decision, states):
    pcder.exe help --json
)";
}

// Asks the running GUI to exit (same path as the tray menu's "Exit") and waits for it to go.
static int RunQuitCommand() {
    const wchar_t* kWindowClass = L"ProcessCommanderDashboardWnd";
    if (!FindWindowW(kWindowClass, nullptr)) {
        std::cout << "ProcessCommander is not running" << std::endl;
        return 1;
    }
    UINT quitMessage = RegisterWindowMessageW(L"ProcessCommanderQuit");
    PostMessageW(FindWindowW(kWindowClass, nullptr), quitMessage, 0, 0);
    for (int i = 0; i < 100 && FindWindowW(kWindowClass, nullptr); ++i) Sleep(100);
    if (FindWindowW(kWindowClass, nullptr)) {
        std::cout << "ProcessCommander did not exit within 10 seconds" << std::endl;
        return 1;
    }
    std::cout << "ProcessCommander quit" << std::endl;
    return 0;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    int argc = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        args.push_back(WideToUtf8(argvW[i]));
    }
    LocalFree(argvW);

    std::vector<std::string> filtered;
    filtered.push_back(args.empty() ? "ProcessCommander.exe" : args[0]);
    std::string dataDirArg;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--data-dir" && i + 1 < args.size()) {
            dataDirArg = args[++i];
        } else if (args[i].rfind("--data-dir=", 0) == 0) {
            dataDirArg = args[i].substr(11);
        } else {
            filtered.push_back(args[i]);
        }
    }
    args.swap(filtered);
    if (!dataDirArg.empty()) {
        SetDataDirectoryOverride(Utf8ToWide(dataDirArg));
    }

    std::string subcmd = (args.size() >= 2) ? args[1] : "gui";

    if (subcmd == "--help" || subcmd == "-h" || subcmd == "help") {
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--json" || args[i] == "-j") {
                return RunCli(args);
            }
        }
        PrintUsage();
        return 0;
    }

    // Like pcder, --json may come before or after the command.
    bool jsonOutput = false;
    std::string firstCmd;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--json" || args[i] == "-j") jsonOutput = true;
        else if (firstCmd.empty()) firstCmd = args[i];
    }
    if (firstCmd == "--version" || firstCmd == "-V" || firstCmd == "version") {
        if (jsonOutput) std::cout << "{\"ok\":true,\"program\":\"ProcessCommander\",\"version\":\"" PC_VERSION "\"}" << std::endl;
        else std::cout << "ProcessCommander " PC_VERSION << std::endl;
        return 0;
    }

    if (subcmd == "mcp") {
        return RunMcpServer();
    }

    if (subcmd == "quit" || subcmd == "--quit") {
        return RunQuitCommand();
    }

    if (subcmd == "gui" || subcmd == "--gui") {
        HANDLE hMutex = CreateMutexW(nullptr, TRUE, GetGuiMutexNameWide().c_str());
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND existing = FindWindowW(L"ProcessCommanderDashboardWnd", nullptr);
            if (existing) {
                ShowWindow(existing, SW_SHOW);
                if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
                SetForegroundWindow(existing);
            }
            if (hMutex) CloseHandle(hMutex);
            return 0;
        }

        // Hide the console window of the GUI daemon instead of calling FreeConsole().
        // Keeping an allocated (hidden) console in the parent process is required by Windows
        // so that CreateProcessW + PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE properly wires
        // StandardInput/StandardOutput/StandardError in child ConPTY processes!
        HWND hConsole = GetConsoleWindow();
        if (hConsole) {
            ShowWindow(hConsole, SW_HIDE);
        } else {
            AllocConsole();
            hConsole = GetConsoleWindow();
            if (hConsole) ShowWindow(hConsole, SW_HIDE);
        }

        ProcessManager pm;
        TaskQueueManager tqm;
        tqm.SetProcessManager(&pm);
        pm.SetProcessFinishedCallback([&tqm](int pid, int ec, const std::string& kind) {
            tqm.OnProcessFinished(pid, ec, kind);
        });
        IpcServer ipc(pm, tqm);
        ipc.Start();
        int rc = RunGuiDashboard(pm, ipc, tqm);
        ipc.Stop();
        pm.SetProcessFinishedCallback(nullptr);
        tqm.ShutdownTaskQueueManager();
        if (hMutex) CloseHandle(hMutex);
        return rc;
    }

    // Backward-compatible console commands (prefer pcder.exe)
    return RunCli(args);
}
