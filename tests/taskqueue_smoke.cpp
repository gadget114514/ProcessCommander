#include "../src/taskqueue.h"
#include "../src/chain_guard.h"
#include "../src/common.h"
#include <windows.h>
#include <cstdio>
#include <string>

using namespace pc;

static int g_failures = 0;

static void Check(bool cond, const std::string& label) {
    std::printf("%s %s\n", cond ? "[PASS]" : "[FAIL]", label.c_str());
    if (!cond) g_failures++;
}

static std::string GetState(TaskQueueManager& tqm, const std::string& id) {
    std::string err;
    std::string resp = tqm.TaskQueueGet(id, false, 0, err);
    return JsonGetString(resp, "state", "");
}

static bool WaitForState(TaskQueueManager& tqm, const std::string& id, const std::string& want, int timeoutMs) {
    uint64_t start = GetNowTickMs();
    while (static_cast<int>(GetNowTickMs() - start) < timeoutMs) {
        if (GetState(tqm, id) == want) return true;
        Sleep(100);
    }
    return GetState(tqm, id) == want;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"pc_tq_smoke_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring env = L"PROCESS_COMMANDER_LOG_DIR=" + dir;
    _wputenv(env.c_str());

    ProcessManager pm;
    TaskQueueManager tqm;
    tqm.SetProcessManager(&pm);
    pm.SetProcessFinishedCallback([&tqm](int pid, int ec, const std::string& kind) {
        tqm.OnProcessFinished(pid, ec, kind);
    });

    std::string guardMsg;
    Check(!CheckChainGuard("A && B", guardMsg), "chain guard rejects &&");
    Check(!CheckChainGuard("cmd /c \"A && B\"", guardMsg), "chain guard rejects cmd /c chain");
    Check(CheckChainGuard("A | B", guardMsg), "chain guard allows single pipe");
    Check(CheckChainGuard("run.exe \"x && y\"", guardMsg), "chain guard allows quoted operator");
    Check(!CheckChainGuard("while(-not (Test-Path X)){Start-Sleep 20}", guardMsg), "chain guard rejects wait loop");
    Check(!CheckChainGuard("powershell -Command \"a; b\"", guardMsg), "chain guard rejects powershell semicolon");

    std::string err;
    std::string jobs = R"([
      {"id":"A","command":"cmd /c exit 0","summary":"A ok","timeout_sec":30},
      {"id":"B","command":"cmd /c exit 0","summary":"B after A","timeout_sec":30,"depends_on":["A"]},
      {"id":"C","command":"cmd /c exit 1","summary":"C fails","timeout_sec":30,"max_attempts":1}
    ])";
    std::string addResp = tqm.TaskQueueAdd("main", jobs, 0, "", err);
    Check(JsonGetBool(addResp, "ok", false), "taskqueue_add ok: " + err);

    Check(WaitForState(tqm, "A", "succeeded", 20000), "A succeeded");
    Check(WaitForState(tqm, "B", "succeeded", 20000), "B succeeded after A");
    Check(WaitForState(tqm, "C", "failed", 20000), "C failed");

    std::string badJobs = R"([{"id":"X","command":"cmd /c exit 0 && cmd /c exit 0","summary":"chain","timeout_sec":10}])";
    std::string badResp = tqm.TaskQueueAdd("main", badJobs, 0, "", err);
    Check(!JsonGetBool(badResp, "ok", false), "taskqueue_add rejects chain");

    std::string noTimeout = R"([{"id":"Y","command":"cmd /c exit 0","summary":"no timeout"}])";
    std::string ntResp = tqm.TaskQueueAdd("main", noTimeout, 0, "", err);
    Check(!JsonGetBool(ntResp, "ok", false), "taskqueue_add rejects missing timeout");

    std::wstring workDir = dir + L"\\work";
    CreateDirectoryW(workDir.c_str(), nullptr);
    std::string workUtf8 = WideToUtf8(workDir);
    std::string envJobs = "[{\"id\":\"E\",\"command\":\"cmd /c echo %PC_JOB_ID%,%PC_ATTEMPT%,%PC_JOB_SUMMARY%>args.txt\","
                          "\"summary\":\"Env Check\",\"timeoutSec\":30,\"cwd\":\"" + JsonEscape(workUtf8) + "\"}]";
    std::string envResp = tqm.TaskQueueAdd("envq", envJobs, 0, "", err);
    Check(JsonGetBool(envResp, "ok", false), "env job added: " + err);
    Check(WaitForState(tqm, "E", "succeeded", 20000), "env job succeeded");
    std::string argsFile = ReadFileUtf8Wide(workDir + L"\\args.txt");
    Check(argsFile.find("E,1,Env Check") != std::string::npos,
          "PC_* env vars and summary passed: " + argsFile);

    // Exit 0 is not success: D must write reportD.txt, it does not -> checkFailed.
    // B2 depends on D -> blocked once D finally fails.
    std::string jobs2 = "["
        "{\"id\":\"D\",\"command\":\"cmd /c exit 0\",\"summary\":\"D no file\",\"timeoutSec\":30,"
        "\"maxAttempts\":1,\"success\":{\"exitCode\":0,\"filesNewer\":[\"reportD.txt\"]},"
        "\"cwd\":\"" + JsonEscape(workUtf8) + "\"},"
        "{\"id\":\"B2\",\"command\":\"cmd /c exit 0\",\"summary\":\"blocked after D\",\"timeoutSec\":30,"
        "\"dependsOn\":[\"D\"],\"cwd\":\"" + JsonEscape(workUtf8) + "\"}"
        "]";
    std::string add2 = tqm.TaskQueueAdd("checkq", jobs2, 0, "", err);
    Check(JsonGetBool(add2, "ok", false), "checkq added: " + err);
    Check(WaitForState(tqm, "D", "failed", 20000), "D failed because the file was not created");
    Check(WaitForState(tqm, "B2", "blocked", 20000), "B2 blocked after D failed");

    std::string jobs3 = "[{\"id\":\"F\",\"command\":\"cmd /c echo ok>reportF.txt\",\"summary\":\"F writes file\","
                        "\"timeoutSec\":30,\"success\":{\"exitCode\":0,\"filesNewer\":[\"reportF.txt\"]},"
                        "\"cwd\":\"" + JsonEscape(workUtf8) + "\"}]";
    std::string add3 = tqm.TaskQueueAdd("checkq", jobs3, 0, "", err);
    Check(JsonGetBool(add3, "ok", false), "F added: " + err);
    Check(WaitForState(tqm, "F", "succeeded", 20000), "F succeeded because it wrote the file");

    // Instructions versioning: H fails, source changed, retry refreshes to rev 2.
    std::wstring instrPath = workDir + L"\\instr.md";
    WriteFileAtomicWide(instrPath, "# Task H\nDo the thing\n");
    std::string instrUtf8 = WideToUtf8(instrPath);
    std::string jobs4 = "[{\"id\":\"H\",\"command\":\"cmd /c exit 1\",\"summary\":\"H with instructions\","
                        "\"timeoutSec\":30,\"maxAttempts\":1,\"instructionsFile\":\"" + JsonEscape(instrUtf8) + "\"}]";
    std::string add4 = tqm.TaskQueueAdd("instrq", jobs4, 0, "", err);
    Check(JsonGetBool(add4, "ok", false), "H added: " + err);
    Check(WaitForState(tqm, "H", "failed", 20000), "H failed");
    {
        std::string r = tqm.TaskQueueGet("H", true, 0, err);
        Check(JsonGetString(r, "text", "").find("Do the thing") != std::string::npos,
              "instructions text captured");
        Check(!JsonGetBool(r, "source_changed", false), "source not changed yet");
        WriteFileAtomicWide(instrPath, "# Task H\nDo the thing (revised)\n");
        r = tqm.TaskQueueGet("H", true, 0, err);
        Check(JsonGetBool(r, "source_changed", false), "source_changed detected after edit");
    }
    {
        std::string r = tqm.TaskQueueRetry("H", true, true, err);
        Check(JsonGetBool(r, "ok", false), "retry H with refresh: " + err);
        std::string g = tqm.TaskQueueGet("H", true, 0, err);
        Check(JsonGetString(g, "rev", "0") == "2", "instructions revision bumped to 2");
    }

    Sleep(300);
    std::string retryResp = tqm.TaskQueueRetry("C", true, false, err);
    Check(JsonGetBool(retryResp, "ok", false), "retry C ok: " + err);
    Check(JsonGetString(retryResp, "state", "") == "pending", "retry C returns pending");
    Check(WaitForState(tqm, "C", "failed", 20000), "C runs and fails again after retry");

    std::wstring stateFile = TaskQueueManager::GetTaskQueueFileWide("main");
    Check(GetFileAttributesW(stateFile.c_str()) != INVALID_FILE_ATTRIBUTES, "taskqueue json persisted");

    {
        TaskQueueCounts cBefore = tqm.GetJobCounts();
        Check(cBefore.total > 0, "GetJobCounts returned non-empty total");
        Check(cBefore.succeeded >= 3, "GetJobCounts accurately counted succeeded jobs");

        std::string sleepJobs = "["
            "{\"id\":\"S1\",\"command\":\"ping -n 5 127.0.0.1\",\"summary\":\"S1 running\",\"timeoutSec\":30},"
            "{\"id\":\"S2\",\"command\":\"ping -n 5 127.0.0.1\",\"summary\":\"S2 waiting\",\"timeoutSec\":30},"
            "{\"id\":\"S3\",\"command\":\"ping -n 5 127.0.0.1\",\"summary\":\"S3 pending on S2\",\"timeoutSec\":30,\"dependsOn\":[\"S2\"]}"
            "]";
        std::string sResp = tqm.TaskQueueAdd("concurq", sleepJobs, 1, "", err);
        Check(JsonGetBool(sResp, "ok", false), "concurq added");
        WaitForState(tqm, "S1", "running", 5000);

        TaskQueueCounts cAfter = tqm.GetJobCounts();
        Check(cAfter.running >= 1, "GetJobCounts accurately detected running job");
        Check(cAfter.waiting >= 1, "GetJobCounts accurately detected waiting ready job");
        Check(cAfter.pending >= 1, "GetJobCounts accurately detected pending dependent job");

        tqm.TaskQueueCancel("S1", err);
        tqm.TaskQueueCancel("S2", err);
        tqm.TaskQueueCancel("S3", err);
    }

    pm.SetProcessFinishedCallback(nullptr);
    tqm.ShutdownTaskQueueManager();

    std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
