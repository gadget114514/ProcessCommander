#include "process_manager.h"
#include <psapi.h>
#include <sstream>
#include <algorithm>

namespace pc {

static std::wstring ResolveCommandLineForWin32(const std::string& rawCmdUtf8) {
    std::wstring cmd = Utf8ToWide(rawCmdUtf8);
    size_t start = cmd.find_first_not_of(L" \t");
    if (start == std::wstring::npos) return L"";
    cmd = cmd.substr(start);

    std::wstring prog;
    if (cmd[0] == L'"') {
        size_t endQuote = cmd.find(L'"', 1);
        if (endQuote != std::wstring::npos) {
            prog = cmd.substr(1, endQuote - 1);
        } else {
            prog = cmd.substr(1);
        }
    } else {
        size_t space = cmd.find_first_of(L" \t");
        prog = (space == std::wstring::npos) ? cmd : cmd.substr(0, space);
    }

    auto endsWithCI = [](const std::wstring& s, const std::wstring& suffix) {
        if (s.size() < suffix.size()) return false;
        return _wcsicmp(s.c_str() + s.size() - suffix.size(), suffix.c_str()) == 0;
    };

    if (endsWithCI(prog, L".cmd") || endsWithCI(prog, L".bat")) {
        return L"cmd.exe /d /s /c \"" + cmd + L"\"";
    }

    if (!endsWithCI(prog, L".exe")) {
        wchar_t foundBuf[MAX_PATH] = {};
        if (SearchPathW(nullptr, prog.c_str(), L".exe", MAX_PATH, foundBuf, nullptr) == 0) {
            if (SearchPathW(nullptr, prog.c_str(), L".cmd", MAX_PATH, foundBuf, nullptr) > 0 ||
                SearchPathW(nullptr, prog.c_str(), L".bat", MAX_PATH, foundBuf, nullptr) > 0) {
                return L"cmd.exe /d /s /c \"" + cmd + L"\"";
            }
        }
    }

    return cmd;
}

static std::vector<wchar_t> BuildChildEnvironment(const std::wstring& cwd) {
    std::vector<wchar_t> block;
    auto append = [&block](const std::wstring& entry) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    };

    LPWCH env = GetEnvironmentStringsW();
    if (env) {
        for (LPWCH p = env; *p; ) {
            std::wstring entry(p);
            p += entry.size() + 1;
            if (entry[0] != L'=') {
                size_t eq = entry.find(L'=');
                std::wstring key = (eq == std::wstring::npos) ? entry : entry.substr(0, eq);
                if (_wcsicmp(key.c_str(), L"PWD") == 0 ||
                    _wcsicmp(key.c_str(), L"OLDPWD") == 0 ||
                    _wcsicmp(key.c_str(), L"INIT_CWD") == 0) {
                    continue;
                }
            }
            append(entry);
        }
        FreeEnvironmentStringsW(env);
    }

    if (!cwd.empty()) {
        append(L"PWD=" + cwd);
        append(L"INIT_CWD=" + cwd);
    }
    block.push_back(L'\0');
    return block;
}

ProcessManager::ManagedProcess::~ManagedProcess() {
    if (hStdinWrite) {
        CloseHandle(hStdinWrite);
        hStdinWrite = nullptr;
    }
    if (hStdoutRead) {
        CloseHandle(hStdoutRead);
        hStdoutRead = nullptr;
    }
    if (readerThread.joinable()) {
        readerThread.join();
    }
    if (terminalView) {
        if (destroyTerminalCb) {
            destroyTerminalCb(terminalView);
        } else {
            delete terminalView;
        }
        terminalView = nullptr;
        hProcess = nullptr;
        hThread = nullptr;
    }
    if (headlessPty) {
        headlessPty->Close();
        headlessPty.reset();
        hProcess = nullptr;
        hThread = nullptr;
    }
    if (hThread) {
        CloseHandle(hThread);
        hThread = nullptr;
    }
    if (hProcess) {
        CloseHandle(hProcess);
        hProcess = nullptr;
    }
    if (hJob) {
        CloseHandle(hJob);
        hJob = nullptr;
    }
}

ProcessManager::ProcessManager() {
    LoadState();
    monitorThread_ = std::thread(&ProcessManager::MonitorThreadFunc, this);
}

ProcessManager::~ProcessManager() {
    stopMonitor_ = true;
    if (monitorThread_.joinable()) {
        monitorThread_.join();
    }
    KillAllRunning();
    SaveState();
    std::lock_guard<std::mutex> lock(mutex_);
    processes_.clear();
}

int ProcessManager::StartProcess(const StartProcessRequest& req, std::string& outError) {
    if (req.commandLine.empty()) {
        outError = "Command line cannot be empty.";
        return -1;
    }

    std::wstring resolvedCmd = ResolveCommandLineForWin32(req.commandLine);
    if (resolvedCmd.empty()) {
        outError = "Failed to resolve command line.";
        return -1;
    }

    std::wstring cwdWide = ResolveAbsolutePathWide(Utf8ToWide(req.workingDir));
    const wchar_t* cwdPtr = cwdWide.c_str();
    std::vector<wchar_t> childEnv = BuildChildEnvironment(cwdWide);

    // Create Win32 Job Object with KILL_ON_JOB_CLOSE
    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    if (!hJob) {
        outError = "CreateJobObjectW failed (error " + std::to_string(GetLastError()) + ")";
        return -1;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));

    auto proc = std::make_shared<ManagedProcess>();
    proc->commandLine = req.commandLine;
    proc->workingDir = WideToUtf8(cwdWide);
    proc->mode = req.mode;
    proc->timeoutSec = req.timeoutSec;
    proc->idleTimeoutSec = req.idleTimeoutSec;
    proc->hJob = hJob;
    proc->destroyTerminalCb = destroyTerminalCb_;

    uint64_t now = GetNowTickMs();
    proc->startTimeStr = GetCurrentTimestampStr();
    proc->startTickMs = now;
    proc->lastOutputTickMs = now;
    proc->lastCpuActivityTickMs = now;

    if (req.mode == ExecMode::ConPTY) {
        std::weak_ptr<ManagedProcess> weakProc = proc;
        auto outputObserver = [weakProc](const char* data, size_t len) {
            auto sp = weakProc.lock();
            if (!sp || len == 0) return;
            sp->lastOutputTickMs = GetNowTickMs();
            std::string chunk(data, len);
            std::string cleanChunk = StripAnsiCodes(chunk);
            std::wstring logPathW;
            {
                std::lock_guard<std::mutex> outLock(sp->outputMutex);
                sp->rawOutput.append(chunk);
                sp->cleanOutput.append(cleanChunk);
                sp->totalBytesRead += len;
                sp->outputVersion++;
                logPathW = sp->logFilePathWide;
                constexpr size_t kMaxRawBytes = 2 * 1024 * 1024;
                if (sp->rawOutput.size() > kMaxRawBytes) {
                    sp->rawOutput.erase(0, sp->rawOutput.size() - kMaxRawBytes);
                }
                if (sp->cleanOutput.size() > kMaxRawBytes) {
                    sp->cleanOutput.erase(0, sp->cleanOutput.size() - kMaxRawBytes);
                }
            }
            if (!logPathW.empty() && !cleanChunk.empty()) {
                AppendToLogFileWide(logPathW, cleanChunk);
            }
        };

        if (createTerminalCb_) {
            std::wstring werr;
            TerminalView* tv = createTerminalCb_(resolvedCmd, cwdWide, outputObserver, werr);
            if (!tv) {
                outError = "TerminalView ConPTY start failed: " + WideToUtf8(werr);
                return -1;
            }
            proc->terminalView = tv;
            proc->hProcess = tv->GetProcessHandle();
            proc->rootPid = tv->ProcessId();
        } else {
            // Headless ConPtySession + TerminalEmulator + TerminalBuffer fallback
            proc->headlessBuffer = std::make_unique<TerminalBuffer>(120, 40);
            proc->headlessEmulator = std::make_unique<TerminalEmulator>();
            proc->headlessEmulator->reset(proc->headlessBuffer.get());
            proc->headlessPty = std::make_unique<ConPtySession>();
            proc->headlessPty->SetWorkingDirectory(cwdWide);
            proc->headlessEmulator->setResponseCallback([weakProc](const std::wstring& resp) {
                if (auto sp = weakProc.lock()) {
                    if (sp->headlessPty) sp->headlessPty->WriteFront(resp);
                }
            });
            proc->headlessPty->SetOutputCallback([weakProc, outputObserver](const char* data, size_t len) {
                outputObserver(data, len);
                auto sp = weakProc.lock();
                if (!sp) return;
                std::wstring ws = Utf8ToWide(std::string(data, len));
                std::lock_guard<std::mutex> outLock(sp->outputMutex);
                if (sp->headlessEmulator) {
                    sp->headlessEmulator->process(ws);
                }
            });
            if (!proc->headlessPty->Start(resolvedCmd, 120, 40)) {
                outError = "ConPtySession::Start failed: " + WideToUtf8(proc->headlessPty->LastError());
                return -1;
            }
            proc->hProcess = proc->headlessPty->GetProcessHandle();
            proc->rootPid = proc->headlessPty->ProcessId();
        }

        if (proc->hProcess) {
            AssignProcessToJobObject(proc->hJob, proc->hProcess);
        }
        proc->treeProcessCount = 1;
    } else {
        PROCESS_INFORMATION pi = {};
        BOOL created = FALSE;
        DWORD lastErr = 0;

        std::vector<wchar_t> cmdMutable(resolvedCmd.begin(), resolvedCmd.end());
        cmdMutable.push_back(L'\0');

        if (req.mode == ExecMode::Pipe) {
            SECURITY_ATTRIBUTES sa = {};
            sa.nLength = sizeof(SECURITY_ATTRIBUTES);
            sa.bInheritHandle = TRUE;

            HANDLE hChildStdinRead = nullptr;
            HANDLE hChildStdoutWrite = nullptr;

            if (!CreatePipe(&hChildStdinRead, &proc->hStdinWrite, &sa, 0) ||
                !CreatePipe(&proc->hStdoutRead, &hChildStdoutWrite, &sa, 0)) {
                outError = "Failed to create anonymous pipes.";
                return -1;
            }

            SetHandleInformation(proc->hStdinWrite, HANDLE_FLAG_INHERIT, 0);
            SetHandleInformation(proc->hStdoutRead, HANDLE_FLAG_INHERIT, 0);

            STARTUPINFOW si = {};
            si.cb = sizeof(STARTUPINFOW);
            si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            si.hStdInput = hChildStdinRead;
            si.hStdOutput = hChildStdoutWrite;
            si.hStdError = hChildStdoutWrite;

            created = CreateProcessW(
                nullptr,
                cmdMutable.data(),
                nullptr,
                nullptr,
                TRUE,
                CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                childEnv.data(),
                cwdPtr,
                &si,
                &pi
            );
            lastErr = GetLastError();

            CloseHandle(hChildStdinRead);
            CloseHandle(hChildStdoutWrite);

        } else { // ExecMode::Console
            STARTUPINFOW si = {};
            si.cb = sizeof(STARTUPINFOW);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_SHOW;

            created = CreateProcessW(
                nullptr,
                cmdMutable.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NEW_CONSOLE | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                childEnv.data(),
                cwdPtr,
                &si,
                &pi
            );
            lastErr = GetLastError();
        }

        if (!created) {
            outError = "CreateProcessW failed (Win32 error " + std::to_string(lastErr) + ") for command: " + req.commandLine;
            return -1;
        }

        if (req.mode == ExecMode::Pipe && !req.openStdin && proc->hStdinWrite) {
            CloseHandle(proc->hStdinWrite);
            proc->hStdinWrite = nullptr;
        }

        AssignProcessToJobObject(proc->hJob, pi.hProcess);
        ResumeThread(pi.hThread);

        proc->hProcess = pi.hProcess;
        proc->hThread = pi.hThread;
        proc->rootPid = pi.dwProcessId;
        proc->treeProcessCount = 1;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        proc->id = nextId_++;
        if (req.name.empty()) {
            proc->name = "proc-" + std::to_string(proc->id);
        } else {
            proc->name = req.name;
        }

        std::string safeName = SanitizeFileNameComponent(proc->name);
        std::string fileTs = GetFileTimestampStr();
        std::wstring logsDirW = GetLogsDirectoryWide();

        if (!req.logFile.empty()) {
            proc->logFilePathWide = Utf8ToWide(req.logFile);
            proc->logFilePath = req.logFile;
        } else {
            proc->logFilePathWide = logsDirW + L"\\proc_" + std::to_wstring(proc->id) + L"_" + Utf8ToWide(safeName) + L"_" + Utf8ToWide(fileTs) + L".log";
            proc->logFilePath = WideToUtf8(proc->logFilePathWide);
        }

        proc->defaultDoneFilePathWide = logsDirW + L"\\proc_" + std::to_wstring(proc->id) + L".done";
        if (!req.doneFile.empty()) {
            proc->doneFilePathWide = Utf8ToWide(req.doneFile);
            proc->doneFilePath = req.doneFile;
        } else {
            proc->doneFilePathWide = proc->defaultDoneFilePathWide;
            proc->doneFilePath = WideToUtf8(proc->doneFilePathWide);
        }

        // Ensure any stale completion marker files do NOT exist while the process is running
        DeleteFileW(proc->defaultDoneFilePathWide.c_str());
        if (proc->doneFilePathWide != proc->defaultDoneFilePathWide) {
            DeleteFileW(proc->doneFilePathWide.c_str());
        }

        {
            std::ostringstream hdr;
            hdr << "================================================================================\r\n"
                << "ProcessCommander Process Log\r\n"
                << "ID          : #" << proc->id << "\r\n"
                << "Name        : " << proc->name << "\r\n"
                << "Command     : " << proc->commandLine << "\r\n"
                << "Working Dir : " << (proc->workingDir.empty() ? "(default)" : proc->workingDir) << "\r\n"
                << "Mode        : " << ExecModeToString(proc->mode) << "\r\n"
                << "Root PID    : " << proc->rootPid << "\r\n"
                << "Start Time  : " << proc->startTimeStr << "\r\n"
                << "Log File    : " << proc->logFilePath << "\r\n"
                << "Done File   : " << proc->doneFilePath << "\r\n"
                << "================================================================================\r\n\r\n";

            std::string earlyClean;
            {
                std::lock_guard<std::mutex> outLock(proc->outputMutex);
                earlyClean = proc->cleanOutput;
            }
            AppendToLogFileWide(proc->logFilePathWide, hdr.str() + earlyClean);
        }

        WriteAuditLog("PROCESS_START",
                      "Started #" + std::to_string(proc->id) + " [" + proc->name + "] PID=" + std::to_string(proc->rootPid) +
                      " Mode=" + ExecModeToString(proc->mode) + " Cmd=\"" + proc->commandLine +
                      "\" Log=\"" + proc->logFilePath + "\" DoneFile=\"" + proc->doneFilePath + "\"");

        if (proc->mode == ExecMode::Console) {
            std::lock_guard<std::mutex> outLock(proc->outputMutex);
            std::string msg = "[ProcessCommander] Started in separate Console Window (PID " + std::to_string(proc->rootPid) + ")\n"
                              "Command: " + proc->commandLine + "\n";
            proc->rawOutput = msg;
            proc->cleanOutput = msg;
            proc->lastLinePreview = "Running in separate Console window (PID " + std::to_string(proc->rootPid) + ")";
            proc->outputVersion++;
            AppendToLogFileWide(proc->logFilePathWide, msg);
        } else if (proc->mode == ExecMode::Pipe && proc->hStdoutRead) {
            proc->readerThread = std::thread(&ProcessManager::ReaderThreadFunc, this, proc);
        }

        processes_.push_back(proc);
    }
    SaveState();
    return proc->id;
}

void ProcessManager::ReaderThreadFunc(std::shared_ptr<ManagedProcess> proc) {
    char buffer[4096];
    DWORD bytesRead = 0;
    while (proc->hStdoutRead) {
        BOOL ok = ReadFile(proc->hStdoutRead, buffer, sizeof(buffer), &bytesRead, nullptr);
        if (!ok || bytesRead == 0) {
            break;
        }
        uint64_t now = GetNowTickMs();
        proc->lastOutputTickMs = now;

        std::string chunk(buffer, bytesRead);
        std::string cleanChunk = StripAnsiCodes(chunk);
        std::wstring logPathW;

        {
            std::lock_guard<std::mutex> outLock(proc->outputMutex);
            proc->rawOutput.append(chunk);
            proc->cleanOutput.append(cleanChunk);
            proc->totalBytesRead += bytesRead;
            proc->outputVersion++;
            logPathW = proc->logFilePathWide;

            constexpr size_t kMaxLogBytes = 2 * 1024 * 1024;
            if (proc->cleanOutput.size() > kMaxLogBytes) {
                proc->cleanOutput.erase(0, proc->cleanOutput.size() - kMaxLogBytes);
            }
            if (proc->rawOutput.size() > kMaxLogBytes) {
                proc->rawOutput.erase(0, proc->rawOutput.size() - kMaxLogBytes);
            }

            std::string preview = GetLastNonEmptyLine(proc->cleanOutput);
            if (!preview.empty()) {
                proc->lastLinePreview = preview;
            }
        }
        if (!logPathW.empty() && !cleanChunk.empty()) {
            AppendToLogFileWide(logPathW, cleanChunk);
        }
    }
}

void ProcessManager::UpdateJobMetrics(ManagedProcess& proc) {
    if (!proc.hJob) return;

    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acct = {};
    if (QueryInformationJobObject(proc.hJob, JobObjectBasicAccountingInformation, &acct, sizeof(acct), nullptr)) {
        uint64_t total100ns = static_cast<uint64_t>(acct.TotalUserTime.QuadPart) +
                              static_cast<uint64_t>(acct.TotalKernelTime.QuadPart);
        uint64_t prev = proc.cpuTime100ns.exchange(total100ns);
        if (total100ns > prev) {
            proc.lastCpuActivityTickMs = GetNowTickMs();
        }
    }

    alignas(JOBOBJECT_BASIC_PROCESS_ID_LIST) uint8_t buf[sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + sizeof(ULONG_PTR) * 256];
    auto* pidList = reinterpret_cast<PJOBOBJECT_BASIC_PROCESS_ID_LIST>(buf);
    if (QueryInformationJobObject(proc.hJob, JobObjectBasicProcessIdList, pidList, sizeof(buf), nullptr)) {
        proc.treeProcessCount = static_cast<uint32_t>(pidList->NumberOfProcessIdsInList);
        uint64_t totalMem = 0;
        for (DWORD i = 0; i < pidList->NumberOfProcessIdsInList; ++i) {
            DWORD pid = static_cast<DWORD>(pidList->ProcessIdList[i]);
            HANDLE hP = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
            if (hP) {
                PROCESS_MEMORY_COUNTERS pmc = {};
                pmc.cb = sizeof(pmc);
                if (GetProcessMemoryInfo(hP, &pmc, sizeof(pmc))) {
                    totalMem += pmc.WorkingSetSize;
                }
                CloseHandle(hP);
            }
        }
        proc.treeMemoryBytes = totalMem;
    }
}

void ProcessManager::WriteDoneFile(ManagedProcess& proc, ProcState finalState, DWORD exitCode,
                                   const std::string& endTimeStr, uint64_t elapsedMs, const std::string& reasonMsg) {
    std::ostringstream oss;
    oss << "{\r\n"
        << "  \"id\": " << proc.id << ",\r\n"
        << "  \"name\": \"" << JsonEscape(proc.name) << "\",\r\n"
        << "  \"command\": \"" << JsonEscape(proc.commandLine) << "\",\r\n"
        << "  \"state\": \"" << ProcStateToString(finalState) << "\",\r\n"
        << "  \"exit_code\": " << exitCode << ",\r\n"
        << "  \"pid\": " << proc.rootPid << ",\r\n"
        << "  \"start_time\": \"" << JsonEscape(proc.startTimeStr) << "\",\r\n"
        << "  \"end_time\": \"" << JsonEscape(endTimeStr) << "\",\r\n"
        << "  \"elapsed_ms\": " << elapsedMs << ",\r\n"
        << "  \"cpu_time_ms\": " << (proc.cpuTime100ns.load() / 10000) << ",\r\n"
        << "  \"uptime_str\": \"" << JsonEscape(FormatUptimeHMS(elapsedMs)) << "\",\r\n"
        << "  \"reason\": \"" << JsonEscape(reasonMsg) << "\",\r\n"
        << "  \"log_file\": \"" << JsonEscape(proc.logFilePath) << "\",\r\n"
        << "  \"done_file\": \"" << JsonEscape(proc.doneFilePath) << "\"\r\n"
        << "}\r\n";

    std::string payload = oss.str();
    if (!proc.defaultDoneFilePathWide.empty()) {
        WriteFileAtomicWide(proc.defaultDoneFilePathWide, payload);
    }
    if (!proc.doneFilePathWide.empty() && proc.doneFilePathWide != proc.defaultDoneFilePathWide) {
        WriteFileAtomicWide(proc.doneFilePathWide, payload);
    }
}

void ProcessManager::TerminateJobInternal(ManagedProcess& proc, ProcState finalState, const std::string& reasonMsg) {
    if (proc.hJob) {
        TerminateJobObject(proc.hJob, 1);
    } else if (proc.hProcess) {
        TerminateProcess(proc.hProcess, 1);
    }

    if (proc.hStdinWrite) {
        CloseHandle(proc.hStdinWrite);
        proc.hStdinWrite = nullptr;
    }

    uint64_t endNow = GetNowTickMs();
    proc.endTickMs = endNow;
    proc.exitCode = 1;
    proc.state = finalState;
    proc.treeProcessCount = 0;
    proc.treeMemoryBytes = 0;

    uint64_t elapsedMs = (endNow >= proc.startTickMs) ? (endNow - proc.startTickMs) : 0;
    std::string endTimeStr = GetCurrentTimestampStr();
    proc.endTimeStr = endTimeStr;
    proc.restoredElapsedMs = elapsedMs;

    if (!reasonMsg.empty()) {
        std::lock_guard<std::mutex> outLock(proc.outputMutex);
        std::string line = "\n[ProcessCommander] " + reasonMsg + "\n";
        proc.rawOutput += line;
        proc.cleanOutput += line;
        proc.lastLinePreview = reasonMsg;
        proc.outputVersion++;
    }

    if (!proc.logFilePathWide.empty()) {
        std::ostringstream ftr;
        ftr << "\r\n================================================================================\r\n"
            << "[ProcessCommander] Process Terminated (" << ProcStateToString(finalState) << ")\r\n"
            << "Reason      : " << reasonMsg << "\r\n"
            << "End Time    : " << endTimeStr << "\r\n"
            << "Uptime      : " << FormatUptimeHMS(elapsedMs) << " (" << FormatDurationSec(elapsedMs) << ")\r\n"
            << "Done File   : " << proc.doneFilePath << "\r\n";
        if (proc.mode == ExecMode::ConPTY && proc.terminalView) {
            std::string screenSnap = WideToUtf8(proc.terminalView->ExtractScreenText(0));
            if (!screenSnap.empty()) {
                ftr << "------------------------ Final Terminal Screen Snapshot ------------------------\r\n"
                    << screenSnap
                    << "--------------------------------------------------------------------------------\r\n";
            }
        }
        ftr << "================================================================================\r\n";
        AppendToLogFileWide(proc.logFilePathWide, ftr.str());
    }

    WriteDoneFile(proc, finalState, 1, endTimeStr, elapsedMs, reasonMsg);

    WriteAuditLog("PROCESS_TERMINATED",
                  "Process #" + std::to_string(proc.id) + " [" + proc.name + "] State=" + ProcStateToString(finalState) +
                  " Uptime=" + FormatUptimeHMS(elapsedMs) + " Reason=\"" + reasonMsg + "\" DoneFile=\"" + proc.doneFilePath + "\"");
}

void ProcessManager::MonitorThreadFunc() {
    while (!stopMonitor_) {
        std::vector<std::shared_ptr<ManagedProcess>> copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            copy = processes_;
        }

        uint64_t now = GetNowTickMs();
        for (auto& proc : copy) {
            if (proc->state != ProcState::Running) continue;

            if (proc->hProcess) {
                DWORD waitRes = WaitForSingleObject(proc->hProcess, 0);
                if (waitRes == WAIT_OBJECT_0) {
                    DWORD code = 0;
                    GetExitCodeProcess(proc->hProcess, &code);
                    proc->exitCode = code;
                    proc->endTickMs = now;
                    ProcState finalSt = (code == 0) ? ProcState::Completed : ProcState::Failed;
                    proc->state = finalSt;

                    if (proc->hStdinWrite) {
                        CloseHandle(proc->hStdinWrite);
                        proc->hStdinWrite = nullptr;
                    }
                    proc->treeProcessCount = 0;
                    proc->treeMemoryBytes = 0;

                    if (proc->mode == ExecMode::Console) {
                        std::lock_guard<std::mutex> outLock(proc->outputMutex);
                        std::string msg = "[ProcessCommander] Console process exited with code " + std::to_string(code) + "\n";
                        proc->cleanOutput += msg;
                        proc->rawOutput += msg;
                        proc->lastLinePreview = "Exited (code " + std::to_string(code) + ")";
                        proc->outputVersion++;
                    }

                    uint64_t elapsedMs = (now >= proc->startTickMs) ? (now - proc->startTickMs) : 0;
                    std::string endTimeStr = GetCurrentTimestampStr();
                    proc->endTimeStr = endTimeStr;
                    proc->restoredElapsedMs = elapsedMs;
                    if (!proc->logFilePathWide.empty()) {
                        std::ostringstream ftr;
                        ftr << "\r\n================================================================================\r\n"
                            << "[ProcessCommander] Process Exited (" << ProcStateToString(finalSt) << ")\r\n"
                            << "Exit Code   : " << code << "\r\n"
                            << "End Time    : " << endTimeStr << "\r\n"
                            << "Uptime      : " << FormatUptimeHMS(elapsedMs) << " (" << FormatDurationSec(elapsedMs) << ")\r\n"
                            << "Done File   : " << proc->doneFilePath << "\r\n";
                        if (proc->mode == ExecMode::ConPTY && proc->terminalView) {
                            std::string screenSnap = WideToUtf8(proc->terminalView->ExtractScreenText(0));
                            if (!screenSnap.empty()) {
                                ftr << "------------------------ Final Terminal Screen Snapshot ------------------------\r\n"
                                    << screenSnap
                                    << "--------------------------------------------------------------------------------\r\n";
                            }
                        }
                        ftr << "================================================================================\r\n";
                        AppendToLogFileWide(proc->logFilePathWide, ftr.str());
                    }

                    WriteDoneFile(*proc, finalSt, code, endTimeStr, elapsedMs, "Exited normally");

                    WriteAuditLog("PROCESS_EXIT",
                                  "Process #" + std::to_string(proc->id) + " [" + proc->name + "] exited with code " +
                                  std::to_string(code) + " Uptime=" + FormatUptimeHMS(elapsedMs) +
                                  " DoneFile=\"" + proc->doneFilePath + "\"");
                    SaveState();
                    continue;
                }
            }

            UpdateJobMetrics(*proc);

            if (proc->timeoutSec > 0) {
                uint64_t elapsedSec = (now - proc->startTickMs) / 1000;
                if (elapsedSec >= static_cast<uint64_t>(proc->timeoutSec)) {
                    TerminateJobInternal(*proc, ProcState::TimedOut,
                        "Process tree terminated: Exceeded runtime timeout (" + std::to_string(proc->timeoutSec) + "s)");
                    SaveState();
                    continue;
                }
            }

            if (proc->idleTimeoutSec > 0) {
                uint64_t lastActivity = (std::max)(proc->lastOutputTickMs.load(), proc->lastCpuActivityTickMs.load());
                uint64_t idleSec = (now - lastActivity) / 1000;
                if (idleSec >= static_cast<uint64_t>(proc->idleTimeoutSec)) {
                    TerminateJobInternal(*proc, ProcState::IdleTimedOut,
                        "Process tree terminated: Exceeded idle/hang timeout (" + std::to_string(proc->idleTimeoutSec) + "s without output or CPU activity)");
                    SaveState();
                    continue;
                }
            }
        }

        Sleep(200);
    }
}

bool ProcessManager::SendInput(int id, const std::string& text, bool appendNewline, std::string& outError) {
    std::shared_ptr<ManagedProcess> proc;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& p : processes_) {
            if (p->id == id) {
                proc = p;
                break;
            }
        }
    }
    if (!proc) {
        outError = "Process ID " + std::to_string(id) + " not found.";
        return false;
    }
    if (proc->state != ProcState::Running) {
        outError = "Process ID " + std::to_string(id) + " is not running.";
        return false;
    }

    if (proc->mode == ExecMode::ConPTY) {
        std::string payload = text;
        if (appendNewline) payload += "\r\n";
        bool sent = false;
        if (proc->terminalView) {
            proc->terminalView->SendInput(payload);
            sent = true;
        } else if (proc->headlessPty) {
            proc->headlessPty->Write(payload);
            sent = true;
        }
        if (sent) {
            proc->lastOutputTickMs = GetNowTickMs();
            if (!proc->logFilePathWide.empty()) {
                std::string inputLog = "\r\n[Input @ " + GetCurrentTimestampStr() + "] > " + text + "\r\n";
                AppendToLogFileWide(proc->logFilePathWide, inputLog);
            }
            WriteAuditLog("PROCESS_INPUT", "Sent input to #" + std::to_string(id) + " [" + proc->name + "]: \"" + JsonEscape(text) + "\"");
            return true;
        }
        outError = "ConPTY session is not active.";
        return false;
    }

    if (!proc->hStdinWrite) {
        outError = "Process ID " + std::to_string(id) + " does not have a writable stdin pipe.";
        return false;
    }

    std::string payload = text;
    if (appendNewline) payload += "\n";

    DWORD written = 0;
    if (!WriteFile(proc->hStdinWrite, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr)) {
        outError = "WriteFile to process stdin failed (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    FlushFileBuffers(proc->hStdinWrite);

    {
        std::lock_guard<std::mutex> outLock(proc->outputMutex);
        std::string echo = "\n> " + text + "\n";
        proc->cleanOutput += echo;
        proc->rawOutput += echo;
        proc->outputVersion++;
    }
    if (!proc->logFilePathWide.empty()) {
        std::string inputLog = "\r\n[Input @ " + GetCurrentTimestampStr() + "] > " + text + "\r\n";
        AppendToLogFileWide(proc->logFilePathWide, inputLog);
    }
    WriteAuditLog("PROCESS_INPUT", "Sent input to #" + std::to_string(id) + " [" + proc->name + "]: \"" + JsonEscape(text) + "\"");
    proc->lastOutputTickMs = GetNowTickMs();
    return true;
}

bool ProcessManager::KillProcess(int id, std::string& outError) {
    std::shared_ptr<ManagedProcess> proc;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& p : processes_) {
            if (p->id == id) {
                proc = p;
                break;
            }
        }
    }
    if (!proc) {
        outError = "Process ID " + std::to_string(id) + " not found.";
        return false;
    }
    if (proc->state != ProcState::Running) {
        return true;
    }
    TerminateJobInternal(*proc, ProcState::Killed, "Process tree killed via Job Object.");
    SaveState();
    return true;
}

int ProcessManager::KillAllRunning() {
    std::vector<std::shared_ptr<ManagedProcess>> copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        copy = processes_;
    }
    int count = 0;
    for (auto& proc : copy) {
        if (proc->state == ProcState::Running) {
            TerminateJobInternal(*proc, ProcState::Killed, "Process tree killed (Kill All).");
            count++;
        }
    }
    if (count > 0) SaveState();
    return count;
}

int ProcessManager::ClearFinished() {
    std::vector<std::shared_ptr<ManagedProcess>> toDestroy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = processes_.begin();
        while (it != processes_.end()) {
            if ((*it)->state != ProcState::Running) {
                toDestroy.push_back(std::move(*it));
                it = processes_.erase(it);
            } else {
                ++it;
            }
        }
    }
    SaveState();
    return static_cast<int>(toDestroy.size());
}

bool ProcessManager::RemoveProcess(int id) {
    std::shared_ptr<ManagedProcess> removed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = processes_.begin(); it != processes_.end(); ++it) {
            if ((*it)->id == id && (*it)->state != ProcState::Running) {
                removed = std::move(*it);
                processes_.erase(it);
                break;
            }
        }
    }
    if (removed) {
        SaveState();
        return true;
    }
    return false;
}

int ProcessManager::ResetProcessIds(std::string& outError) {
    int count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& proc : processes_) {
            if (proc->state == ProcState::Running) {
                outError = "Cannot reset process IDs while processes are running. Stop them first.";
                return -1;
            }
        }

        std::vector<std::shared_ptr<ManagedProcess>> ordered = processes_;
        std::sort(ordered.begin(), ordered.end(),
                  [](const std::shared_ptr<ManagedProcess>& a, const std::shared_ptr<ManagedProcess>& b) {
                      return a->id < b->id;
                  });

        int newId = 1;
        for (const auto& proc : ordered) {
            proc->id = newId++;
        }
        nextId_ = newId;
        count = static_cast<int>(ordered.size());
    }

    SaveState();
    return count;
}

struct FindWindowByPidData {
    DWORD targetPid;
    HWND foundHwnd;
};

static BOOL CALLBACK EnumWindowsByPidProc(HWND hwnd, LPARAM lParam) {
    auto* data = reinterpret_cast<FindWindowByPidData*>(lParam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == data->targetPid && IsWindowVisible(hwnd)) {
        data->foundHwnd = hwnd;
        return FALSE;
    }
    return TRUE;
}

bool ProcessManager::FocusProcessWindow(int id) {
    std::shared_ptr<ManagedProcess> proc;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& p : processes_) {
            if (p->id == id) {
                proc = p;
                break;
            }
        }
    }
    if (!proc) return false;
    if (proc->terminalView && proc->terminalView->Hwnd()) {
        SetFocus(proc->terminalView->Hwnd());
        return true;
    }
    if (proc->state != ProcState::Running) return false;

    FindWindowByPidData data{proc->rootPid, nullptr};
    EnumWindows(EnumWindowsByPidProc, reinterpret_cast<LPARAM>(&data));
    if (data.foundHwnd) {
        if (IsIconic(data.foundHwnd)) ShowWindow(data.foundHwnd, SW_RESTORE);
        SetForegroundWindow(data.foundHwnd);
        return true;
    }
    return false;
}

TerminalView* ProcessManager::GetTerminalView(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& p : processes_) {
        if (p->id == id) return p->terminalView;
    }
    return nullptr;
}

ProcessSnapshot ProcessManager::BuildSnapshot(ProcessManager::ManagedProcess& p, uint64_t now) {
    ProcessSnapshot snap;
    snap.id = p.id;
    snap.name = p.name;
    snap.commandLine = p.commandLine;
    snap.workingDir = p.workingDir;
    snap.mode = p.mode;
    snap.state = p.state.load();
    snap.rootPid = p.rootPid;
    snap.exitCode = p.exitCode.load();
    snap.startTimeStr = p.startTimeStr;
    snap.endTimeStr = p.endTimeStr;
    snap.startTickMs = p.startTickMs;
    snap.endTickMs = p.endTickMs.load();
    snap.lastOutputTickMs = p.lastOutputTickMs.load();
    snap.lastCpuActivityTickMs = p.lastCpuActivityTickMs.load();
    snap.cpuTimeMs = p.cpuTime100ns.load() / 10000;
    if (snap.state == ProcState::Running) {
        snap.elapsedMs = (p.startTickMs > 0 && now >= p.startTickMs) ? (now - p.startTickMs) : p.restoredElapsedMs;
        uint64_t lastActivity = (std::max)(snap.lastOutputTickMs, snap.lastCpuActivityTickMs);
        snap.idleMs = (lastActivity > 0 && now >= lastActivity) ? (now - lastActivity) : 0;
    } else {
        if (p.startTickMs > 0 && snap.endTickMs >= p.startTickMs) {
            snap.elapsedMs = snap.endTickMs - p.startTickMs;
        } else {
            snap.elapsedMs = p.restoredElapsedMs;
        }
        snap.idleMs = 0;
    }
    snap.timeoutSec = p.timeoutSec;
    snap.idleTimeoutSec = p.idleTimeoutSec;
    snap.treeProcessCount = p.treeProcessCount.load();
    snap.treeMemoryBytes = p.treeMemoryBytes.load();
    snap.terminalHwnd = p.terminalView ? p.terminalView->Hwnd() : nullptr;

    {
        std::lock_guard<std::mutex> outLock(p.outputMutex);
        snap.totalBytesRead = p.totalBytesRead;
        snap.outputVersion = p.outputVersion;
        snap.lastLinePreview = p.lastLinePreview;
        snap.logFilePath = p.logFilePath;
        snap.doneFilePath = p.doneFilePath;
    }

    if (p.mode == ExecMode::ConPTY && p.terminalView) {
        std::string termLast = WideToUtf8(p.terminalView->ExtractLastNonEmptyLine());
        if (!termLast.empty()) {
            if (termLast.size() > 120) termLast = termLast.substr(0, 117) + "...";
            snap.lastLinePreview = termLast;
        }
    }
    return snap;
}

bool ProcessManager::GetSnapshot(int id, ProcessSnapshot& outSnap) {
    std::shared_ptr<ManagedProcess> proc;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& p : processes_) {
            if (p->id == id) {
                proc = p;
                break;
            }
        }
    }
    if (!proc) return false;
    outSnap = BuildSnapshot(*proc, GetNowTickMs());
    return true;
}

std::vector<ProcessSnapshot> ProcessManager::GetAllSnapshots() {
    std::vector<std::shared_ptr<ManagedProcess>> copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        copy = processes_;
    }
    uint64_t now = GetNowTickMs();
    std::vector<ProcessSnapshot> result;
    result.reserve(copy.size());
    for (auto& p : copy) {
        result.push_back(BuildSnapshot(*p, now));
    }
    return result;
}

bool ProcessManager::GetOutput(int id, bool stripAnsi, int tailLines, std::string& outText, uint64_t* outVersion) {
    std::shared_ptr<ManagedProcess> proc;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& p : processes_) {
            if (p->id == id) {
                proc = p;
                break;
            }
        }
    }
    if (!proc) return false;

    {
        std::lock_guard<std::mutex> outLock(proc->outputMutex);
        if (outVersion) *outVersion = proc->outputVersion;
        if (!stripAnsi) {
            outText = (tailLines > 0) ? GetTailLines(proc->rawOutput, tailLines) : proc->rawOutput;
            return true;
        }
    }

    if (proc->mode == ExecMode::ConPTY && proc->terminalView) {
        std::wstring screenWide = proc->terminalView->ExtractScreenText(tailLines);
        outText = WideToUtf8(screenWide);
        if (outText.empty()) {
            std::lock_guard<std::mutex> outLock(proc->outputMutex);
            std::string cleanFallback = StripAnsiCodes(proc->rawOutput);
            outText = (tailLines > 0) ? GetTailLines(cleanFallback, tailLines) : cleanFallback;
        }
        return true;
    }

    {
        std::lock_guard<std::mutex> outLock(proc->outputMutex);
        outText = (tailLines > 0) ? GetTailLines(proc->cleanOutput, tailLines) : proc->cleanOutput;
    }
    if (outText.empty() && !proc->logFilePathWide.empty()) {
        std::string diskLog = ReadFileUtf8Wide(proc->logFilePathWide);
        if (!diskLog.empty()) {
            outText = (tailLines > 0) ? GetTailLines(diskLog, tailLines) : diskLog;
        }
    }
    return true;
}

bool ProcessManager::WaitForExit(int id, uint32_t maxWaitMs) {
    uint64_t start = GetNowTickMs();
    while (true) {
        ProcessSnapshot snap;
        if (!GetSnapshot(id, snap)) return true;
        if (snap.state != ProcState::Running) return true;
        if (maxWaitMs > 0 && (GetNowTickMs() - start) >= maxWaitMs) {
            return false;
        }
        Sleep(100);
    }
}

void ProcessManager::SaveState() {
    std::vector<ProcessSnapshot> snaps = GetAllSnapshots();
    int curNextId = 1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        curNextId = nextId_;
    }

    std::ostringstream oss;
    oss << "{\r\n"
        << "  \"next_id\": " << curNextId << ",\r\n"
        << "  \"processes\": [\r\n";

    for (size_t i = 0; i < snaps.size(); ++i) {
        const auto& s = snaps[i];
        oss << "    {"
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
            << "\"elapsed_ms\":" << s.elapsedMs << ","
            << "\"cpu_time_ms\":" << s.cpuTimeMs << ","
            << "\"timeout_sec\":" << s.timeoutSec << ","
            << "\"idle_timeout_sec\":" << s.idleTimeoutSec << ","
            << "\"total_bytes_read\":" << s.totalBytesRead << ","
            << "\"log_file\":\"" << JsonEscape(s.logFilePath) << "\","
            << "\"done_file\":\"" << JsonEscape(s.doneFilePath) << "\","
            << "\"last_line\":\"" << JsonEscape(s.lastLinePreview) << "\""
            << "}" << (i + 1 < snaps.size() ? "," : "") << "\r\n";
    }
    oss << "  ]\r\n}\r\n";

    std::wstring statePath = GetLogsDirectoryWide() + L"\\processes_state.json";
    WriteFileAtomicWide(statePath, oss.str());
}

void ProcessManager::LoadState() {
    std::wstring statePath = GetLogsDirectoryWide() + L"\\processes_state.json";
    std::string content = ReadFileUtf8Wide(statePath);
    if (content.empty()) return;

    std::lock_guard<std::mutex> lock(mutex_);
    int64_t savedNextId = JsonGetInt(content, "next_id", 1);
    if (savedNextId > nextId_) {
        nextId_ = static_cast<int>(savedNextId);
    }

    size_t arrPos = content.find("\"processes\"");
    if (arrPos == std::string::npos) return;
    arrPos = content.find('[', arrPos);
    if (arrPos == std::string::npos) return;

    size_t pos = arrPos + 1;
    while (pos < content.size()) {
        size_t objStart = content.find('{', pos);
        if (objStart == std::string::npos) break;
        int depth = 0;
        bool inStr = false;
        size_t objEnd = std::string::npos;
        for (size_t i = objStart; i < content.size(); ++i) {
            char c = content[i];
            if (c == '"' && (i == 0 || content[i - 1] != '\\')) {
                inStr = !inStr;
            } else if (!inStr) {
                if (c == '{') depth++;
                else if (c == '}') {
                    depth--;
                    if (depth == 0) {
                        objEnd = i;
                        break;
                    }
                }
            }
        }
        if (objEnd == std::string::npos) break;

        std::string itemJson = content.substr(objStart, objEnd - objStart + 1);
        pos = objEnd + 1;

        int id = static_cast<int>(JsonGetInt(itemJson, "id", 0));
        if (id <= 0) continue;
        if (id >= nextId_) nextId_ = id + 1;

        auto proc = std::make_shared<ManagedProcess>();
        proc->id = id;
        proc->name = JsonGetString(itemJson, "name", "proc-" + std::to_string(id));
        proc->commandLine = JsonGetString(itemJson, "command", "");
        proc->workingDir = JsonGetString(itemJson, "cwd", "");
        proc->mode = StringToExecMode(JsonGetString(itemJson, "mode", "pty"));
        ProcState st = StringToProcState(JsonGetString(itemJson, "state", "completed"));
        if (st == ProcState::Running) {
            // Previous ProcessCommander instance exited; Job Object closed and terminated child tree
            st = ProcState::Killed;
        }
        proc->state = st;
        proc->rootPid = static_cast<DWORD>(JsonGetInt(itemJson, "pid", 0));
        DWORD ec = static_cast<DWORD>(JsonGetInt(itemJson, "exit_code", 0));
        if (ec == STILL_ACTIVE) ec = 1;
        proc->exitCode = ec;
        proc->startTimeStr = JsonGetString(itemJson, "start_time", "");
        proc->endTimeStr = JsonGetString(itemJson, "end_time", "");
        proc->restoredElapsedMs = static_cast<uint64_t>(JsonGetInt(itemJson, "elapsed_ms", 0));
        proc->cpuTime100ns = static_cast<uint64_t>(JsonGetInt(itemJson, "cpu_time_ms", 0)) * 10000;
        proc->timeoutSec = static_cast<int>(JsonGetInt(itemJson, "timeout_sec", 0));
        proc->idleTimeoutSec = static_cast<int>(JsonGetInt(itemJson, "idle_timeout_sec", 0));
        proc->totalBytesRead = static_cast<uint64_t>(JsonGetInt(itemJson, "total_bytes_read", 0));
        proc->logFilePath = JsonGetString(itemJson, "log_file", "");
        proc->logFilePathWide = Utf8ToWide(proc->logFilePath);
        proc->doneFilePath = JsonGetString(itemJson, "done_file", "");
        proc->doneFilePathWide = Utf8ToWide(proc->doneFilePath);
        proc->lastLinePreview = JsonGetString(itemJson, "last_line", "");

        processes_.push_back(proc);
    }
}

} // namespace pc
