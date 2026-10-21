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

} // namespace

void PrintCliUsage() {
    std::cout <<
R"(pcder - ProcessCommander console client

USAGE:
  pcder [--json] <command> [options]

GLOBAL OPTIONS:
  --json, -j               Print machine-readable JSON instead of human-friendly text.
  -h, --help               Show this help.

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
        --open-stdin             Pipe mode only: keep stdin writable for pcder send.
                                 By default pipe-mode stdin is closed after launch so
                                 non-interactive programs (e.g. opencode run) see EOF.
        --cwd <dir>              Working directory
        --log-file <path>        Custom path for live output log
        --done-file <path>       Custom completion marker file created on exit
        --timeout <sec>          Max runtime timeout in seconds (0 = unlimited)
        --idle-timeout <sec>     Hang timeout if no output for N seconds (0 = unlimited)
        --wait                   Block until process exits
        --wait-ms <ms>           Wait up to N ms for initial output (default: 500)

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
"    MCP tools: start_process, list_processes, get_process_output,\n"
"               send_process_input, wait_process, kill_process.\n"
"\n"
"NOTE:\n"
"  The GUI dashboard is provided by ProcessCommander.exe, which is auto-started\n"
"  on first use if it is not already running.\n";
}

namespace {

void PrintCliUsageJson() {
    const std::string guiExe = JsonEscape(ResolveGuiExePathUtf8());
    std::cout << R"({
  "ok": true,
  "program": "pcder",
  "description": "ProcessCommander console client. Sends commands to the running ProcessCommander GUI dashboard over a named pipe.",
  "usage": "pcder [--json] <command> [options]",
  "global_options": [
    {"name": "--json", "alias": "-j", "description": "Print the raw machine-readable JSON response. May be placed before or after the command."},
    {"name": "--help", "alias": "-h", "description": "Show usage. Combine with --json to get this machine-readable help."}
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
      "description": "Start a managed process under a Win32 Job Object and display it in the dashboard. Use '--' to pass the command line verbatim.",
      "usage": "pcder start [options] -- <command...>",
      "options": [
        {"name": "--cmd", "value": "<command>", "description": "Command line to execute."},
        {"name": "--name", "value": "<label>", "description": "Display name on the dashboard."},
        {"name": "--mode", "value": "<pty|pipe|console>", "values": ["pty", "pipe", "console"], "description": "Execution mode. Default: pipe (no ConPTY)."},
        {"name": "--pty", "description": "Enable ConPTY. Shortcut for --mode pty. Without this flag or an explicit --mode pty, no ConPTY is used."},
        {"name": "--open-stdin", "description": "Pipe mode only: keep stdin writable for pcder send. By default pipe-mode stdin is closed after launch so non-interactive programs see EOF."},
        {"name": "--cwd", "value": "<dir>", "description": "Working directory."},
        {"name": "--log-file", "aliases": ["--log"], "value": "<path>", "description": "Custom live-output log path."},
        {"name": "--done-file", "aliases": ["--done"], "value": "<path>", "description": "Custom completion marker path, written when the process exits."},
        {"name": "--timeout", "value": "<sec>", "description": "Maximum runtime in seconds (0 = unlimited)."},
        {"name": "--idle-timeout", "value": "<sec>", "description": "Kill if no output for N seconds (0 = unlimited)."},
        {"name": "--wait", "description": "Block until the process exits."},
        {"name": "--wait-ms", "value": "<ms>", "description": "Wait up to N ms for initial output. Default: 500."}
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
      "name": "help",
      "description": "Show command usage.",
      "usage": "pcder help [--json]",
      "options": []
    }
  ],
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
      "config_example": {
        "mcpServers": {
          "process-commander": {
            "command": ")" << guiExe << R"(",
            "args": ["mcp"]
          }
        }
      },
      "tools": ["start_process", "list_processes", "get_process_output", "send_process_input", "wait_process", "kill_process"]
    }
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
    std::cout << "\n";
}

void PrintProcessDetails(const std::string& procJson, const std::string& indent = "       ") {
    std::string command = JsonGetString(procJson, "command", "");
    std::string cwd = JsonGetString(procJson, "cwd", "");
    std::string start = JsonGetString(procJson, "start_time", "");
    std::string cpu = JsonGetString(procJson, "cpu_time_str", "");
    std::string logFile = JsonGetString(procJson, "log_file", "");
    std::string doneFile = JsonGetString(procJson, "done_file", "");
    std::string lastLine = JsonGetString(procJson, "last_line", "");

    if (!command.empty()) std::cout << indent << "cmd : " << command << "\n";
    if (!cwd.empty()) std::cout << indent << "cwd : " << cwd << "\n";
    if (!start.empty()) std::cout << indent << "start: " << start << "\n";
    if (!cpu.empty()) std::cout << indent << "cpu  : " << cpu << "\n";
    if (!logFile.empty()) std::cout << indent << "log : " << logFile << "\n";
    if (!doneFile.empty()) std::cout << indent << "done: " << doneFile << "\n";
    if (!lastLine.empty()) std::cout << indent << "last: " << lastLine << "\n";
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

int PrintResponse(const std::string& action, const std::string& respJson, bool jsonOutput, int requestedId = 0) {
    if (jsonOutput) {
        std::cout << respJson << "\n";
        return JsonGetBool(respJson, "ok", false) ? 0 : 1;
    }

    if (!JsonGetBool(respJson, "ok", false)) {
        std::cerr << "Error: " << JsonGetString(respJson, "error", "Unknown error.") << "\n";
        return 1;
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

    std::cout << respJson << "\n";
    return 0;
}

} // namespace

int RunCli(const std::vector<std::string>& rawArgs) {
    std::vector<std::string> args;
    args.push_back(rawArgs.empty() ? "pcder" : rawArgs[0]);

    bool jsonOutput = false;
    for (size_t i = 1; i < rawArgs.size(); ++i) {
        if (rawArgs[i] == "--json" || rawArgs[i] == "-j") {
            jsonOutput = true;
        } else {
            args.push_back(rawArgs[i]);
        }
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

    std::ostringstream reqJson;
    int requestedId = 0;

    if (subcmd == "start" || subcmd == "run") {
        std::string name;
        std::string cmd;
        std::string cwd;
        std::string logFile;
        std::string doneFile;
        std::string mode = "pipe";
        bool openStdin = false;
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
            } else if (a == "--pty") {
                mode = "pty";
            } else if (a == "--open-stdin") {
                openStdin = true;
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
            if (jsonOutput) {
                std::cout << "{\"ok\":false,\"error\":\"Missing --cmd or command arguments.\"}\n";
            } else {
                std::cerr << "Error: Missing --cmd or command arguments.\n";
            }
            return 1;
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

    return PrintResponse(subcmd, respJson, jsonOutput, requestedId);
}

} // namespace pc
