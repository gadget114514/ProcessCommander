#include "mcp_server.h"
#include "ipc_client.h"
#include "common.h"
#include <iostream>
#include <sstream>
#include <io.h>
#include <fcntl.h>

namespace pc {

// Extract raw JSON value for "id" (e.g. 1 or "abc") from top-level request
static std::string ExtractJsonRpcIdRaw(const std::string& json) {
    size_t pos = json.find("\"id\"");
    if (pos == std::string::npos) return "null";
    pos += 4;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ':')) pos++;
    if (pos >= json.size()) return "null";
    if (json[pos] == '"') {
        size_t end = pos + 1;
        while (end < json.size()) {
            if (json[end] == '"' && json[end - 1] != '\\') {
                return json.substr(pos, end - pos + 1);
            }
            end++;
        }
        return "null";
    } else {
        size_t end = pos;
        while (end < json.size() && json[end] != ',' && json[end] != '}' && !std::isspace(static_cast<unsigned char>(json[end]))) {
            end++;
        }
        return json.substr(pos, end - pos);
    }
}

// Extract the "arguments" object substring inside "params"
static std::string ExtractArgumentsJson(const std::string& json) {
    size_t pos = json.find("\"arguments\"");
    if (pos == std::string::npos) return "{}";
    pos = json.find('{', pos);
    if (pos == std::string::npos) return "{}";
    int depth = 0;
    bool inStr = false;
    for (size_t i = pos; i < json.size(); ++i) {
        char c = json[i];
        if (c == '"' && (i == 0 || json[i - 1] != '\\')) {
            inStr = !inStr;
        } else if (!inStr) {
            if (c == '{') depth++;
            else if (c == '}') {
                depth--;
                if (depth == 0) {
                    return json.substr(pos, i - pos + 1);
                }
            }
        }
    }
    return json.substr(pos);
}

static std::string BuildMcpToolResult(const std::string& rpcIdRaw, const std::string& contentText, bool isError = false) {
    std::ostringstream oss;
    oss << "{\"jsonrpc\":\"2.0\",\"id\":" << rpcIdRaw
        << ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"" << JsonEscape(contentText) << "\"}]"
        << (isError ? ",\"isError\":true" : "")
        << "}}";
    return oss.str();
}

int RunMcpServer() {
    // Set stdin and stdout to binary mode to avoid CRLF translation issues
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    WriteAuditLog("MCP_SERVER", "MCP stdio server started (Log dir: " + GetLogsDirectoryUtf8() + ")");

    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) continue;

        std::string method = JsonGetString(line, "method", "");
        std::string rpcId = ExtractJsonRpcIdRaw(line);

        if (method == "initialize") {
            WriteAuditLog("MCP_INIT", line);
            std::ostringstream oss;
            oss << "{\"jsonrpc\":\"2.0\",\"id\":" << rpcId
                << ",\"result\":{"
                << "\"protocolVersion\":\"2024-11-05\","
                << "\"capabilities\":{\"tools\":{}},"
                << "\"serverInfo\":{\"name\":\"ProcessCommander\",\"version\":\"1.0.0\"}"
                << "}}";
            std::cout << oss.str() << "\n" << std::flush;
        }
        else if (method == "notifications/initialized" || method.rfind("notifications/", 0) == 0) {
            // Notification requires no response
            continue;
        }
        else if (method == "ping") {
            std::cout << "{\"jsonrpc\":\"2.0\",\"id\":" << rpcId << ",\"result\":{}}\n" << std::flush;
        }
        else if (method == "tools/list") {
            std::ostringstream oss;
            oss << "{\"jsonrpc\":\"2.0\",\"id\":" << rpcId << ",\"result\":{\"tools\":["
                << "{"
                << "\"name\":\"start_process\","
                << "\"description\":\"Start and manage an external LLM CLI or program (e.g. claude, opencode) under a Win32 Job Object and display it on the ProcessCommander GUI Dashboard. Automatically writes live output to log_file and creates done_file atomically when the process exits. Set wait=true and wait_ms=0 to block until completion.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"cmd\":{\"type\":\"string\",\"description\":\"Command line to execute (e.g. 'claude -p ...' or 'opencode')\"},"
                << "\"name\":{\"type\":\"string\",\"description\":\"Friendly display label for the dashboard\"},"
                << "\"mode\":{\"type\":\"string\",\"enum\":[\"pipe\",\"pty\",\"console\"],\"description\":\"Execution mode: 'pipe' (default; non-interactive stdout/stdin, no ConPTY), 'pty' (Win32 ConPTY interactive terminal), or 'console' (separate visible console window)\"},"
                << "\"pty\":{\"type\":\"boolean\",\"description\":\"Set true to enable ConPTY (same as mode='pty'). Default false.\"},"
                << "\"open_stdin\":{\"type\":\"boolean\",\"description\":\"Pipe mode only: keep stdin writable for send_process_input. Default true; set false to close stdin after launch so non-interactive programs see EOF.\"},"
                << "\"cwd\":{\"type\":\"string\",\"description\":\"Working directory path (default: current directory)\"},"
                << "\"log_file\":{\"type\":\"string\",\"description\":\"Optional custom path for the real-time output log file (defaults to logs/proc_<id>_<name>_<timestamp>.log)\"},"
                << "\"done_file\":{\"type\":\"string\",\"description\":\"Optional custom path for the completion marker file created atomically when the process exits (defaults to logs/proc_<id>.done)\"},"
                << "\"timeout_sec\":{\"type\":\"integer\",\"description\":\"Max runtime timeout in seconds (0 = unlimited)\"},"
                << "\"idle_timeout_sec\":{\"type\":\"integer\",\"description\":\"Auto-kill if no output for N seconds (0 = unlimited)\"},"
                << "\"wait\":{\"type\":\"boolean\",\"description\":\"Wait until the process exits before returning (blocking completion notification)\"},"
                << "\"wait_ms\":{\"type\":\"integer\",\"description\":\"With wait=true: maximum wait in ms (0 = unlimited). With wait=false: initial output wait. Default: 500.\"}"
                << "},\"required\":[\"cmd\"]}"
                << "},"
                << "{"
                << "\"name\":\"list_processes\","
                << "\"description\":\"List all managed processes with their status, PID, process tree count, memory usage, elapsed/idle time, and latest output line.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"integer\",\"description\":\"Optional specific process ID to query\"}"
                << "}}"
                << "},"
                << "{"
                << "\"name\":\"get_process_output\","
                << "\"description\":\"Read stdout/stderr or ConPTY terminal output of a managed process.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"integer\",\"description\":\"Managed process ID\"},"
                << "\"tail\":{\"type\":\"integer\",\"description\":\"Number of trailing lines to fetch (0 = all)\"},"
                << "\"wait_ms\":{\"type\":\"integer\",\"description\":\"Optional milliseconds to wait before reading\"}"
                << "},\"required\":[\"id\"]}"
                << "},"
                << "{"
                << "\"name\":\"send_process_input\","
                << "\"description\":\"Send text input to a running process's stdin or ConPTY session.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"integer\",\"description\":\"Managed process ID\"},"
                << "\"input\":{\"type\":\"string\",\"description\":\"Text to send\"},"
                << "\"newline\":{\"type\":\"boolean\",\"description\":\"Append newline (default true)\"},"
                << "\"wait_ms\":{\"type\":\"integer\",\"description\":\"Milliseconds to wait for response output after sending (default 500)\"}"
                << "},\"required\":[\"id\",\"input\"]}"
                << "},"
                << "{"
                << "\"name\":\"kill_process\","
                << "\"description\":\"Forcefully terminate a managed process and all of its child/grandchild processes via Win32 Job Object.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"integer\",\"description\":\"Managed process ID (or 0 to kill all running processes)\"}"
                << "},\"required\":[\"id\"]}"
                << "},"
                << "{"
                << "\"name\":\"wait_process\","
                << "\"description\":\"Wait for a managed process to exit (up to wait_ms) and return its final status and output.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"integer\",\"description\":\"Managed process ID\"},"
                << "\"wait_ms\":{\"type\":\"integer\",\"description\":\"Max milliseconds to wait (default 30000)\"}"
                << "},\"required\":[\"id\"]}"
                << "}"
                << "]}}";
            std::cout << oss.str() << "\n" << std::flush;
        }
        else if (method == "tools/call") {
            std::string toolName = JsonGetString(line, "name", "");
            std::string argsJson = ExtractArgumentsJson(line);
            WriteAuditLog("MCP_TOOL_CALL", "tool=" + toolName + " args=" + argsJson);
            std::ostringstream ipcReq;

            if (toolName == "start_process") {
                std::string cmd = JsonGetString(argsJson, "cmd", "");
                std::string name = JsonGetString(argsJson, "name", "");
                std::string mode = JsonGetString(argsJson, "mode", "pipe");
                if (JsonGetBool(argsJson, "pty", false)) {
                    mode = "pty";
                }
                bool openStdin = JsonGetBool(argsJson, "open_stdin", true);
                std::string cwd = ResolveAbsolutePathUtf8(JsonGetString(argsJson, "cwd", ""));
                std::string logFile = JsonGetString(argsJson, "log_file", "");
                std::string doneFile = JsonGetString(argsJson, "done_file", "");
                int64_t timeoutSec = JsonGetInt(argsJson, "timeout_sec", 0);
                int64_t idleTimeoutSec = JsonGetInt(argsJson, "idle_timeout_sec", 0);
                bool wait = JsonGetBool(argsJson, "wait", false);
                int64_t waitMs = JsonGetInt(argsJson, "wait_ms", -1);
                if (waitMs < 0) {
                    waitMs = wait ? 0 : 500;
                }

                ipcReq << "{\"action\":\"start\""
                       << ",\"cmd\":\"" << JsonEscape(cmd) << "\""
                       << ",\"name\":\"" << JsonEscape(name) << "\""
                       << ",\"mode\":\"" << JsonEscape(mode) << "\""
                       << ",\"open_stdin\":" << (openStdin ? "true" : "false")
                       << ",\"cwd\":\"" << JsonEscape(cwd) << "\""
                       << ",\"log_file\":\"" << JsonEscape(logFile) << "\""
                       << ",\"done_file\":\"" << JsonEscape(doneFile) << "\""
                       << ",\"timeout_sec\":" << timeoutSec
                       << ",\"idle_timeout_sec\":" << idleTimeoutSec
                       << ",\"wait\":" << (wait ? "true" : "false")
                       << ",\"wait_ms\":" << waitMs << "}";
            }
            else if (toolName == "list_processes") {
                int64_t id = JsonGetInt(argsJson, "id", 0);
                ipcReq << "{\"action\":\"list\",\"id\":" << id << "}";
            }
            else if (toolName == "get_process_output") {
                int64_t id = JsonGetInt(argsJson, "id", 0);
                int64_t tail = JsonGetInt(argsJson, "tail", 100);
                int64_t waitMs = JsonGetInt(argsJson, "wait_ms", 0);
                ipcReq << "{\"action\":\"logs\",\"id\":" << id << ",\"tail\":" << tail << ",\"wait_ms\":" << waitMs << "}";
            }
            else if (toolName == "send_process_input") {
                int64_t id = JsonGetInt(argsJson, "id", 0);
                std::string input = JsonGetString(argsJson, "input", "");
                bool newline = JsonGetBool(argsJson, "newline", true);
                int64_t waitMs = JsonGetInt(argsJson, "wait_ms", 500);
                ipcReq << "{\"action\":\"send\",\"id\":" << id
                       << ",\"input\":\"" << JsonEscape(input) << "\""
                       << ",\"newline\":" << (newline ? "true" : "false")
                       << ",\"wait_ms\":" << waitMs << "}";
            }
            else if (toolName == "kill_process") {
                int64_t id = JsonGetInt(argsJson, "id", 0);
                if (id <= 0) {
                    ipcReq << "{\"action\":\"kill_all\"}";
                } else {
                    ipcReq << "{\"action\":\"kill\",\"id\":" << id << "}";
                }
            }
            else if (toolName == "wait_process") {
                int64_t id = JsonGetInt(argsJson, "id", 0);
                int64_t waitMs = JsonGetInt(argsJson, "wait_ms", 30000);
                ipcReq << "{\"action\":\"wait\",\"id\":" << id << ",\"wait_ms\":" << waitMs << "}";
            }
            else {
                std::cout << BuildMcpToolResult(rpcId, "Unknown tool: " + toolName, true) << "\n" << std::flush;
                continue;
            }

            std::string ipcResp, err;
            if (!SendIpcRequest(ipcReq.str(), ipcResp, err, true)) {
                WriteAuditLog("MCP_TOOL_ERR", "tool=" + toolName + " error=" + err);
                std::cout << BuildMcpToolResult(rpcId, "IPC error: " + err, true) << "\n" << std::flush;
            } else {
                bool ok = JsonGetBool(ipcResp, "ok", false);
                WriteAuditLog("MCP_TOOL_RESP", "tool=" + toolName + " ok=" + (ok ? "true" : "false") + " bytes=" + std::to_string(ipcResp.size()));
                std::cout << BuildMcpToolResult(rpcId, ipcResp, !ok) << "\n" << std::flush;
            }
        }
        else {
            std::cout << "{\"jsonrpc\":\"2.0\",\"id\":" << rpcId
                      << ",\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}\n" << std::flush;
        }
    }
    return 0;
}

} // namespace pc
