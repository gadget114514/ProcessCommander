#include "ipc_server.h"
#include <sstream>

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
        << "\"last_line\":\"" << JsonEscape(s.lastLinePreview) << "\""
        << "}";
    return oss.str();
}

IpcServer::IpcServer(ProcessManager& pm, HWND notifyHwnd)
    : pm_(pm), notifyHwnd_(notifyHwnd) {}

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
    HANDLE hDummy = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hDummy != INVALID_HANDLE_VALUE) {
        CloseHandle(hDummy);
    }
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
}

void IpcServer::AcceptLoop() {
    while (!stop_) {
        // [SECURITY] Use PIPE_REJECT_REMOTE_CLIENTS to prevent unauthorized remote connections.
        // FILE_FLAG_FIRST_PIPE_INSTANCE cannot be used here because we need unlimited instances
        // in this accept loop.
        HANDLE hPipe = CreateNamedPipeW(
            kPipeName,
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
        bool waitFinish = JsonGetBool(reqJson, "wait", false);
        int waitMs = static_cast<int>(JsonGetInt(reqJson, "wait_ms", 0));

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

    return "{\"ok\":false,\"error\":\"Unknown IPC action: " + JsonEscape(action) + "\"}";
}

} // namespace pc
