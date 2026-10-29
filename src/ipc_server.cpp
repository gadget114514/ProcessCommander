#include "ipc_server.h"
#include "chain_guard.h"
#include "llm_stream.h"
#include <ctime>
#include <sstream>
#include <cctype>

namespace pc {

constexpr UINT WM_PC_IPC_EVENT = WM_APP + 101;

std::string SnapshotToJson(const ProcessSnapshot& s) {
    std::ostringstream oss;
    oss << "{"
        << "\"id\":" << s.id << ","
        << "\"name\":\"" << JsonEscape(s.name) << "\","
        << "\"command\":\"" << JsonEscape(s.commandLine) << "\","
        << "\"cwd\":\"" << JsonEscape(s.workingDir) << "\","
        << "\"mode\":\"" << ExecModeToString(s.mode) << "\","
        << "\"state\":\"" << ProcStateToString(s.state) << "\","
        << "\"pid\":" << s.rootPid << ","
        << "\"exit_code\":" << s.exitCode << ","
        << "\"start_time\":\"" << JsonEscape(s.startTimeStr) << "\","
        << "\"end_time\":\"" << JsonEscape(s.endTimeStr) << "\","
        << "\"estimated_end_time\":\"" << JsonEscape(s.estimatedEndTimeStr) << "\","
        << "\"elapsed_ms\":" << s.elapsedMs << ","
        << "\"elapsed_str\":\"" << JsonEscape(FormatDurationSec(s.elapsedMs)) << "\","
        << "\"cpu_time_ms\":" << s.cpuTimeMs << ","
        << "\"cpu_time_str\":\"" << JsonEscape(FormatUptimeHMS(s.cpuTimeMs)) << "\","
        << "\"uptime_str\":\"" << JsonEscape(FormatUptimeHMS(s.elapsedMs)) << "\","
        << "\"idle_ms\":" << s.idleMs << ","
        << "\"timeout_sec\":" << s.timeoutSec << ","
        << "\"idle_timeout_sec\":" << s.idleTimeoutSec << ","
        << "\"tree_procs\":" << s.treeProcessCount << ","
        << "\"memory_bytes\":" << s.treeMemoryBytes << ","
        << "\"memory_str\":\"" << JsonEscape(FormatBytes(s.treeMemoryBytes)) << "\","
        << "\"total_bytes_read\":" << s.totalBytesRead << ","
        << "\"log_file\":\"" << JsonEscape(s.logFilePath) << "\","
        << "\"done_file\":\"" << JsonEscape(s.doneFilePath) << "\","
        << "\"last_line\":\"" << JsonEscape(s.lastLinePreview) << "\","
        << "\"job_key\":\"" << JsonEscape(s.jobKey) << "\","
        << "\"job_id\":" << s.jobId << ","
        << "\"attempt\":" << s.attemptN << ","
        << "\"agent_type\":\"" << JsonEscape(s.agentType) << "\","
        << "\"agent_status\":\"" << JsonEscape(s.agentStatus) << "\","
        << "\"current_tool\":\"" << JsonEscape(s.currentTool) << "\","
        << "\"current_tool_action\":\"" << JsonEscape(s.currentToolAction) << "\","
        << "\"last_thought\":\"" << JsonEscape(s.lastThought) << "\","
        << "\"last_message\":\"" << JsonEscape(s.lastMessage) << "\","
        << "\"step_count\":" << s.stepCount << ","
        << "\"stream_event_count\":" << s.streamEventCount << ","
        << "\"stream_file\":\"" << JsonEscape(s.streamFilePath) << "\","
        << "\"supervisor_notes\":\"" << JsonEscape(s.supervisorNotes) << "\","
        << "\"supervisor_goal\":\"" << JsonEscape(s.supervisorGoal) << "\""
        << "}";
    return oss.str();
}


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

IpcServer::IpcServer(ProcessManager& pm, TaskQueueManager& tqm, HWND notifyHwnd)
    : pm_(pm), tqm_(tqm), notifyHwnd_(notifyHwnd) {}

IpcServer::~IpcServer() {
    Stop();
}

bool IpcServer::Start() {
    stop_ = false;
    acceptThread_ = std::thread(&IpcServer::AcceptLoop, this);
    return true;
}

void IpcServer::Stop() {
    if (stop_.exchange(true)) return;
    // Wake up blocking ConnectNamedPipe by opening a dummy connection
    HANDLE hDummy = CreateFileW(GetPipeNameWide().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hDummy != INVALID_HANDLE_VALUE) {
        CloseHandle(hDummy);
    }
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
}

void IpcServer::AcceptLoop() {
    while (!stop_) {
        HANDLE hPipe = CreateNamedPipeW(
            GetPipeNameWide().c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES,
            256 * 1024,
            64 * 1024,
            0,
            nullptr
        );
        if (hPipe == INVALID_HANDLE_VALUE) {
            Sleep(100);
            continue;
        }

        BOOL connected = ConnectNamedPipe(hPipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (stop_) {
            CloseHandle(hPipe);
            break;
        }
        if (connected) {
            std::thread(&IpcServer::HandleClient, this, hPipe).detach();
        } else {
            CloseHandle(hPipe);
        }
    }
}

void IpcServer::HandleClient(HANDLE hPipe) {
    std::string req;
    char buf[16384];
    DWORD bytesRead = 0;

    while (true) {
        BOOL ok = ReadFile(hPipe, buf, sizeof(buf), &bytesRead, nullptr);
        if (bytesRead > 0) {
            req.append(buf, bytesRead);
        }
        if (ok) {
            break; // Message complete in PIPE_READMODE_MESSAGE
        }
        if (GetLastError() != ERROR_MORE_DATA) {
            break;
        }
    }

    if (!req.empty()) {
        std::string resp = HandleRequestJson(req);
        DWORD written = 0;
        WriteFile(hPipe, resp.data(), static_cast<DWORD>(resp.size()), &written, nullptr);
        FlushFileBuffers(hPipe);
    }

    DisconnectNamedPipe(hPipe);
    CloseHandle(hPipe);
}

std::string IpcServer::HandleRequestJson(const std::string& reqJson) {
    std::string action = JsonGetString(reqJson, "action", "");
    if (action != "ping") {
        WriteAuditLog("IPC_REQ", reqJson);
    }
    std::ostringstream oss;

    if (action == "ping") {
        return "{\"ok\":true,\"status\":\"running\"}";
    }
    else if (action == "start") {
        StartProcessRequest req;
        req.name = JsonGetString(reqJson, "name", "");
        req.commandLine = JsonGetString(reqJson, "cmd", "");
        req.workingDir = JsonGetString(reqJson, "cwd", "");
        req.logFile = JsonGetString(reqJson, "log_file", "");
        req.doneFile = JsonGetString(reqJson, "done_file", "");
        req.mode = StringToExecMode(JsonGetString(reqJson, "mode", "pipe"));
        req.openStdin = JsonGetBool(reqJson, "open_stdin", true);
        req.timeoutSec = static_cast<int>(JsonGetInt(reqJson, "timeout_sec", 0));
        req.idleTimeoutSec = static_cast<int>(JsonGetInt(reqJson, "idle_timeout_sec", 0));
        req.estimatedEndTimeStr = JsonGetString(reqJson, "estimate_end", "");
        req.agentType = JsonGetString(reqJson, "agent_type", "");
        req.jsonStream = JsonGetBool(reqJson, "json_stream", false);
        req.supervisorNotes = JsonGetString(reqJson, "supervisor_notes", "");
        req.supervisorGoal = JsonGetString(reqJson, "supervisor_goal", "");
        bool waitFinish = JsonGetBool(reqJson, "wait", false);
        int waitMs = static_cast<int>(JsonGetInt(reqJson, "wait_ms", 0));

        if (!JsonGetBool(reqJson, "allow_chain", false)) {
            std::string chainErr;
            if (!CheckChainGuard(req.commandLine, chainErr)) {
                oss << "{\"ok\":false,\"error\":\"" << JsonEscape(chainErr) << "\"}";
                return oss.str();
            }
        }
        std::string guardJobId;
        if (tqm_.FindUnfinishedJobByCommand(req.commandLine, guardJobId)) {
            std::string msg = "This command belongs to unfinished taskqueue job '" + guardJobId +
                              "'. Use taskqueue_retry " + guardJobId + " instead of start_process.";
            oss << "{\"ok\":false,\"error\":\"" << JsonEscape(msg) << "\"}";
            return oss.str();
        }

        std::string err;
        int id = pm_.StartProcess(req, err);
        if (id < 0) {
            oss << "{\"ok\":false,\"error\":\"" << JsonEscape(err) << "\"}";
            return oss.str();
        }

        HWND hwnd = notifyHwnd_.load();
        if (hwnd) PostMessageW(hwnd, WM_PC_IPC_EVENT, static_cast<WPARAM>(id), 0);

        if (waitFinish) {
            uint32_t maxWait = (waitMs > 0) ? static_cast<uint32_t>(waitMs) : 0;
            pm_.WaitForExit(id, maxWait);
        } else if (waitMs > 0) {
            pm_.WaitForExit(id, static_cast<uint32_t>(waitMs));
        }

        ProcessSnapshot snap;
        pm_.GetSnapshot(id, snap);
        std::string outLog;
        pm_.GetOutput(id, true, 200, outLog);

        oss << "{\"ok\":true,\"id\":" << id
            << ",\"log_file\":\"" << JsonEscape(snap.logFilePath) << "\""
            << ",\"done_file\":\"" << JsonEscape(snap.doneFilePath) << "\""
            << ",\"process\":" << SnapshotToJson(snap)
            << ",\"output\":\"" << JsonEscape(outLog) << "\"}";
        return oss.str();
    }
    else if (action == "list") {
        int filterId = static_cast<int>(JsonGetInt(reqJson, "id", 0));
        oss << "{\"ok\":true,\"processes\":[";
        if (filterId > 0) {
            ProcessSnapshot snap;
            if (pm_.GetSnapshot(filterId, snap)) {
                oss << SnapshotToJson(snap);
            }
        } else {
            auto snaps = pm_.GetAllSnapshots();
            for (size_t i = 0; i < snaps.size(); ++i) {
                if (i > 0) oss << ",";
                oss << SnapshotToJson(snaps[i]);
            }
        }
        oss << "]}";
        return oss.str();
    }
    else if (action == "logs" || action == "output") {
        int id = static_cast<int>(JsonGetInt(reqJson, "id", 0));
        int tailLines = static_cast<int>(JsonGetInt(reqJson, "tail", 0));
        bool raw = JsonGetBool(reqJson, "raw", false);
        int waitMs = static_cast<int>(JsonGetInt(reqJson, "wait_ms", 0));

        if (waitMs > 0) {
            pm_.WaitForExit(id, static_cast<uint32_t>(waitMs));
        }

        ProcessSnapshot snap;
        if (!pm_.GetSnapshot(id, snap)) {
            return "{\"ok\":false,\"error\":\"Process ID not found.\"}";
        }
        std::string text;
        pm_.GetOutput(id, !raw, tailLines, text);
        oss << "{\"ok\":true,\"id\":" << id
            << ",\"process\":" << SnapshotToJson(snap)
            << ",\"output\":\"" << JsonEscape(text) << "\"}";
        return oss.str();
    }
    else if (action == "send") {
        int id = static_cast<int>(JsonGetInt(reqJson, "id", 0));
        std::string input = JsonGetString(reqJson, "input", "");
        bool newline = JsonGetBool(reqJson, "newline", true);
        int waitMs = static_cast<int>(JsonGetInt(reqJson, "wait_ms", 300));

        std::string err;
        if (!pm_.SendInput(id, input, newline, err)) {
            oss << "{\"ok\":false,\"error\":\"" << JsonEscape(err) << "\"}";
            return oss.str();
        }
        if (waitMs > 0) {
            Sleep(static_cast<DWORD>(waitMs));
        }
        ProcessSnapshot snap;
        pm_.GetSnapshot(id, snap);
        std::string outLog;
        pm_.GetOutput(id, true, 50, outLog);
        oss << "{\"ok\":true,\"id\":" << id
            << ",\"process\":" << SnapshotToJson(snap)
            << ",\"recent_output\":\"" << JsonEscape(outLog) << "\"}";
        return oss.str();
    }
    else if (action == "kill") {
        int id = static_cast<int>(JsonGetInt(reqJson, "id", 0));
        std::string err;
        if (!pm_.KillProcess(id, err)) {
            oss << "{\"ok\":false,\"error\":\"" << JsonEscape(err) << "\"}";
            return oss.str();
        }
        HWND hwnd = notifyHwnd_.load();
        if (hwnd) PostMessageW(hwnd, WM_PC_IPC_EVENT, static_cast<WPARAM>(id), 0);
        return "{\"ok\":true,\"killed_id\":" + std::to_string(id) + "}";
    }
    else if (action == "kill_all") {
        int count = pm_.KillAllRunning();
        HWND hwnd = notifyHwnd_.load();
        if (hwnd) PostMessageW(hwnd, WM_PC_IPC_EVENT, 0, 0);
        return "{\"ok\":true,\"killed_count\":" + std::to_string(count) + "}";
    }
    else if (action == "clear") {
        int count = pm_.ClearFinished();
        HWND hwnd = notifyHwnd_.load();
        if (hwnd) PostMessageW(hwnd, WM_PC_IPC_EVENT, 0, 0);
        return "{\"ok\":true,\"cleared_count\":" + std::to_string(count) + "}";
    }
    else if (action == "wait") {
        int id = static_cast<int>(JsonGetInt(reqJson, "id", 0));
        int waitMs = static_cast<int>(JsonGetInt(reqJson, "wait_ms", 30000));
        ProcessSnapshot snap;
        if (!pm_.GetSnapshot(id, snap)) {
            return "{\"ok\":false,\"error\":\"Process ID not found.\"}";
        }
        bool finished = pm_.WaitForExit(id, static_cast<uint32_t>(waitMs > 0 ? waitMs : 0));
        pm_.GetSnapshot(id, snap);
        std::string outLog;
        pm_.GetOutput(id, true, 200, outLog);
        oss << "{\"ok\":true,\"finished\":" << (finished ? "true" : "false")
            << ",\"process\":" << SnapshotToJson(snap)
            << ",\"output\":\"" << JsonEscape(outLog) << "\"}";
        return oss.str();
    }
    else if (action == "get_agent_stream") {
        int id = static_cast<int>(JsonGetInt(reqJson, "id", 0));
        int tail = static_cast<int>(JsonGetInt(reqJson, "tail", 50));
        std::string typeFilter = JsonGetString(reqJson, "type", "");
        if (typeFilter.empty()) typeFilter = JsonGetString(reqJson, "filter", "");
        uint64_t sinceSeq = static_cast<uint64_t>(JsonGetInt(reqJson, "since_seq", 0));
        std::string format = JsonGetString(reqJson, "format", "json");

        std::vector<LlmStreamEvent> events;
        LlmAgentState state;
        if (!pm_.GetAgentStream(id, tail, typeFilter, sinceSeq, events, state)) {
            return "{\"ok\":false,\"error\":\"Agent stream not found for process ID " + std::to_string(id) + "\"}";
        }

        if (format == "summary") {
            std::string summary = LlmEventsToSummary(events, state);
            oss << "{\"ok\":true,\"id\":" << id << ",\"agent_type\":\"" << JsonEscape(state.agentType)
                << "\",\"status\":\"" << JsonEscape(state.status)
                << "\",\"summary\":\"" << JsonEscape(summary) << "\"}";
            return oss.str();
        }

        std::string jsonEvents = LlmEventsToJson(events, state);
        return jsonEvents;
    }
    else if (action == "get_usage_history") {
        int sinceSec = static_cast<int>(JsonGetInt(reqJson, "since_sec", 86400));
        int64_t since = static_cast<int64_t>(time(nullptr)) - sinceSec;
        std::vector<LlmUsagePoint> pts = GetLlmUsageHistory(since);
        oss << "{\"ok\":true,\"record_calls\":" << GetLlmUsageRecordCalls()
            << ",\"count\":" << pts.size() << ",\"points\":[";
        for (size_t i = 0; i < pts.size(); ++i) {
            if (i) oss << ",";
            oss << "{\"epoch\":" << pts[i].epochSec
                << ",\"input\":" << pts[i].inputTokens
                << ",\"output\":" << pts[i].outputTokens
                << ",\"thinking\":" << pts[i].thinkingTokens
                << ",\"cache\":" << (pts[i].cacheReadTokens + pts[i].cacheWriteTokens)
                << ",\"total\":" << pts[i].totalTokens
                << ",\"cost\":" << pts[i].costUsd
                << ",\"agent\":\"" << JsonEscape(pts[i].agentType) << "\""
                << ",\"model\":\"" << JsonEscape(pts[i].model) << "\"}";
        }
        oss << "]}";
        return oss.str();
    }
    else if (action == "get_agent_state") {
        int id = static_cast<int>(JsonGetInt(reqJson, "id", 0));
        std::vector<LlmStreamEvent> events;
        LlmAgentState st;
        if (!pm_.GetAgentStream(id, 1, "", 0, events, st)) {
            ProcessSnapshot snap;
            if (pm_.GetSnapshot(id, snap)) {
                st.agentType = snap.agentType;
                st.status = snap.agentStatus;
                st.currentTool = snap.currentTool;
                st.currentToolAction = snap.currentToolAction;
                st.lastThought = snap.lastThought;
                st.lastMessage = snap.lastMessage;
                st.stepCount = snap.stepCount;
                st.eventCount = snap.streamEventCount;
                st.streamFilePath = snap.streamFilePath;
                st.supervisorNotes = snap.supervisorNotes;
                st.supervisorGoal = snap.supervisorGoal;
            } else {
                return "{\"ok\":false,\"error\":\"Process ID " + std::to_string(id) + " not found\"}";
            }
        }
        oss << "{\"ok\":true,\"id\":" << id
            << ",\"agent_type\":\"" << JsonEscape(st.agentType) << "\""
            << ",\"status\":\"" << JsonEscape(st.status) << "\""
            << ",\"current_tool\":\"" << JsonEscape(st.currentTool) << "\""
            << ",\"current_tool_action\":\"" << JsonEscape(st.currentToolAction) << "\""
            << ",\"last_thought\":\"" << JsonEscape(st.lastThought) << "\""
            << ",\"last_message\":\"" << JsonEscape(st.lastMessage) << "\""
            << ",\"step_count\":" << st.stepCount
            << ",\"event_count\":" << st.eventCount
            << ",\"total_tokens\":" << st.totalTokens
            << ",\"cost_usd\":" << st.costUsd
            << ",\"stream_file\":\"" << JsonEscape(st.streamFilePath) << "\""
            << ",\"supervisor_notes\":\"" << JsonEscape(st.supervisorNotes) << "\""
            << ",\"supervisor_goal\":\"" << JsonEscape(st.supervisorGoal) << "\""
            << ",\"updated_at\":\"" << JsonEscape(st.updatedAt) << "\"}";
        return oss.str();
    }
    else if (action == "update_agent_info") {
        int id = static_cast<int>(JsonGetInt(reqJson, "id", 0));
        std::string notes = JsonGetString(reqJson, "supervisor_notes", "");
        if (notes.empty()) notes = JsonGetString(reqJson, "notes", "");
        std::string goal = JsonGetString(reqJson, "supervisor_goal", "");
        if (goal.empty()) goal = JsonGetString(reqJson, "goal", "");
        if (!pm_.UpdateAgentInfo(id, notes, goal)) {
            return "{\"ok\":false,\"error\":\"Failed to update agent info for process ID " + std::to_string(id) + "\"}";
        }
        return "{\"ok\":true,\"id\":" + std::to_string(id) + "}";
    }

    else if (action.rfind("taskqueue_", 0) == 0) {
        std::string err;
        std::string resp;
        if (action == "taskqueue_add") {
            resp = tqm_.TaskQueueAdd(JsonGetString(reqJson, "taskqueue", ""),
                                     ExtractRawJsonValue(reqJson, "jobs"),
                                     JsonGetInt(reqJson, "concurrency", 0),
                                     JsonGetString(reqJson, "notify_file", ""), err);
        } else if (action == "taskqueue_list") {
            resp = tqm_.TaskQueueList(JsonGetString(reqJson, "taskqueue", ""),
                                      JsonGetString(reqJson, "state", ""), err);
        } else if (action == "taskqueue_get") {
            resp = tqm_.TaskQueueGet(JsonGetString(reqJson, "id", ""),
                                     JsonGetBool(reqJson, "include_instructions", true),
                                     static_cast<int>(JsonGetInt(reqJson, "rev", 0)), err);
        } else if (action == "taskqueue_retry") {
            resp = tqm_.TaskQueueRetry(JsonGetString(reqJson, "id", ""),
                                       JsonGetBool(reqJson, "reset_attempts", true),
                                       JsonGetBool(reqJson, "refresh_instructions", true), err);
        } else if (action == "taskqueue_update") {
            resp = tqm_.TaskQueueUpdate(JsonGetString(reqJson, "id", ""),
                                        ExtractRawJsonValue(reqJson, "patch"), err);
        } else if (action == "taskqueue_cancel") {
            resp = tqm_.TaskQueueCancel(JsonGetString(reqJson, "id", ""), err);
        } else if (action == "taskqueue_pause") {
            resp = tqm_.TaskQueuePause(JsonGetString(reqJson, "taskqueue", ""), true, err);
        } else if (action == "taskqueue_resume") {
            resp = tqm_.TaskQueuePause(JsonGetString(reqJson, "taskqueue", ""), false, err);
        } else if (action == "taskqueue_wait") {
            resp = tqm_.TaskQueueWait(JsonGetString(reqJson, "taskqueue", ""),
                                      static_cast<int>(JsonGetInt(reqJson, "since_version", 0)),
                                      static_cast<int>(JsonGetInt(reqJson, "timeout_ms", 30000)), err);
        } else if (action == "taskqueue_purge") {
            resp = tqm_.TaskQueuePurge(JsonGetString(reqJson, "taskqueue", ""),
                                       ExtractRawJsonValue(reqJson, "states"), err);
        } else if (action == "taskqueue_link") {
            resp = tqm_.TaskQueueLink(JsonGetString(reqJson, "job", ""),
                                      JsonGetString(reqJson, "depends_on", ""),
                                      JsonGetString(reqJson, "on", ""), err);
        } else if (action == "taskqueue_unlink") {
            resp = tqm_.TaskQueueUnlink(JsonGetString(reqJson, "job", ""),
                                        JsonGetString(reqJson, "depends_on", ""), err);
        } else if (action == "taskqueue_graph") {
            resp = tqm_.TaskQueueGraph(JsonGetString(reqJson, "taskqueue", ""),
                                       JsonGetString(reqJson, "root", ""), err);
        }
        if (!resp.empty()) {
            HWND hwnd = notifyHwnd_.load();
            if (hwnd) PostMessageW(hwnd, WM_PC_IPC_EVENT, 0, 0);
            return resp;
        }
        return "{\"ok\":false,\"error\":\"" + JsonEscape(err.empty() ? "unknown taskqueue action" : err) + "\"}";
    }
    return "{\"ok\":false,\"error\":\"Unknown IPC action: " + JsonEscape(action) + "\"}";
}

} // namespace pc
