#include "../src/taskqueue.h"
#include "../src/chain_guard.h"
#include "../src/common.h"
#include "../src/llm_stream.h"
#include <windows.h>
#include <ctime>
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

    {
        // Scheduler: W1/W2 write the same file -> serialized; R1/R2 only read it -> parallel.
        std::wstring resDir = dir + L"\\res";
        CreateDirectoryW(resDir.c_str(), nullptr);
        std::string resCwd = JsonEscape(WideToUtf8(resDir));
        std::string jobsW = "["
            "{\"id\":\"W1\",\"command\":\"ping -n 4 127.0.0.1\",\"summary\":\"W1\",\"timeoutSec\":30,\"cwd\":\"" + resCwd + "\","
            "\"resources\":{\"write\":[\"shared.txt\"]}},"
            "{\"id\":\"W2\",\"command\":\"ping -n 4 127.0.0.1\",\"summary\":\"W2\",\"timeoutSec\":30,\"cwd\":\"" + resCwd + "\","
            "\"resources\":{\"write\":[\"shared.txt\"]}}"
            "]";
        Check(JsonGetBool(tqm.TaskQueueAdd("resw", jobsW, 4, "", err), "ok", false), "resource writers added: " + err);
        Check(WaitForState(tqm, "W1", "running", 5000), "W1 starts");
        Sleep(800);
        Check(GetState(tqm, "W2") == "pending", "W2 waits while W1 holds shared.txt");
        std::string g = tqm.TaskQueueGraph("resw", "", err);
        Check(g.find("\"resource_wait\":\"W1: shared.txt\"") != std::string::npos, "graph shows resource_wait for W2");
        Check(WaitForState(tqm, "W1", "succeeded", 20000), "W1 succeeded");
        Check(WaitForState(tqm, "W2", "succeeded", 20000), "W2 runs after W1 releases shared.txt");

        std::string jobsR = "["
            "{\"id\":\"RD1\",\"command\":\"ping -n 4 127.0.0.1\",\"summary\":\"RD1\",\"timeoutSec\":30,\"cwd\":\"" + resCwd + "\","
            "\"resources\":{\"read\":[\"shared.txt\"]}},"
            "{\"id\":\"RD2\",\"command\":\"ping -n 4 127.0.0.1\",\"summary\":\"RD2\",\"timeoutSec\":30,\"cwd\":\"" + resCwd + "\","
            "\"resources\":{\"read\":[\"shared.txt\"]}}"
            "]";
        Check(JsonGetBool(tqm.TaskQueueAdd("resr", jobsR, 4, "", err), "ok", false), "resource readers added: " + err);
        Check(WaitForState(tqm, "RD1", "running", 5000) && WaitForState(tqm, "RD2", "running", 5000), "readers run in parallel");
        Check(WaitForState(tqm, "RD2", "succeeded", 20000), "reader RD2 succeeded");

        // Order across queues: a writer queued behind a running reader is not overtaken by a
        // reader registered later in another queue (no writer starvation).
        auto one = [&](const std::string& id, const std::string& mode) {
            return "[{\"id\":\"" + id + "\",\"command\":\"ping -n 4 127.0.0.1\",\"summary\":\"" + id +
                   "\",\"timeoutSec\":30,\"cwd\":\"" + resCwd + "\",\"resources\":{\"" + mode + "\":[\"order.txt\"]}}]";
        };
        Check(JsonGetBool(tqm.TaskQueueAdd("ordq1", one("OA", "read"), 2, "", err), "ok", false), "OA added: " + err);
        Check(WaitForState(tqm, "OA", "running", 5000), "reader OA starts");
        Check(JsonGetBool(tqm.TaskQueueAdd("ordq2", one("OW", "write"), 2, "", err), "ok", false), "OW added: " + err);
        Check(JsonGetBool(tqm.TaskQueueAdd("ordq3", one("OB", "read"), 2, "", err), "ok", false), "OB added: " + err);
        Sleep(800);
        Check(GetState(tqm, "OW") == "pending", "writer OW waits for reader OA");
        Check(GetState(tqm, "OB") == "pending", "later reader OB in another queue waits behind writer OW");
        std::string og = tqm.TaskQueueGraph("ordq3", "", err);
        Check(og.find("OW (queued ahead): order.txt") != std::string::npos, "graph shows OB queued behind OW");
        Check(WaitForState(tqm, "OW", "running", 20000), "OW starts after OA");
        Check(GetState(tqm, "OB") == "pending", "OB still waits while OW writes");
        Check(WaitForState(tqm, "OB", "succeeded", 30000), "OB runs after OW");

        // Locks off (settings checkbox): conflicting writers run together.
        tqm.SetResourceLocksEnabled(false);
        Check(JsonGetBool(tqm.TaskQueueAdd("resoff1", one("OFF1", "write"), 2, "", err), "ok", false), "OFF1 added: " + err);
        Check(JsonGetBool(tqm.TaskQueueAdd("resoff2", one("OFF2", "write"), 2, "", err), "ok", false), "OFF2 added: " + err);
        Check(WaitForState(tqm, "OFF1", "running", 5000) && WaitForState(tqm, "OFF2", "running", 5000),
              "with resource locks off, conflicting writers run in parallel");
        Check(tqm.TaskQueueGraph("resoff1", "", err).find("\"resource_locks\":false") != std::string::npos,
              "graph reports resource_locks off");
        tqm.SetResourceLocksEnabled(true);
        Check(WaitForState(tqm, "OFF2", "succeeded", 20000), "lock-off writer finished");

        std::string bad = "[{\"id\":\"RB\",\"command\":\"cmd /c exit 0\",\"summary\":\"bad\",\"timeoutSec\":10,\"resources\":[\"x\"]}]";
        Check(!JsonGetBool(tqm.TaskQueueAdd("resw", bad, 0, "", err), "ok", false), "resources must be an object");
        std::string got = tqm.TaskQueueGet("W1", false, 0, err);
        Check(got.find("\"resources\":{\"write\":[\"shared.txt\"],\"read\":[]}") != std::string::npos, "resources returned by get");
    }

    pm.SetProcessFinishedCallback(nullptr);
    tqm.ShutdownTaskQueueManager();

    // Restart while a max_attempts=1 job is running: the interrupted attempt must not
    // fail the job, and its dependent must stay pending instead of becoming blocked.
    {
        std::string restartState =
            "{\"name\":\"restartq\",\"concurrency\":1,\"paused\":false,\"version\":3,"
            "\"notify_file\":\"\",\"max_timeout_sec\":7200,\"default_max_attempts\":2,\"jobs\":["
            "{\"id\":\"R1\",\"taskqueue\":\"restartq\",\"command\":\"cmd /c exit 0\",\"timeout_sec\":30,"
            "\"max_attempts\":1,\"state\":\"running\",\"attempts\":[{\"n\":1,\"process_id\":999,"
            "\"start\":\"2026-01-01 00:00:00\",\"end\":\"\",\"reason\":\"\",\"exit_code\":-1}]},"
            "{\"id\":\"R2\",\"taskqueue\":\"restartq\",\"command\":\"cmd /c exit 0\",\"timeout_sec\":30,"
            "\"max_attempts\":1,\"state\":\"pending\",\"depends_on\":[{\"id\":\"R1\",\"on\":\"success\"}]}"
            "]}";
        WriteFileAtomicWide(TaskQueueManager::GetTaskQueueFileWide("restartq"), restartState);
        TaskQueueManager restarted;
        Check(GetState(restarted, "R1") == "pending", "interrupted running job returns to pending after restart");
        Check(GetState(restarted, "R2") == "pending", "dependent of interrupted job stays pending after restart");
        restarted.ShutdownTaskQueueManager();
    }

    {
        // agy reports usage per step; totals must be the sum, not the high-water mark.
        SetUsageHistoryPersistence(true);
        int64_t since = static_cast<int64_t>(time(nullptr)) - 5;
        LlmStreamTracker tr;
        tr.Init(9001, "agy", "");
        std::string lines =
            "{\"event\":\"init\",\"init\":{\"conversation_id\":\"smoke-no-transcript\"}}\n"
            "{\"event\":\"step_update\",\"step_update\":{\"conversation_id\":\"smoke-no-transcript\",\"step_index\":1,\"state\":\"DONE\",\"step_type\":\"agent_response\","
            "\"usage\":{\"input_tokens\":7422,\"output_tokens\":677,\"thinking_tokens\":249,\"cache_read_tokens\":0,\"total_tokens\":8099}}}\n"
            "{\"event\":\"step_update\",\"step_update\":{\"conversation_id\":\"smoke-no-transcript\",\"step_index\":2,\"state\":\"DONE\",\"step_type\":\"agent_response\","
            "\"usage\":{\"input_tokens\":2675,\"output_tokens\":122,\"thinking_tokens\":48,\"cache_read_tokens\":128496,\"total_tokens\":2797}}}\n"
            "{\"event\":\"result\",\"result\":{\"conversation_id\":\"smoke-no-transcript\",\"status\":\"SUCCESS\",\"response\":\"ok\","
            "\"usage\":{\"input_tokens\":10097,\"output_tokens\":799,\"thinking_tokens\":297,\"cache_read_tokens\":128496,\"total_tokens\":10896}}}\n";
        tr.IngestChunk(lines.data(), lines.size());
        Check(tr.GetState().totalTokens == 10896, "agy per-step usage sums into state total");
        uint64_t histTotal = 0;
        for (const auto& p : GetLlmUsageHistory(since)) {
            if (p.agentType == "agy") histTotal += p.totalTokens;
        }
        Check(histTotal == 10896, "agy per-step usage recorded once per step in usage history");
        std::string hist = ReadFileUtf8Wide(dir + L"\\usage_history.jsonl");
        size_t nl = 0;
        for (char c : hist) if (c == '\n') nl++;
        Check(nl == 2 && hist.find("\\n") == std::string::npos, "usage_history.jsonl written in logs dir, one line per point");
        SetUsageHistoryPersistence(false);
    }

    {
        // Resource overlap rules (doc/resource-locks.md).
        Check(NormalizeResource("src\\A.cpp", "C:\\Proj\\") == "c:/proj/src/a.cpp", "relative resource resolves against cwd");
        Check(NormalizeResource("./src/../doc/x.md", "c:/p") == "c:/p/doc/x.md", "resource . and .. are resolved");
        Check(NormalizeResource("@Build", "c:/p") == "@build", "named lock is not resolved against cwd");
        Check(NormalizeResource("Foo.cs", "c:/p") == "c:/p/**/foo.cs", "bare file name matches anywhere below cwd (as ww)");
        Check(NormalizeResource("*.md", "") == "**/*.md", "bare glob matches anywhere");
        Check(NormalizeResource("**", "c:/p") == "c:/p/**", "** stays the whole cwd");
        Check(ResourcesOverlap(NormalizeResource("Foo.cs", "c:/p"), NormalizeResource("Assets/x/Foo.cs", "c:/p")),
              "bare file name overlaps the same file in a subfolder");
        Check(!ResourcesOverlap(NormalizeResource("Foo.cs", "c:/p"), NormalizeResource("src/b.cpp", "c:/p")),
              "bare file name does not overlap another file");
        // Same cases as tools/ww/ww-run.selftest.ps1 (Test-PathPatternOverlap), so both agree.
        Check(!ResourcesOverlap("c:/p/src/a.cpp", "c:/p/src/b.cpp"), "different files do not overlap");
        Check(ResourcesOverlap("c:/p/**/foo.cs", "c:/p/src/x/foo.cs") && ResourcesOverlap("c:/p/**/foo.cs", "c:/p/foo.cs"),
              "bare file name overlaps itself at any depth");
        Check(!ResourcesOverlap("c:/p/**/foo.cs", "c:/p/**/bar.cs"), "two different bare file names do not overlap");
        Check(ResourcesOverlap("c:/p/**", "c:/p/x/y.cpp") && !ResourcesOverlap("c:/p/doc/**", "c:/p/src/a.cpp"),
              "** under a directory covers only that directory");
        Check(ResourcesOverlap("c:/p/src/**/*.cpp", "c:/p/src/x/a.cpp") && !ResourcesOverlap("c:/p/src/**/*.cpp", "c:/p/src/x/a.h"),
              "**/*.cpp matches only .cpp files");
        Check(ResourcesOverlap("c:/p/src", "c:/p/src/a.cpp"), "directory covers its files");
        Check(!ResourcesOverlap("c:/p/src", "c:/p/src2/a.cpp"), "sibling with common prefix does not overlap");
        Check(ResourcesOverlap("c:/p/src/**/*.cpp", "c:/p/src/x/y.cpp"), "** glob matches nested file");
        Check(!ResourcesOverlap("c:/p/src/*.cpp", "c:/p/src/a.h"), "* glob does not match other extension");
        Check(!ResourcesOverlap("c:/p/src/*.cpp", "c:/p/src/x/a.cpp"), "* glob does not cross /");
        Check(ResourcesOverlap("c:/p/**/foo.cs", "c:/p/src"), "glob that can match below a directory overlaps it");
        Check(ResourcesOverlap("c:/p/src/*", "c:/p/src/x/a.cpp"), "glob matching an ancestor directory overlaps");
        Check(ResourcesOverlap("c:/p/doc/**", "c:/p/doc/*.md"), "glob x glob with nested literal prefixes overlaps");
        Check(!ResourcesOverlap("c:/p/doc/**", "c:/p/src/*.md"), "glob x glob with disjoint prefixes does not overlap");
        Check(!ResourcesOverlap("@build", "c:/p/build"), "named lock never overlaps a path");

        TaskQueueResources w1, w2, r1, r2;
        w1.write = {"src/a.cpp"};
        w2.write = {"src/**"};
        r1.read = {"src/a.cpp"};
        r2.read = {"src/**"};
        std::string res;
        Check(FindResourceConflict(w1, "c:/p", w2, "c:/p", res), "write x write conflicts");
        Check(FindResourceConflict(w1, "c:/p", r2, "c:/p", res), "write x read conflicts");
        Check(FindResourceConflict(r1, "c:/p", w2, "c:/p", res), "read x write conflicts");
        Check(!FindResourceConflict(r1, "c:/p", r2, "c:/p", res), "read x read does not conflict");
        Check(!FindResourceConflict(w1, "c:/p", w2, "c:/p/.claude/worktrees/x", res), "same relative path in another cwd does not conflict");
    }

    std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
