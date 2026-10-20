#pragma once

#include "process_manager.h"
#include <thread>
#include <atomic>
#include <string>

namespace pc {

class IpcServer {
public:
    explicit IpcServer(ProcessManager& pm, HWND notifyHwnd = nullptr);
    ~IpcServer();

    bool Start();
    void Stop();
    void SetNotifyHwnd(HWND hwnd) { notifyHwnd_ = hwnd; }

    // Execute a JSON request directly against ProcessManager and return JSON response
    std::string HandleRequestJson(const std::string& reqJson);

private:
    void AcceptLoop();
    void HandleClient(HANDLE hPipe);

    ProcessManager& pm_;
    std::atomic<HWND> notifyHwnd_{nullptr};
    std::atomic<bool> stop_{false};
    std::thread acceptThread_;
};

// Client helper: auto-starts the GUI Dashboard if not running, sends request, returns JSON response
bool SendIpcRequest(const std::string& reqJson, std::string& outRespJson, std::string& outError, bool autoStartGui = true);

// Format ProcessSnapshot as JSON object
std::string SnapshotToJson(const ProcessSnapshot& s);

} // namespace pc
