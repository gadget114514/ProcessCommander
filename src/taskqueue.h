#pragma once

#include "process_manager.h"
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>

namespace pc {

// ---------------------------------------------------------------------------
// TaskQueue state machine
// ---------------------------------------------------------------------------

enum class TaskQueueState {
    Pending = 0,
    Running = 1,
    Succeeded = 2,
    Failed = 3,
    Blocked = 4,
    Cancelled = 5,
};

std::string TaskQueueStateToString(TaskQueueState state);
TaskQueueState TaskQueueStateFromString(const std::string& s);

struct TaskQueueCounts {
    int running = 0;
    int waiting = 0;   // Ready to run (state == "pending", dependencies satisfied)
    int pending = 0;   // Waiting for dependencies (state == "pending", dependencies not satisfied)
    int blocked = 0;
    int succeeded = 0;
    int failed = 0;
    int cancelled = 0;
    int total = 0;
    int64_t minRemainingSec = -1;
    int64_t sumRemainingSec = -1;
};

// ---------------------------------------------------------------------------
// TaskQueue data model
// ---------------------------------------------------------------------------

struct TaskQueueInstructions {
    std::string source;
    int rev = 1;
    std::string sha256;
    std::string text;
    std::string capturedAt;
};

struct TaskQueueIdle {
    int outputSec = 0;
    int cpuSec = 0;
};

struct TaskQueueSuccess {
    int exitCode = 0;
    std::vector<std::string> filesNewer;
    std::string outputRegex;
};

struct TaskQueueDependency {
    std::string id;
    std::string on;
};

struct TaskQueueAttempt {
    int n = 1;
    int processId = 0;
    std::string start;
    std::string end;
    std::string reason;
    int exitCode = -1;
    int instructionsRev = 1;
    std::string lastLinesFile;
    std::string checkDetail;
    // Transient (not persisted): launch wall clock used by filesNewer checks.
    uint64_t startFiletime = 0;
};

struct TaskQueueJob {
    std::string id;
    std::string taskqueue;
    int num = 0; // transient numeric id (process correlation)
    std::string name;
    std::string summary;
    TaskQueueInstructions instructions;
    std::string command;
    std::string cwd;
    std::map<std::string, std::string> env;
    std::vector<TaskQueueDependency> dependsOn;
    int timeoutSec = 0;
    TaskQueueIdle idle;
    int maxAttempts = 2;
    int retryDelaySec = 0;
    TaskQueueSuccess success;
    std::vector<std::string> killExclude;
    int priority = 0;
    std::string state;
    std::vector<TaskQueueAttempt> attempts;
    std::string blockedBy;
    std::string createdAt;
    std::string updatedAt;
    std::string notes;
    int64_t retryAtMs = 0;
};

struct TaskQueue {
    std::string name;
    int concurrency = 1;
    bool paused = false;
    int version = 0;
    std::string notifyFile;
    int maxTimeoutSec = 7200;
    int defaultMaxAttempts = 2;
    std::vector<TaskQueueJob> jobs;
};

// ---------------------------------------------------------------------------
// TaskQueueManager: task queue store + scheduler + notifier
//
// The TaskQueue layer never touches OS processes directly; it always goes
// through ProcessManager (which reports completion via the finished callback).
// ---------------------------------------------------------------------------

class TaskQueueManager {
public:
    TaskQueueManager();
    ~TaskQueueManager();

    using ProcessFinishedCallback = std::function<void(int processId, int exitCode, const std::string& endKind)>;
    void SetProcessFinishedCallback(ProcessFinishedCallback cb);
    void SetProcessManager(ProcessManager* pm);

    // Task queue operations. Each returns a complete JSON response string
    // ({"ok":true,...} or {"ok":false,"error":"..."}).
    std::string TaskQueueAdd(const std::string& taskqueue, const std::string& jobsJson, int64_t concurrency, const std::string& notifyFile, std::string& outError);
    std::string TaskQueueList(const std::string& taskqueue, const std::string& state, std::string& outError);
    std::string TaskQueueGet(const std::string& id, bool includeInstructions, int rev, std::string& outError);
    std::string TaskQueueRetry(const std::string& id, bool resetAttempts, bool refreshInstructions, std::string& outError);
    std::string TaskQueueUpdate(const std::string& id, const std::string& patchJson, std::string& outError);
    std::string TaskQueueCancel(const std::string& id, std::string& outError);
    std::string TaskQueuePause(const std::string& taskqueue, bool paused, std::string& outError);
    std::string TaskQueueWait(const std::string& taskqueue, int sinceVersion, int timeoutMs, std::string& outError);
    std::string TaskQueuePurge(const std::string& taskqueue, const std::string& statesJson, std::string& outError);
    std::string TaskQueueLink(const std::string& job, const std::string& dependsOn, const std::string& on, std::string& outError);
    std::string TaskQueueUnlink(const std::string& job, const std::string& dependsOn, std::string& outError);
    std::string TaskQueueGraph(const std::string& taskqueue, const std::string& root, std::string& outError);

    // Scheduler hooks
    void OnProcessFinished(int processId, int exitCode, const std::string& endKind);
    void RequestSchedulerTick();
    void ShutdownTaskQueueManager();

    int64_t GetTaskQueueVersion(const std::string& taskqueue) const;

    TaskQueueCounts GetJobCounts() const;

    // Returns true and fills outJobId when an unfinished job (pending/running/
    // blocked/failed) uses exactly the given command.
    bool FindUnfinishedJobByCommand(const std::string& command, std::string& outJobId) const;

    static std::wstring GetTaskQueuesDirWide();
    static std::wstring GetTaskQueueFileWide(const std::string& name);
    static std::wstring GetInstructionsDirWide(const std::string& name);
    static std::wstring GetInstructionsFileWide(const std::string& name, const std::string& jobId, int rev);
    static std::wstring GetTaskEventsFileWide(const std::string& name);

    // Event notification (events.jsonl + notifyFile). Public for the GUI layer.
    void EmitTaskQueueEvent(const std::string& taskqueue, const std::string& type, const std::string& jobId, int64_t attempt);

private:
    struct LaunchItem {
        std::string taskqueue;
        std::string jobId;
        int attemptN = 0;
        StartProcessRequest req;
    };

    void LoadQueues();
    void SaveQueue(const TaskQueue& tq);
    void SaveAllLocked();
    void SchedulerLoop();
    void ProcessSchedulerTick();

    TaskQueueJob* FindJob(const std::string& id, TaskQueue** outQueue);
    const TaskQueueJob* FindJobConst(const std::string& id) const;

    void ApplyAttemptOutcome(TaskQueue& tq, TaskQueueJob& job, bool success,
                             const std::string& reason, int exitCode, const std::string& checkDetail);
    void ReevaluateAll();
    bool ReevaluateJob(TaskQueue& tq, TaskQueueJob& job);
    void ReevaluateDependents(const std::string& jobId);
    void CheckQueueNotices(TaskQueue& tq);

    static bool IsDepSatisfied(const TaskQueueJob& dep, const std::string& on);
    bool HasDependencyCycle(const TaskQueue& tq, const std::string& jobId,
                            const std::vector<TaskQueueDependency>& deps,
                            const std::string& ignoreDependency);
    bool BuildLaunchRequest(TaskQueue& tq, TaskQueueJob& job, int attemptN, LaunchItem& out);
    bool EvaluateSuccess(const TaskQueueJob& job, TaskQueueAttempt& attempt,
                         const std::string& endKind, int exitCode,
                         const std::string& logFile, std::string& detail);

    void EmitEventLocked(TaskQueue& tq, const std::string& type, const std::string& jobId, int64_t attempt);
    std::string JobToJson(const TaskQueueJob& job, bool includeInstructions) const;
    static std::string BuildError(const std::string& msg);

    ProcessManager* pm_ = nullptr;
    ProcessFinishedCallback finishedCb_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::map<std::string, TaskQueue> queues_;
    std::map<std::string, std::string> lastNotice_; // taskqueue -> last emitted notice type
    int nextJobNum_ = 1;
    std::atomic<bool> schedulerRunning_{false};
    std::thread schedulerThread_;
};

} // namespace pc
