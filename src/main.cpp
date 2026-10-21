#include "common.h"
#include "process_manager.h"
#include "ipc_server.h"
#include "mcp_server.h"
#include "gui_dashboard.h"
#include "cli.h"
#include <shellapi.h>
#include <iostream>
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

  ProcessCommander.exe --help
      Show this help.

CONSOLE CLIENT / CLI SETUP:
  Use pcder.exe for CLI commands (start, list, logs, send, wait, kill, kill-all, clear).
  It auto-starts the GUI dashboard and locates ProcessCommander.exe in its folder.
  Run `pcder.exe help` for human-readable usage, or `pcder.exe help --json` for
  machine-readable commands and setup instructions for LLMs.

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
  MCP tools: start_process, list_processes, get_process_output, send_process_input,
             wait_process, kill_process.
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
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--json" || args[i] == "-j") {
                return RunCli(args);
            }
        }
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
        IpcServer ipc(pm);
        ipc.Start();
        int rc = RunGuiDashboard(pm, ipc);
        ipc.Stop();
        if (hMutex) CloseHandle(hMutex);
        return rc;
    }

    // Backward-compatible console commands (prefer pcder.exe)
    return RunCli(args);
}
