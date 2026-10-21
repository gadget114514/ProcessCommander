#pragma once

#include "common.h"
#include "TerminalView.h"
#include <mutex>
#include <thread>
#include <memory>
#include <atomic>
#include <functional>

namespace pc {

struct StartProcessRequest {
    std::string name;
    std::string commandLine;
    std::string workingDir;
    std::string logFile;     // optional custom log file path
    std::string doneFile;    // optional custom completion marker file path
    ExecMode mode = ExecMode::Pipe;
    bool openStdin = true;   // pipe mode: keep stdin writable; false closes it after launch so children see EOF
    int timeoutSec = 0;      // 0 = no limit
    int idleTimeoutSec = 0;  // 0 = no limit
};

struct ProcessSnapshot {
    int id = 0;
    std::string name;
    std::string commandLine;
    std::string workingDir;
    ExecMode mode = ExecMode::Pipe;
    ProcState state = ProcState::Running;
    DWORD rootPid = 0;
    DWORD exitCode = STILL_ACTIVE;
    std::string startTimeStr;
    std::string endTimeStr;
    uint64_t startTickMs = 0;
    uint64_t endTickMs = 0;
    uint64_t lastOutputTickMs = 0;
    uint64_t elapsedMs = 0;
    uint64_t idleMs = 0;
    uint64_t cpuTimeMs = 0;
    uint64_t lastCpuActivityTickMs = 0;
    int timeoutSec = 0;
    int idleTimeoutSec = 0;
    uint32_t treeProcessCount = 0;
    uint64_t treeMemoryBytes = 0;
    uint64_t totalBytesRead = 0;
    uint64_t outputVersion = 0;
    std::string lastLinePreview;
    std::string logFilePath;
    std::string doneFilePath;
    HWND terminalHwnd = nullptr;
};

class ProcessManager {
public:
    using CreateTerminalCallback = std::function<TerminalView*(
        const std::wstring& resolvedCmd,
        const std::wstring& cwd,
        std::function<void(const char*, size_t)> onOutput,
        std::wstring& outError)>;
    using DestroyTerminalCallback = std::function<void(TerminalView*)>;

    ProcessManager();
    ~ProcessManager();

    void SetTerminalCallbacks(CreateTerminalCallback createCb, DestroyTerminalCallback destroyCb) {
        createTerminalCb_ = std::move(createCb);
        destroyTerminalCb_ = std::move(destroyCb);
    }

    // Start a new managed process. Returns assigned process ID (>0) or -1 on error.
    int StartProcess(const StartProcessRequest& req, std::string& outError);

    // Send input string to stdin / ConPTY of process
    bool SendInput(int id, const std::string& text, bool appendNewline, std::string& outError);

    // Terminate process tree via Job Object
    bool KillProcess(int id, std::string& outError);

    // Terminate all running processes
    int KillAllRunning();

    // Remove finished processes from list
    int ClearFinished();
    bool RemoveProcess(int id);

    // Renumber all processes sequentially from #1 (requires no running processes).
    // Returns the number of renumbered processes, or -1 on error.
    int ResetProcessIds(std::string& outError);

    // Bring separate console window to foreground
    bool FocusProcessWindow(int id);

    // Get TerminalView* for a process ID (returns nullptr if not a ConPTY process)
    TerminalView* GetTerminalView(int id);

    // Get snapshot of a single process
    bool GetSnapshot(int id, ProcessSnapshot& outSnap);

    // Get snapshots of all managed processes
    std::vector<ProcessSnapshot> GetAllSnapshots();

    // Get process output log (reads from TerminalBuffer for ConPTY processes)
    bool GetOutput(int id, bool stripAnsi, int tailLines, std::string& outText, uint64_t* outVersion = nullptr);

    // Wait until a process finishes or maxWaitMs elapses (returns true if finished)
    bool WaitForExit(int id, uint32_t maxWaitMs);

private:
    struct ManagedProcess {
        int id = 0;
        std::string name;
        std::string commandLine;
        std::string workingDir;
        ExecMode mode = ExecMode::Pipe;
        std::atomic<ProcState> state{ProcState::Running};

        DWORD rootPid = 0;
        std::atomic<DWORD> exitCode{STILL_ACTIVE};

        HANDLE hProcess = nullptr;
        HANDLE hThread = nullptr;
        HANDLE hJob = nullptr;
        HANDLE hStdinWrite = nullptr;
        HANDLE hStdoutRead = nullptr;

        // Pseudo-PTY Terminal (src/Terminal + include)
        TerminalView* terminalView = nullptr;
        DestroyTerminalCallback destroyTerminalCb;

        // Headless ConPTY fallback when GUI callbacks are not registered
        std::unique_ptr<ConPtySession> headlessPty;
        std::unique_ptr<TerminalBuffer> headlessBuffer;
        std::unique_ptr<TerminalEmulator> headlessEmulator;

        std::string startTimeStr;
        std::string endTimeStr;
        uint64_t startTickMs = 0;
        uint64_t restoredElapsedMs = 0;
        std::atomic<uint64_t> endTickMs{0};
        std::atomic<uint64_t> lastOutputTickMs{0};
        std::atomic<uint64_t> cpuTime100ns{0};
        std::atomic<uint64_t> lastCpuActivityTickMs{0};
        int timeoutSec = 0;
        int idleTimeoutSec = 0;

        std::atomic<uint32_t> treeProcessCount{0};
        std::atomic<uint64_t> treeMemoryBytes{0};

        std::mutex outputMutex;
        std::string rawOutput;
        std::string cleanOutput;
        std::string lastLinePreview;
        std::string logFilePath;
        std::wstring logFilePathWide;
        std::string doneFilePath;
        std::wstring doneFilePathWide;
        std::wstring defaultDoneFilePathWide;
        uint64_t totalBytesRead = 0;
        uint64_t outputVersion = 0;

        std::thread readerThread;

        ~ManagedProcess();
    };

    void ReaderThreadFunc(std::shared_ptr<ManagedProcess> proc);
    void MonitorThreadFunc();
    void UpdateJobMetrics(ManagedProcess& proc);
    void TerminateJobInternal(ManagedProcess& proc, ProcState finalState, const std::string& reasonMsg);
    static void WriteDoneFile(ManagedProcess& proc, ProcState finalState, DWORD exitCode, const std::string& endTimeStr, uint64_t elapsedMs, const std::string& reasonMsg);
    static ProcessSnapshot BuildSnapshot(ManagedProcess& p, uint64_t now);
    void SaveState();
    void LoadState();

    std::mutex mutex_;
    std::vector<std::shared_ptr<ManagedProcess>> processes_;
    int nextId_ = 1;
    std::atomic<bool> stopMonitor_{false};
    std::thread monitorThread_;
    CreateTerminalCallback createTerminalCb_;
    DestroyTerminalCallback destroyTerminalCb_;
};

} // namespace pc
