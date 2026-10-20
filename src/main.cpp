#include "common.h"
#include "process_manager.h"
#include "ipc_server.h"
#include "mcp_server.h"
#include "gui_dashboard.h"
#include <shellapi.h>
#include <iostream>
#include <sstream>
#include <vector>

using namespace pc;

static void PrintUsage() {
    std::cout <<
R"(ProcessCommander - Native Win32 Multi-LLM Process & ConPTY Manager + Live GUI Dashboard

USAGE:
  ProcessCommander.exe [gui]
      Launch the live Win32 Dashboard GUI and Named Pipe server.

  ProcessCommander.exe mcp
      Run as a Model Context Protocol (MCP) stdio server for Claude.
      Automatically connects to (or launches) the Win32 GUI Dashboard.

  ProcessCommander.exe start [options] -- <command...>
      Start a managed process (e.g. claude, opencode) under a Win32 Job Object.
      Options:
        --cmd "<command>"        Command line to execute
        --name <label>           Display name on dashboard (e.g. "opencode-worker")
        --mode <pty|pipe|console>
                                 pty     = Win32 ConPTY interactive session (default for TUI)
                                 pipe    = Anonymous pipes (non-interactive stdout/stdin)
                                 console = Separate visible console window
        --cwd <dir>              Working directory
        --log-file <path>        Custom path for live output log (default: logs/proc_<id>_<name>_<ts>.log)
        --done-file <path>       Custom completion marker file created on exit (default: logs/proc_<id>.done)
        --timeout <sec>          Max runtime timeout in seconds (0 = unlimited)
        --idle-timeout <sec>     Hang timeout if no output for N seconds (0 = unlimited)
        --wait                   Block until process exits
        --wait-ms <ms>           Wait up to N ms for initial output (default: 500)

  ProcessCommander.exe list [--id <id>]
      List all managed processes and their live metrics in JSON.

  ProcessCommander.exe logs --id <id> [--tail <lines>] [--wait-ms <ms>] [--raw]
      Fetch process output logs (ANSI codes stripped by default unless --raw).

  ProcessCommander.exe send --id <id> --input "<text>" [--no-newline] [--wait-ms <ms>]
      Send input text to a running process's stdin or ConPTY session.

  ProcessCommander.exe wait --id <id> [--wait-ms <ms>]
      Wait until the specified process exits (or wait-ms elapses).

  ProcessCommander.exe kill --id <id>
      Forcefully kill the entire process tree (Job Object) for <id>.

  ProcessCommander.exe kill-all
      Forcefully kill all running managed process trees.

  ProcessCommander.exe clear
      Remove finished processes from the dashboard list.
)";
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

    std::string subcmd = (args.size() >= 2) ? args[1] : "gui";

    if (subcmd == "--help" || subcmd == "-h" || subcmd == "help") {
        PrintUsage();
        return 0;
    }

    if (subcmd == "mcp") {
        return RunMcpServer();
    }

    if (subcmd == "gui" || subcmd == "--gui") {
        HANDLE hMutex = CreateMutexW(nullptr, TRUE, kMutexName);
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND existing = FindWindowW(L"ProcessCommanderDashboardWnd", nullptr);
            if (existing) {
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
        IpcServer ipc(pm);
        ipc.Start();
        int rc = RunGuiDashboard(pm, ipc);
        ipc.Stop();
        if (hMutex) CloseHandle(hMutex);
        return rc;
    }

    // CLI Subcommands (start, list, logs, send, wait, kill, kill-all, clear)
    std::ostringstream reqJson;

    if (subcmd == "start" || subcmd == "run") {
        std::string name;
        std::string cmd;
        std::string cwd;
        std::string logFile;
        std::string doneFile;
        std::string mode = "pty";
        int timeoutSec = 0;
        int idleTimeoutSec = 0;
        bool waitFinish = false;
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
            } else if (a == "--timeout" && i + 1 < args.size()) {
                timeoutSec = std::stoi(args[++i]);
            } else if (a == "--idle-timeout" && i + 1 < args.size()) {
                idleTimeoutSec = std::stoi(args[++i]);
            } else if (a == "--wait") {
                waitFinish = true;
            } else if (a == "--wait-ms" && i + 1 < args.size()) {
                waitMs = std::stoi(args[++i]);
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
            std::cerr << "{\"ok\":false,\"error\":\"Missing --cmd or command arguments.\"}\n";
            return 1;
        }

        reqJson << "{\"action\":\"start\""
                << ",\"name\":\"" << JsonEscape(name) << "\""
                << ",\"cmd\":\"" << JsonEscape(cmd) << "\""
                << ",\"cwd\":\"" << JsonEscape(cwd) << "\""
                << ",\"log_file\":\"" << JsonEscape(logFile) << "\""
                << ",\"done_file\":\"" << JsonEscape(doneFile) << "\""
                << ",\"mode\":\"" << JsonEscape(mode) << "\""
                << ",\"timeout_sec\":" << timeoutSec
                << ",\"idle_timeout_sec\":" << idleTimeoutSec
                << ",\"wait\":" << (waitFinish ? "true" : "false")
                << ",\"wait_ms\":" << waitMs << "}";
    }
    else if (subcmd == "list" || subcmd == "status") {
        int id = 0;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--id" && i + 1 < args.size()) id = std::stoi(args[++i]);
            else if (id == 0 && args[i][0] != '-') { try { id = std::stoi(args[i]); } catch (...) {} }
        }
        reqJson << "{\"action\":\"list\",\"id\":" << id << "}";
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
    }
    else if (subcmd == "kill") {
        int id = 0;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--id" && i + 1 < args.size()) id = std::stoi(args[++i]);
            else if (id == 0 && args[i][0] != '-') { try { id = std::stoi(args[i]); } catch (...) {} }
        }
        reqJson << "{\"action\":\"kill\",\"id\":" << id << "}";
    }
    else if (subcmd == "kill-all") {
        reqJson << "{\"action\":\"kill_all\"}";
    }
    else if (subcmd == "clear") {
        reqJson << "{\"action\":\"clear\"}";
    }
    else {
        std::cerr << "Unknown command: " << subcmd << "\n";
        PrintUsage();
        return 1;
    }

    std::string respJson, err;
    if (!SendIpcRequest(reqJson.str(), respJson, err, true)) {
        std::cerr << "{\"ok\":false,\"error\":\"" << JsonEscape(err) << "\"}\n";
        return 1;
    }

    std::cout << respJson << "\n";
    return JsonGetBool(respJson, "ok", false) ? 0 : 1;
}
