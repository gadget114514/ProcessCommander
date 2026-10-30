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

// Extract a raw JSON value (object, array, string or scalar) for a top-level key.
static std::string ExtractRawJsonValue(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    size_t keyPos = json.find(pattern);
    if (keyPos == std::string::npos) return "";
    size_t colon = json.find(':', keyPos + pattern.size());
    if (colon == std::string::npos) return "";
    size_t p = colon + 1;
    while (p < json.size() && std::isspace(static_cast<unsigned char>(json[p]))) p++;
    if (p >= json.size()) return "";
    char open = json[p];
    if (open == '[' || open == '{') {
        char close = (open == '[') ? ']' : '}';
        int depth = 0;
        bool inStr = false;
        bool escaped = false;
        for (size_t i = p; i < json.size(); ++i) {
            char c = json[i];
            if (inStr) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') { inStr = true; continue; }
            if (c == open) depth++;
            else if (c == close) { depth--; if (depth == 0) return json.substr(p, i - p + 1); }
        }
        return "";
    }
    size_t end = p;
    if (open == '"') {
        end = p + 1;
        while (end < json.size()) {
            if (json[end] == '"' && json[end - 1] != '\\') { end++; break; }
            end++;
        }
        return json.substr(p, end - p);
    }
    while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != ']' &&
           !std::isspace(static_cast<unsigned char>(json[end]))) end++;
    return json.substr(p, end - p);
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
                << "\"serverInfo\":{\"name\":\"ProcessCommander\",\"version\":\"" PC_VERSION "\"}"
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
                << "\"description\":\"Start and manage an external LLM CLI or program (e.g. claude, opencode) under a Win32 Job Object and display it on the ProcessCommander GUI Dashboard. Automatically writes live output to log_file and creates done_file atomically when the process exits. Set wait=true and wait_ms=0 to block until completion. To run multiple commands in order, use taskqueue_add instead; chained commands such as '&&' are rejected.\","
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
                << "\"estimate_end\":{\"type\":\"string\",\"description\":\"Optional estimated end time shown in gray in the End Time column while running (e.g. '18:30' or '2026-10-22 18:30:00'). If omitted, timeout_sec makes the column show remaining seconds.\"},"
                << "\"agent_type\":{\"type\":\"string\",\"description\":\"LLM agent type: 'claude', 'agy', 'opencode', 'codex', 'aider', 'ollama', or 'auto' (default: auto-detected from command/name). Enables JSONL stream parsing (<log>.stream.jsonl) and model capture.\",\"enum\":[\"auto\",\"claude\",\"agy\",\"antigravity\",\"opencode\",\"codex\",\"aider\",\"ollama\"]},"
                << "\"json_stream\":{\"type\":\"boolean\",\"description\":\"Enable JSONL stream parser and event log written to <log>.stream.jsonl (automatically enabled when agent_type is detected or set). View via GUI 'LLM JSON Stream Events' or pcder agent stream <id>.\"},"
                << "\"supervisor_notes\":{\"type\":\"string\",\"description\":\"Notes or context provided by the supervisor LLM for this worker agent\"},"
                << "\"supervisor_goal\":{\"type\":\"string\",\"description\":\"High-level goal or instruction provided by the supervisor LLM for this worker agent\"},"
                << "\"wait\":{\"type\":\"boolean\",\"description\":\"Wait until the process exits before returning (blocking completion notification)\"},"
                << "\"wait_ms\":{\"type\":\"integer\",\"description\":\"With wait=true: maximum wait in ms (0 = unlimited). With wait=false: initial output wait. Default: 500.\"}"
                << "},\"required\":[\"cmd\"]}"
                << "},"
                << "{"
                << "\"name\":\"get_agent_stream\","
                << "\"description\":\"Read structured JSON streaming events (tools called, arguments, reasoning/thoughts, assistant responses, status) emitted by Claude, agy, or OpenCode worker agents. Events are automatically captured and saved to logs/proc_<id>_<name>_stream.jsonl.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"integer\",\"description\":\"Managed process ID of the agent\"},"
                << "\"tail\":{\"type\":\"integer\",\"description\":\"Number of recent events to return (default 50, 0 = all)\"},"
                << "\"type\":{\"type\":\"string\",\"description\":\"Filter by event type: 'tool_call', 'tool_result', 'thinking', 'message', 'status', 'step', 'error', 'usage'\"},"
                << "\"since_seq\":{\"type\":\"integer\",\"description\":\"Return only events with sequence number greater than since_seq\"},"
                << "\"format\":{\"type\":\"string\",\"enum\":[\"json\",\"summary\"],\"description\":\"Return format: 'json' (structured events array, default) or 'summary' (compact text summary)\"}"
                << "},\"required\":[\"id\"]}"
                << "},"
                << "{"
                << "\"name\":\"get_agent_state\","
                << "\"description\":\"Get current live state of an LLM agent process, including active tool, current tool action, thinking status, last thoughts, assistant message, token usage, supervisor notes and goal.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"integer\",\"description\":\"Managed process ID of the agent\"}"
                << "},\"required\":[\"id\"]}"
                << "},"
                << "{"
                << "\"name\":\"update_agent_info\","
                << "\"description\":\"Update supervisor instructions, notes, or goals for an ongoing or queued LLM worker agent process.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"integer\",\"description\":\"Managed process ID of the agent\"},"
                << "\"supervisor_notes\":{\"type\":\"string\",\"description\":\"Supervisor notes/context for the worker agent\"},"
                << "\"supervisor_goal\":{\"type\":\"string\",\"description\":\"Supervisor goal for the worker agent\"}"
                << "},\"required\":[\"id\"]}"
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
                << "},"
                << "{"
                << "\"name\":\"taskqueue_add\","
                << "\"description\":\"Register one or more jobs in a task queue. ProcessCommander decides execution order, timeouts, success checks and automatic retries. Each job must have a one-line summary and its own timeoutSec. Use dependsOn to chain jobs instead of shell '&&'.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"taskqueue\":{\"type\":\"string\",\"description\":\"Task queue name, e.g. 'main'\"},"
                << "\"jobs\":{\"type\":\"array\",\"description\":\"Job objects: id, command, summary, timeoutSec (required); cwd, env, dependsOn (['id'] or [{id,on}]), idle {outputSec,cpuSec}, maxAttempts, retryDelaySec, success {exitCode,filesNewer,outputRegex}, killExclude, resources {write:[...],read:[...]} (paths/globs relative to cwd or @name locks; jobs with overlapping write/write or write/read never run together, read/read may), priority, instructionsFile or instructionsText\",\"items\":{\"type\":\"object\"}},"
                << "\"concurrency\":{\"type\":\"integer\",\"description\":\"Max jobs running at the same time for this queue (default 1)\"},"
                << "\"notifyFile\":{\"type\":\"string\",\"description\":\"Optional file updated on jobFailed / taskqueueStalled / taskqueueDrained\"}"
                << "},\"required\":[\"taskqueue\",\"jobs\"]}"
                << "},"
                << "{"
                << "\"name\":\"taskqueue_list\","
                << "\"description\":\"List task queue jobs with state, attempts, last reason and blockedBy.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"taskqueue\":{\"type\":\"string\",\"description\":\"Optional task queue name (all queues when omitted)\"},"
                << "\"state\":{\"type\":\"string\",\"description\":\"Optional state filter: pending, running, succeeded, failed, blocked, cancelled\"}"
                << "}}"
                << "},"
                << "{"
                << "\"name\":\"taskqueue_get\","
                << "\"description\":\"Get a job with attempt history, instructions text, and sourceChanged/sourceMissing flags.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"string\",\"description\":\"Job id\"},"
                << "\"includeInstructions\":{\"type\":\"boolean\",\"description\":\"Include instructions text (default true)\"},"
                << "\"rev\":{\"type\":\"integer\",\"description\":\"Optional instructions revision to return\"}"
                << "},\"required\":[\"id\"]}"
                << "},"
                << "{"
                << "\"name\":\"taskqueue_retry\","
                << "\"description\":\"Move a failed/blocked/cancelled job back to pending, optionally refreshing its instructions from the source file. Blocked dependents become pending automatically.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"string\",\"description\":\"Job id\"},"
                << "\"resetAttempts\":{\"type\":\"boolean\",\"description\":\"Reset the attempt counter (default true)\"},"
                << "\"refreshInstructions\":{\"type\":\"boolean\",\"description\":\"Reload instructions from the source file (default true)\"}"
                << "},\"required\":[\"id\"]}"
                << "},"
                << "{"
                << "\"name\":\"taskqueue_update\","
                << "\"description\":\"Update a pending/failed/blocked job (summary, command, timeoutSec, idle, maxAttempts, success, env, instructions). 'notes' may also be written after a job finishes (review result). Dependencies are changed with taskqueue_link/unlink.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"string\",\"description\":\"Job id\"},"
                << "\"summary\":{\"type\":\"string\"},"
                << "\"command\":{\"type\":\"string\"},"
                << "\"timeoutSec\":{\"type\":\"integer\"},"
                << "\"idle\":{\"type\":\"object\"},"
                << "\"maxAttempts\":{\"type\":\"integer\"},"
                << "\"success\":{\"type\":\"object\"},"
                << "\"env\":{\"type\":\"object\"},"
                << "\"instructionsFile\":{\"type\":\"string\"},"
                << "\"instructionsText\":{\"type\":\"string\"},"
                << "\"notes\":{\"type\":\"string\",\"description\":\"Review result / notes, writable even after the job finishes\"}"
                << "},\"required\":[\"id\"]}"
                << "},"
                << "{"
                << "\"name\":\"taskqueue_cancel\","
                << "\"description\":\"Cancel a job. A running attempt is killed and is not retried.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"string\",\"description\":\"Job id\"}"
                << "},\"required\":[\"id\"]}"
                << "},"
                << "{"
                << "\"name\":\"taskqueue_pause\","
                << "\"description\":\"Stop starting new jobs in a task queue (running jobs continue).\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"taskqueue\":{\"type\":\"string\"}"
                << "},\"required\":[\"taskqueue\"]}"
                << "},"
                << "{"
                << "\"name\":\"taskqueue_resume\","
                << "\"description\":\"Resume a paused task queue.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"taskqueue\":{\"type\":\"string\"}"
                << "},\"required\":[\"taskqueue\"]}"
                << "},"
                << "{"
                << "\"name\":\"taskqueue_wait\","
                << "\"description\":\"Wait until a task queue version changes (long poll) and return its jobs.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"taskqueue\":{\"type\":\"string\"},"
                << "\"sinceVersion\":{\"type\":\"integer\",\"description\":\"Version last seen by the caller\"},"
                << "\"timeoutMs\":{\"type\":\"integer\",\"description\":\"Max wait in milliseconds (default 30000)\"}"
                << "},\"required\":[\"taskqueue\"]}"
                << "},"
                << "{"
                << "\"name\":\"taskqueue_purge\","
                << "\"description\":\"Remove finished jobs from a task queue.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"taskqueue\":{\"type\":\"string\"},"
                << "\"states\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"States to purge (default succeeded, failed, cancelled)\"}"
                << "},\"required\":[\"taskqueue\"]}"
                << "},"
                << "{"
                << "\"name\":\"taskqueue_link\","
                << "\"description\":\"Add one dependency to an existing pending/blocked/failed job.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"job\":{\"type\":\"string\",\"description\":\"Job that gains the dependency\"},"
                << "\"dependsOn\":{\"type\":\"string\",\"description\":\"Dependency job id\"},"
                << "\"on\":{\"type\":\"string\",\"enum\":[\"success\",\"done\"],\"description\":\"Start condition (default success)\"}"
                << "},\"required\":[\"job\",\"dependsOn\"]}"
                << "},"
                << "{"
                << "\"name\":\"taskqueue_unlink\","
                << "\"description\":\"Remove one dependency from an existing pending/blocked/failed job.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"job\":{\"type\":\"string\"},"
                << "\"dependsOn\":{\"type\":\"string\"}"
                << "},\"required\":[\"job\",\"dependsOn\"]}"
                << "},"
                << "{"
                << "\"name\":\"taskqueue_graph\","
                << "\"description\":\"Return the dependency graph with each job's state, ready/waiting/blocked status and dependents.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"taskqueue\":{\"type\":\"string\",\"description\":\"Optional task queue filter\"},"
                << "\"root\":{\"type\":\"string\",\"description\":\"Optional job id: return only its upstream/downstream component\"}"
                << "}}"
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
                std::string estimateEnd = JsonGetString(argsJson, "estimate_end", "");
                std::string agentType = JsonGetString(argsJson, "agent_type", "");
                bool jsonStream = JsonGetBool(argsJson, "json_stream", false);
                std::string supervisorNotes = JsonGetString(argsJson, "supervisor_notes", "");
                std::string supervisorGoal = JsonGetString(argsJson, "supervisor_goal", "");
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
                       << ",\"estimate_end\":\"" << JsonEscape(estimateEnd) << "\""
                       << ",\"agent_type\":\"" << JsonEscape(agentType) << "\""
                       << ",\"json_stream\":" << (jsonStream ? "true" : "false")
                       << ",\"supervisor_notes\":\"" << JsonEscape(supervisorNotes) << "\""
                       << ",\"supervisor_goal\":\"" << JsonEscape(supervisorGoal) << "\""
                       << ",\"wait\":" << (wait ? "true" : "false")
                       << ",\"wait_ms\":" << waitMs << "}";
            }
            else if (toolName == "get_agent_stream") {
                int64_t id = JsonGetInt(argsJson, "id", 0);
                int64_t tail = JsonGetInt(argsJson, "tail", 50);
                std::string typeFilter = JsonGetString(argsJson, "type", "");
                if (typeFilter.empty()) typeFilter = JsonGetString(argsJson, "filter", "");
                int64_t sinceSeq = JsonGetInt(argsJson, "since_seq", 0);
                std::string format = JsonGetString(argsJson, "format", "json");
                ipcReq << "{\"action\":\"get_agent_stream\",\"id\":" << id
                       << ",\"tail\":" << tail
                       << ",\"type\":\"" << JsonEscape(typeFilter) << "\""
                       << ",\"since_seq\":" << sinceSeq
                       << ",\"format\":\"" << JsonEscape(format) << "\"}";
            }
            else if (toolName == "get_agent_state") {
                int64_t id = JsonGetInt(argsJson, "id", 0);
                ipcReq << "{\"action\":\"get_agent_state\",\"id\":" << id << "}";
            }
            else if (toolName == "update_agent_info") {
                int64_t id = JsonGetInt(argsJson, "id", 0);
                std::string notes = JsonGetString(argsJson, "supervisor_notes", "");
                if (notes.empty()) notes = JsonGetString(argsJson, "notes", "");
                std::string goal = JsonGetString(argsJson, "supervisor_goal", "");
                if (goal.empty()) goal = JsonGetString(argsJson, "goal", "");
                ipcReq << "{\"action\":\"update_agent_info\",\"id\":" << id
                       << ",\"supervisor_notes\":\"" << JsonEscape(notes) << "\""
                       << ",\"supervisor_goal\":\"" << JsonEscape(goal) << "\"}";
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
            else if (toolName == "taskqueue_add") {
                std::string tqName = JsonGetString(argsJson, "taskqueue", "");
                std::string jobsRaw = ExtractRawJsonValue(argsJson, "jobs");
                std::string notifyFile = JsonGetString(argsJson, "notifyFile", "");
                if (notifyFile.empty()) notifyFile = JsonGetString(argsJson, "notify_file", "");
                ipcReq << "{\"action\":\"taskqueue_add\",\"taskqueue\":\"" << JsonEscape(tqName) << "\""
                       << ",\"jobs\":" << (jobsRaw.empty() ? "[]" : jobsRaw)
                       << ",\"concurrency\":" << JsonGetInt(argsJson, "concurrency", 0)
                       << ",\"notify_file\":\"" << JsonEscape(notifyFile) << "\"}";
            }
            else if (toolName == "taskqueue_list") {
                ipcReq << "{\"action\":\"taskqueue_list\""
                       << ",\"taskqueue\":\"" << JsonEscape(JsonGetString(argsJson, "taskqueue", "")) << "\""
                       << ",\"state\":\"" << JsonEscape(JsonGetString(argsJson, "state", "")) << "\"}";
            }
            else if (toolName == "taskqueue_get") {
                bool includeInstr = JsonGetBool(argsJson, "includeInstructions",
                                                JsonGetBool(argsJson, "include_instructions", true));
                ipcReq << "{\"action\":\"taskqueue_get\""
                       << ",\"id\":\"" << JsonEscape(JsonGetString(argsJson, "id", "")) << "\""
                       << ",\"include_instructions\":" << (includeInstr ? "true" : "false")
                       << ",\"rev\":" << JsonGetInt(argsJson, "rev", 0) << "}";
            }
            else if (toolName == "taskqueue_retry") {
                bool resetAttempts = JsonGetBool(argsJson, "resetAttempts",
                                                 JsonGetBool(argsJson, "reset_attempts", true));
                bool refreshInstr = JsonGetBool(argsJson, "refreshInstructions",
                                                JsonGetBool(argsJson, "refresh_instructions", true));
                ipcReq << "{\"action\":\"taskqueue_retry\""
                       << ",\"id\":\"" << JsonEscape(JsonGetString(argsJson, "id", "")) << "\""
                       << ",\"reset_attempts\":" << (resetAttempts ? "true" : "false")
                       << ",\"refresh_instructions\":" << (refreshInstr ? "true" : "false") << "}";
            }
            else if (toolName == "taskqueue_update") {
                ipcReq << "{\"action\":\"taskqueue_update\""
                       << ",\"id\":\"" << JsonEscape(JsonGetString(argsJson, "id", "")) << "\""
                       << ",\"patch\":" << argsJson << "}";
            }
            else if (toolName == "taskqueue_cancel") {
                ipcReq << "{\"action\":\"taskqueue_cancel\",\"id\":\""
                       << JsonEscape(JsonGetString(argsJson, "id", "")) << "\"}";
            }
            else if (toolName == "taskqueue_pause" || toolName == "taskqueue_resume") {
                ipcReq << "{\"action\":\"" << toolName << "\",\"taskqueue\":\""
                       << JsonEscape(JsonGetString(argsJson, "taskqueue", "")) << "\"}";
            }
            else if (toolName == "taskqueue_wait") {
                int64_t sinceVersion = JsonGetInt(argsJson, "sinceVersion",
                                                  JsonGetInt(argsJson, "since_version", 0));
                int64_t timeoutMs = JsonGetInt(argsJson, "timeoutMs",
                                               JsonGetInt(argsJson, "timeout_ms", 30000));
                ipcReq << "{\"action\":\"taskqueue_wait\""
                       << ",\"taskqueue\":\"" << JsonEscape(JsonGetString(argsJson, "taskqueue", "")) << "\""
                       << ",\"since_version\":" << sinceVersion
                       << ",\"timeout_ms\":" << timeoutMs << "}";
            }
            else if (toolName == "taskqueue_purge") {
                std::string statesRaw = ExtractRawJsonValue(argsJson, "states");
                ipcReq << "{\"action\":\"taskqueue_purge\""
                       << ",\"taskqueue\":\"" << JsonEscape(JsonGetString(argsJson, "taskqueue", "")) << "\""
                       << ",\"states\":" << (statesRaw.empty() ? "[]" : statesRaw) << "}";
            }
            else if (toolName == "taskqueue_link") {
                std::string dep = JsonGetString(argsJson, "dependsOn", "");
                if (dep.empty()) dep = JsonGetString(argsJson, "depends_on", "");
                ipcReq << "{\"action\":\"taskqueue_link\""
                       << ",\"job\":\"" << JsonEscape(JsonGetString(argsJson, "job", "")) << "\""
                       << ",\"depends_on\":\"" << JsonEscape(dep) << "\""
                       << ",\"on\":\"" << JsonEscape(JsonGetString(argsJson, "on", "")) << "\"}";
            }
            else if (toolName == "taskqueue_unlink") {
                std::string dep = JsonGetString(argsJson, "dependsOn", "");
                if (dep.empty()) dep = JsonGetString(argsJson, "depends_on", "");
                ipcReq << "{\"action\":\"taskqueue_unlink\""
                       << ",\"job\":\"" << JsonEscape(JsonGetString(argsJson, "job", "")) << "\""
                       << ",\"depends_on\":\"" << JsonEscape(dep) << "\"}";
            }
            else if (toolName == "taskqueue_graph") {
                ipcReq << "{\"action\":\"taskqueue_graph\""
                       << ",\"taskqueue\":\"" << JsonEscape(JsonGetString(argsJson, "taskqueue", "")) << "\""
                       << ",\"root\":\"" << JsonEscape(JsonGetString(argsJson, "root", "")) << "\"}";
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
