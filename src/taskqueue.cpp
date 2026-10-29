#include "taskqueue.h"
#include "common.h"
#include "chain_guard.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <chrono>
#include <set>
#include <regex>

namespace pc {

std::string TaskQueueStateToString(TaskQueueState state) {
    switch (state) {
        case TaskQueueState::Pending:   return "pending";
        case TaskQueueState::Running:   return "running";
        case TaskQueueState::Succeeded: return "succeeded";
        case TaskQueueState::Failed:    return "failed";
        case TaskQueueState::Blocked:   return "blocked";
        case TaskQueueState::Cancelled: return "cancelled";
    }
    return "pending";
}

TaskQueueState TaskQueueStateFromString(const std::string& s) {
    if (s == "running") return TaskQueueState::Running;
    if (s == "succeeded") return TaskQueueState::Succeeded;
    if (s == "failed") return TaskQueueState::Failed;
    if (s == "blocked") return TaskQueueState::Blocked;
    if (s == "cancelled") return TaskQueueState::Cancelled;
    return TaskQueueState::Pending;
}

// ---------------------------------------------------------------------------
// File system locations
// ---------------------------------------------------------------------------

std::wstring TaskQueueManager::GetTaskQueuesDirWide() {
    std::wstring dir = GetLogsDirectoryWide();
    dir += L"\\taskqueues";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring TaskQueueManager::GetTaskQueueFileWide(const std::string& name) {
    return GetTaskQueuesDirWide() + L"\\" + Utf8ToWide(SanitizeFileNameComponent(name)) + L".json";
}

std::wstring TaskQueueManager::GetInstructionsDirWide(const std::string& name) {
    std::wstring base = GetTaskQueuesDirWide() + L"\\" + Utf8ToWide(SanitizeFileNameComponent(name));
    CreateDirectoryW(base.c_str(), nullptr);
    std::wstring dir = base + L"\\instructions";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring TaskQueueManager::GetInstructionsFileWide(const std::string& name, const std::string& jobId, int rev) {
    std::wstring fileName = Utf8ToWide(SanitizeFileNameComponent(jobId)) + L".r" + std::to_wstring(rev) + L".md";
    return GetInstructionsDirWide(name) + L"\\" + fileName;
}

std::wstring TaskQueueManager::GetTaskEventsFileWide(const std::string& name) {
    return GetLogsDirectoryWide() + L"\\taskqueue-" + Utf8ToWide(SanitizeFileNameComponent(name)) + L".events.jsonl";
}

// ---------------------------------------------------------------------------
// Minimal JSON field accessors (snake_case keys used by the taskqueue layer)
// ---------------------------------------------------------------------------

static size_t FindValPos(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = 0;
    while (true) {
        size_t found = json.find(pattern, pos);
        if (found == std::string::npos) return std::string::npos;
        size_t afterKey = found + pattern.size();
        while (afterKey < json.size() && std::isspace(static_cast<unsigned char>(json[afterKey]))) afterKey++;
        if (afterKey < json.size() && json[afterKey] == ':') {
            afterKey++;
            while (afterKey < json.size() && std::isspace(static_cast<unsigned char>(json[afterKey]))) afterKey++;
            return afterKey;
        }
        pos = found + pattern.size();
    }
}

static bool HasJsonKey(const std::string& json, const std::string& key) {
    return FindValPos(json, key) != std::string::npos;
}

static std::string JsonFieldStr(const std::string& obj, const std::string& key, const std::string& defVal = "") {
    size_t p = FindValPos(obj, key);
    if (p == std::string::npos) return defVal;
    if (p < obj.size() && obj[p] == '"') {
        std::string out;
        size_t i = p + 1;
        while (i < obj.size()) {
            char c = obj[i++];
            if (c == '"') break;
            if (c == '\\' && i < obj.size()) {
                char esc = obj[i++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    default: out.push_back(esc); break;
                }
            } else {
                out.push_back(c);
            }
        }
        return out;
    }
    size_t end = p;
    while (end < obj.size() && obj[end] != ',' && obj[end] != '}' && obj[end] != ']' &&
           !std::isspace(static_cast<unsigned char>(obj[end]))) {
        end++;
    }
    std::string raw = obj.substr(p, end - p);
    if (raw == "null" || raw.empty()) return defVal;
    return raw;
}

static int64_t JsonFieldInt(const std::string& obj, const std::string& key, int64_t defVal = 0) {
    std::string s = JsonFieldStr(obj, key, "");
    if (s.empty()) return defVal;
    try {
        return std::stoll(s);
    } catch (...) {
        return defVal;
    }
}

static bool JsonFieldBool(const std::string& obj, const std::string& key, bool defVal = false) {
    std::string s = JsonFieldStr(obj, key, "");
    if (s == "true" || s == "1") return true;
    if (s == "false" || s == "0") return false;
    return defVal;
}

static std::string JsonFieldObj(const std::string& obj, const std::string& key, const std::string& defVal = "{}") {
    size_t p = FindValPos(obj, key);
    if (p == std::string::npos || p >= obj.size() || obj[p] != '{') return defVal;
    int depth = 0;
    bool inStr = false;
    bool escaped = false;
    for (size_t i = p; i < obj.size(); ++i) {
        char c = obj[i];
        if (inStr) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; continue; }
        if (c == '{') depth++;
        else if (c == '}') { depth--; if (depth == 0) return obj.substr(p, i - p + 1); }
    }
    return defVal;
}

static bool FindArrayBounds(const std::string& json, const std::string& key, size_t& outStart, size_t& outEnd) {
    size_t p = FindValPos(json, key);
    if (p == std::string::npos || p >= json.size() || json[p] != '[') return false;
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
        if (c == '[') depth++;
        else if (c == ']') { depth--; if (depth == 0) { outStart = p; outEnd = i; return true; } }
    }
    return false;
}

static std::string TrimJsonStr(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

static std::vector<std::string> SplitTopLevelArray(const std::string& json, size_t openPos, size_t closePos) {
    std::vector<std::string> items;
    std::string current;
    int depth = 0;
    bool inStr = false;
    bool escaped = false;
    for (size_t i = openPos + 1; i < closePos && i < json.size(); ++i) {
        char c = json[i];
        if (inStr) {
            current.push_back(c);
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; current.push_back(c); continue; }
        if (c == '[' || c == '{') depth++;
        if (c == ']' || c == '}') depth--;
        if (c == ',' && depth == 0) {
            std::string item = TrimJsonStr(current);
            if (!item.empty()) items.push_back(item);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    std::string tail = TrimJsonStr(current);
    if (!tail.empty()) items.push_back(tail);
    return items;
}

static std::string StripQuotes(const std::string& s) {
    std::string t = TrimJsonStr(s);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') return t.substr(1, t.size() - 2);
    return t;
}

static std::vector<std::string> JsonFieldStrArray(const std::string& obj, const std::string& key) {
    size_t start = 0, end = 0;
    if (!FindArrayBounds(obj, key, start, end)) return {};
    std::vector<std::string> out;
    for (const auto& it : SplitTopLevelArray(obj, start, end)) {
        std::string t = StripQuotes(it);
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

static std::vector<std::string> JsonFieldObjArray(const std::string& obj, const std::string& key) {
    size_t start = 0, end = 0;
    if (!FindArrayBounds(obj, key, start, end)) return {};
    std::vector<std::string> out;
    for (const auto& it : SplitTopLevelArray(obj, start, end)) {
        std::string t = StripQuotes(it);
        if (t.size() >= 2 && t.front() == '{' && t.back() == '}') out.push_back(t);
    }
    return out;
}

static std::string BuildErrorJson(const std::string& msg) {
    return "{\"ok\":false,\"error\":\"" + JsonEscape(msg) + "\"}";
}

// Accept both camelCase (design doc / MCP arguments) and snake_case keys.
static std::string NormalizeJobJsonKeys(const std::string& in) {
    static const char* kPairs[][2] = {
        {"timeoutSec", "timeout_sec"},
        {"maxAttempts", "max_attempts"},
        {"retryDelaySec", "retry_delay_sec"},
        {"dependsOn", "depends_on"},
        {"killExclude", "kill_exclude"},
        {"instructionsFile", "instructions_file"},
        {"instructionsText", "instructions_text"},
        {"outputSec", "output_sec"},
        {"cpuSec", "cpu_sec"},
        {"exitCode", "exit_code"},
        {"filesNewer", "files_newer"},
        {"outputRegex", "output_regex"}
    };
    std::string out = in;
    for (const auto& pair : kPairs) {
        std::string pattern = std::string("\"") + pair[0] + "\"";
        std::string repl = std::string("\"") + pair[1] + "\"";
        size_t pos = 0;
        while ((pos = out.find(pattern, pos)) != std::string::npos) {
            out.replace(pos, pattern.size(), repl);
            pos += repl.size();
        }
    }
    return out;
}

} // namespace pc

namespace pc {

// ---------------------------------------------------------------------------
// JSON serialization (taskqueue.json format, snake_case keys)
// ---------------------------------------------------------------------------

static std::string AttemptToJson(const TaskQueueAttempt& a) {
    std::ostringstream oss;
    oss << "{\"n\":" << a.n
        << ",\"process_id\":" << a.processId
        << ",\"start\":\"" << JsonEscape(a.start) << "\""
        << ",\"end\":\"" << JsonEscape(a.end) << "\""
        << ",\"reason\":\"" << JsonEscape(a.reason) << "\""
        << ",\"exit_code\":" << a.exitCode
        << ",\"instructions_rev\":" << a.instructionsRev
        << ",\"last_lines_file\":\"" << JsonEscape(a.lastLinesFile) << "\""
        << ",\"check_detail\":\"" << JsonEscape(a.checkDetail) << "\"}";
    return oss.str();
}

static std::string EnvToJson(const std::map<std::string, std::string>& env) {
    std::ostringstream oss;
    oss << "{";
    bool first = true;
    for (const auto& kv : env) {
        if (!first) oss << ",";
        first = false;
        oss << "\"" << JsonEscape(kv.first) << "\":\"" << JsonEscape(kv.second) << "\"";
    }
    oss << "}";
    return oss.str();
}

static std::string DepsToJson(const std::vector<TaskQueueDependency>& deps) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < deps.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "{\"id\":\"" << JsonEscape(deps[i].id) << "\",\"on\":\"" << JsonEscape(deps[i].on) << "\"}";
    }
    oss << "]";
    return oss.str();
}

static std::string StrArrayToJson(const std::vector<std::string>& v) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "\"" << JsonEscape(v[i]) << "\"";
    }
    oss << "]";
    return oss.str();
}

static std::string AttemptsToJson(const std::vector<TaskQueueAttempt>& attempts) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < attempts.size(); ++i) {
        if (i > 0) oss << ",";
        oss << AttemptToJson(attempts[i]);
    }
    oss << "]";
    return oss.str();
}

static std::string InstructionsToJson(const TaskQueueInstructions& in) {
    std::ostringstream oss;
    oss << "{\"source\":\"" << JsonEscape(in.source) << "\""
        << ",\"rev\":" << in.rev
        << ",\"sha256\":\"" << JsonEscape(in.sha256) << "\""
        << ",\"text\":\"" << JsonEscape(in.text) << "\""
        << ",\"captured_at\":\"" << JsonEscape(in.capturedAt) << "\"}";
    return oss.str();
}

static std::string SuccessToJson(const TaskQueueSuccess& s) {
    std::ostringstream oss;
    oss << "{\"exit_code\":" << s.exitCode
        << ",\"files_newer\":" << StrArrayToJson(s.filesNewer)
        << ",\"output_regex\":";
    if (s.outputRegex.empty()) oss << "null";
    else oss << "\"" << JsonEscape(s.outputRegex) << "\"";
    oss << "}";
    return oss.str();
}

static TaskQueueAttempt ParseAttempt(const std::string& j) {
    TaskQueueAttempt a;
    a.n = static_cast<int>(JsonFieldInt(j, "n", 1));
    a.processId = static_cast<int>(JsonFieldInt(j, "process_id", 0));
    a.start = JsonFieldStr(j, "start", "");
    a.end = JsonFieldStr(j, "end", "");
    a.reason = JsonFieldStr(j, "reason", "");
    a.exitCode = static_cast<int>(JsonFieldInt(j, "exit_code", -1));
    a.instructionsRev = static_cast<int>(JsonFieldInt(j, "instructions_rev", 1));
    a.lastLinesFile = JsonFieldStr(j, "last_lines_file", "");
    a.checkDetail = JsonFieldStr(j, "check_detail", "");
    return a;
}

static TaskQueueInstructions ParseInstructions(const std::string& j) {
    TaskQueueInstructions in;
    in.source = JsonFieldStr(j, "source", "");
    in.rev = static_cast<int>(JsonFieldInt(j, "rev", 1));
    in.sha256 = JsonFieldStr(j, "sha256", "");
    in.text = JsonFieldStr(j, "text", "");
    in.capturedAt = JsonFieldStr(j, "captured_at", "");
    return in;
}

static std::map<std::string, std::string> ParseJsonStringMap(const std::string& objJson) {
    std::map<std::string, std::string> out;
    size_t p = objJson.find('{');
    if (p == std::string::npos) return out;
    size_t i = p + 1;
    while (i < objJson.size()) {
        while (i < objJson.size() && (std::isspace(static_cast<unsigned char>(objJson[i])) || objJson[i] == ',')) i++;
        if (i >= objJson.size() || objJson[i] == '}') break;
        if (objJson[i] != '"') { i++; continue; }
        std::string key;
        i++;
        while (i < objJson.size() && objJson[i] != '"') {
            if (objJson[i] == '\\' && i + 1 < objJson.size()) { i++; }
            key.push_back(objJson[i]);
            i++;
        }
        i++;
        while (i < objJson.size() && objJson[i] != ':') i++;
        if (i < objJson.size()) i++;
        while (i < objJson.size() && std::isspace(static_cast<unsigned char>(objJson[i]))) i++;
        if (i >= objJson.size()) break;
        std::string val;
        if (objJson[i] == '"') {
            i++;
            while (i < objJson.size() && objJson[i] != '"') {
                if (objJson[i] == '\\' && i + 1 < objJson.size()) {
                    char e = objJson[i + 1];
                    switch (e) {
                        case 'n': val.push_back('\n'); break;
                        case 'r': val.push_back('\r'); break;
                        case 't': val.push_back('\t'); break;
                        case '"': val.push_back('"'); break;
                        case '\\': val.push_back('\\'); break;
                        default: val.push_back(e); break;
                    }
                    i += 2;
                } else {
                    val.push_back(objJson[i]);
                    i++;
                }
            }
            i++;
        } else {
            size_t e = i;
            while (e < objJson.size() && objJson[e] != ',' && objJson[e] != '}') e++;
            val = TrimJsonStr(objJson.substr(i, e - i));
            i = e;
        }
        out[key] = val;
    }
    return out;
}

static TaskQueueJob ParseJob(const std::string& j) {
    TaskQueueJob job;
    job.id = JsonFieldStr(j, "id", "");
    job.taskqueue = JsonFieldStr(j, "taskqueue", "");
    job.name = JsonFieldStr(j, "name", "");
    job.summary = JsonFieldStr(j, "summary", "");
    job.instructions = ParseInstructions(JsonFieldObj(j, "instructions"));
    job.command = JsonFieldStr(j, "command", "");
    job.cwd = JsonFieldStr(j, "cwd", "");
    job.env = ParseJsonStringMap(JsonFieldObj(j, "env"));
    for (const auto& depJson : JsonFieldObjArray(j, "depends_on")) {
        TaskQueueDependency dep;
        dep.id = JsonFieldStr(depJson, "id", "");
        dep.on = JsonFieldStr(depJson, "on", "success");
        if (!dep.id.empty()) job.dependsOn.push_back(dep);
    }
    job.timeoutSec = static_cast<int>(JsonFieldInt(j, "timeout_sec", 0));
    std::string idleObj = JsonFieldObj(j, "idle");
    job.idle.outputSec = static_cast<int>(JsonFieldInt(idleObj, "output_sec", 0));
    job.idle.cpuSec = static_cast<int>(JsonFieldInt(idleObj, "cpu_sec", 0));
    job.maxAttempts = static_cast<int>(JsonFieldInt(j, "max_attempts", 2));
    job.retryDelaySec = static_cast<int>(JsonFieldInt(j, "retry_delay_sec", 0));
    std::string successObj = JsonFieldObj(j, "success");
    job.success.exitCode = static_cast<int>(JsonFieldInt(successObj, "exit_code", 0));
    job.success.filesNewer = JsonFieldStrArray(successObj, "files_newer");
    job.success.outputRegex = JsonFieldStr(successObj, "output_regex", "");
    job.killExclude = JsonFieldStrArray(j, "kill_exclude");
    job.priority = static_cast<int>(JsonFieldInt(j, "priority", 0));
    job.state = JsonFieldStr(j, "state", "pending");
    for (const auto& attJson : JsonFieldObjArray(j, "attempts")) {
        job.attempts.push_back(ParseAttempt(attJson));
    }
    job.blockedBy = JsonFieldStr(j, "blocked_by", "");
    job.createdAt = JsonFieldStr(j, "created_at", "");
    job.updatedAt = JsonFieldStr(j, "updated_at", "");
    job.notes = JsonFieldStr(j, "notes", "");
    job.retryAtMs = JsonFieldInt(j, "retry_at_ms", 0);
    return job;
}

static TaskQueue ParseQueue(const std::string& content) {
    TaskQueue tq;
    tq.name = JsonFieldStr(content, "name", "");
    tq.concurrency = static_cast<int>(JsonFieldInt(content, "concurrency", 1));
    tq.paused = JsonFieldBool(content, "paused", false);
    tq.version = static_cast<int>(JsonFieldInt(content, "version", 0));
    tq.notifyFile = JsonFieldStr(content, "notify_file", "");
    tq.maxTimeoutSec = static_cast<int>(JsonFieldInt(content, "max_timeout_sec", 7200));
    tq.defaultMaxAttempts = static_cast<int>(JsonFieldInt(content, "default_max_attempts", 2));
    for (const auto& jobJson : JsonFieldObjArray(content, "jobs")) {
        TaskQueueJob job = ParseJob(jobJson);
        if (!job.id.empty()) {
            if (job.taskqueue.empty()) job.taskqueue = tq.name;
            if (job.name.empty()) job.name = job.id;
            tq.jobs.push_back(job);
        }
    }
    return tq;
}

} // namespace pc

namespace pc {

// ---------------------------------------------------------------------------
// TaskQueueManager: construction / persistence / notifications
// ---------------------------------------------------------------------------

std::string TaskQueueManager::BuildError(const std::string& msg) {
    return BuildErrorJson(msg);
}

TaskQueueManager::TaskQueueManager() {
    LoadQueues();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [name, tq] : queues_) {
            bool changed = false;
            for (auto& job : tq.jobs) {
                if (job.state != "running") continue;
                if (!job.attempts.empty()) {
                    ApplyAttemptOutcome(tq, job, false, "interrupted", -1, "ProcessCommander restarted");
                } else {
                    job.state = "pending";
                    job.updatedAt = GetCurrentTimestampStr();
                    tq.version++;
                }
                changed = true;
            }
            if (changed) {
                ReevaluateAll();
                SaveQueue(tq);
            }
        }
    }
    schedulerRunning_.store(true);
    schedulerThread_ = std::thread(&TaskQueueManager::SchedulerLoop, this);
}

TaskQueueManager::~TaskQueueManager() {
    ShutdownTaskQueueManager();
}

void TaskQueueManager::ShutdownTaskQueueManager() {
    if (!schedulerRunning_.exchange(false)) return;
    cv_.notify_all();
    if (schedulerThread_.joinable()) {
        schedulerThread_.join();
    }
}

void TaskQueueManager::SetProcessManager(ProcessManager* pm) {
    std::lock_guard<std::mutex> lock(mutex_);
    pm_ = pm;
}

void TaskQueueManager::SetProcessFinishedCallback(ProcessFinishedCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    finishedCb_ = std::move(cb);
}

void TaskQueueManager::RequestSchedulerTick() {
    cv_.notify_all();
}

int64_t TaskQueueManager::GetTaskQueueVersion(const std::string& taskqueue) const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
    auto it = queues_.find(taskqueue);
    return (it == queues_.end()) ? -1 : it->second.version;
}

TaskQueueCounts TaskQueueManager::GetJobCounts() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
    TaskQueueCounts counts;
    counts.minRemainingSec = -1;
    counts.sumRemainingSec = 0;
    bool anyRemain = false;
    for (const auto& [name, tq] : queues_) {
        (void)name;
        for (const auto& job : tq.jobs) {
            counts.total++;
            if (job.state == "running") {
                counts.running++;
                if (job.timeoutSec > 0 && !job.attempts.empty()) {
                    const auto& lastAtt = job.attempts.back();
                    if (!lastAtt.start.empty()) {
                        int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
                        if (sscanf_s(lastAtt.start.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &sec) == 6) {
                            struct tm t = {};
                            t.tm_year = y - 1900;
                            t.tm_mon = mo - 1;
                            t.tm_mday = d;
                            t.tm_hour = h;
                            t.tm_min = mi;
                            t.tm_sec = sec;
                            t.tm_isdst = -1;
                            time_t startEpoch = mktime(&t);
                            if (startEpoch != static_cast<time_t>(-1)) {
                                time_t nowEpoch = time(nullptr);
                                int64_t elapsed = static_cast<int64_t>(nowEpoch - startEpoch);
                                int64_t rem = static_cast<int64_t>(job.timeoutSec) - elapsed;
                                if (rem < 0) rem = 0;
                                if (counts.minRemainingSec < 0 || rem < counts.minRemainingSec) {
                                    counts.minRemainingSec = rem;
                                }
                                counts.sumRemainingSec += rem;
                                anyRemain = true;
                            }
                        }
                    }
                } else if (job.timeoutSec > 0) {
                    int64_t rem = static_cast<int64_t>(job.timeoutSec);
                    if (counts.minRemainingSec < 0 || rem < counts.minRemainingSec) {
                        counts.minRemainingSec = rem;
                    }
                    counts.sumRemainingSec += rem;
                    anyRemain = true;
                }
            } else if (job.state == "pending") {
                bool allSat = true;
                for (const auto& d : job.dependsOn) {
                    const TaskQueueJob* dep = FindJobConst(d.id);
                    if (!dep) {
                        for (const auto& tj : tq.jobs) {
                            if (tj.id == d.id) { dep = &tj; break; }
                        }
                    }
                    if (!dep || !IsDepSatisfied(*dep, d.on)) {
                        allSat = false;
                        break;
                    }
                }
                if (allSat) {
                    counts.waiting++;
                } else {
                    counts.pending++;
                }
                if (job.timeoutSec > 0) {
                    counts.sumRemainingSec += static_cast<int64_t>(job.timeoutSec);
                    anyRemain = true;
                }
            } else if (job.state == "blocked") {
                counts.blocked++;
                if (job.timeoutSec > 0) {
                    counts.sumRemainingSec += static_cast<int64_t>(job.timeoutSec);
                    anyRemain = true;
                }
            } else if (job.state == "succeeded") {
                counts.succeeded++;
            } else if (job.state == "failed") {
                counts.failed++;
            } else if (job.state == "cancelled") {
                counts.cancelled++;
            }
        }
    }
    if (!anyRemain) counts.sumRemainingSec = -1;
    return counts;
}

bool TaskQueueManager::FindUnfinishedJobByCommand(const std::string& command, std::string& outJobId) const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
    for (const auto& [name, tq] : queues_) {
        (void)name;
        for (const auto& job : tq.jobs) {
            if (job.state == "succeeded" || job.state == "cancelled") continue;
            if (job.command == command) {
                outJobId = job.id;
                return true;
            }
        }
    }
    return false;
}

void TaskQueueManager::EmitTaskQueueEvent(const std::string& taskqueue, const std::string& type, const std::string& jobId, int64_t attempt) {
    std::lock_guard<std::mutex> lock(mutex_);
    TaskQueue& tq = queues_[taskqueue];
    EmitEventLocked(tq, type, jobId, attempt);
    SaveQueue(tq);
}

void TaskQueueManager::EmitEventLocked(TaskQueue& tq, const std::string& type, const std::string& jobId, int64_t attempt) {
    tq.version++;
    std::ostringstream oss;
    oss << "{\"version\":" << tq.version
        << ",\"type\":\"" << JsonEscape(type) << "\""
        << ",\"taskqueue\":\"" << JsonEscape(tq.name) << "\""
        << ",\"job_id\":\"" << JsonEscape(jobId) << "\""
        << ",\"attempt\":" << attempt
        << ",\"timestamp\":\"" << GetCurrentTimestampStr() << "\"}";
    std::string line = oss.str() + "\r\n";
    AppendToLogFileWide(GetTaskEventsFileWide(tq.name), line);
    if (!tq.notifyFile.empty() &&
        (type == "taskqueueDrained" || type == "taskqueueStalled" || type == "jobFailed")) {
        WriteFileAtomicWide(Utf8ToWide(tq.notifyFile), oss.str());
    }
}

void TaskQueueManager::LoadQueues() {
    std::wstring dir = GetTaskQueuesDirWide();
    WIN32_FIND_DATAW fd = {};
    HANDLE hFind = FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring path = dir + L"\\" + fd.cFileName;
        std::string content = ReadFileUtf8Wide(path);
        if (content.empty()) continue;
        TaskQueue tq = ParseQueue(content);
        if (tq.name.empty()) {
            std::wstring fn = fd.cFileName;
            if (fn.size() > 5) tq.name = WideToUtf8(fn.substr(0, fn.size() - 5));
        }
        if (tq.name.empty()) continue;
        for (auto& job : tq.jobs) {
            if (job.num <= 0) job.num = nextJobNum_++;
        }
        queues_[tq.name] = tq;
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
}

void TaskQueueManager::SaveQueue(const TaskQueue& tq) {
    if (tq.name.empty()) return;
    std::ostringstream oss;
    oss << "{\r\n"
        << "  \"name\": \"" << JsonEscape(tq.name) << "\",\r\n"
        << "  \"concurrency\": " << tq.concurrency << ",\r\n"
        << "  \"paused\": " << (tq.paused ? "true" : "false") << ",\r\n"
        << "  \"version\": " << tq.version << ",\r\n"
        << "  \"notify_file\": \"" << JsonEscape(tq.notifyFile) << "\",\r\n"
        << "  \"max_timeout_sec\": " << tq.maxTimeoutSec << ",\r\n"
        << "  \"default_max_attempts\": " << tq.defaultMaxAttempts << ",\r\n"
        << "  \"jobs\": [";
    for (size_t i = 0; i < tq.jobs.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "\r\n    " << JobToJson(tq.jobs[i], true);
    }
    oss << "\r\n  ]\r\n}\r\n";
    WriteFileAtomicWide(GetTaskQueueFileWide(tq.name), oss.str());
}

std::string TaskQueueManager::JobToJson(const TaskQueueJob& job, bool includeInstructions) const {
    std::string out;
    out.reserve(1024); // Optimize: Pre-allocate to avoid temporary heap allocations during append
    out += "{\"id\":\""; out += JsonEscape(job.id);
    out += "\",\"taskqueue\":\""; out += JsonEscape(job.taskqueue);
    out += "\",\"name\":\""; out += JsonEscape(job.name);
    out += "\",\"summary\":\""; out += JsonEscape(job.summary);
    out += "\"";
    if (includeInstructions) {
        out += ",\"instructions\":"; out += InstructionsToJson(job.instructions);
    } else {
        out += ",\"instructions\":{\"source\":\""; out += JsonEscape(job.instructions.source);
        out += "\",\"rev\":"; out += std::to_string(job.instructions.rev); out += "}";
    }
    out += ",\"command\":\""; out += JsonEscape(job.command);
    out += "\",\"cwd\":\""; out += JsonEscape(job.cwd);
    out += "\",\"env\":"; out += EnvToJson(job.env);
    out += ",\"depends_on\":"; out += DepsToJson(job.dependsOn);
    out += ",\"timeout_sec\":"; out += std::to_string(job.timeoutSec);
    out += ",\"idle\":{\"output_sec\":"; out += std::to_string(job.idle.outputSec);
    out += ",\"cpu_sec\":"; out += std::to_string(job.idle.cpuSec); out += "}";
    out += ",\"max_attempts\":"; out += std::to_string(job.maxAttempts);
    out += ",\"retry_delay_sec\":"; out += std::to_string(job.retryDelaySec);
    out += ",\"success\":"; out += SuccessToJson(job.success);
    out += ",\"kill_exclude\":"; out += StrArrayToJson(job.killExclude);
    out += ",\"priority\":"; out += std::to_string(job.priority);
    out += ",\"state\":\""; out += JsonEscape(job.state);
    out += "\",\"attempts\":"; out += AttemptsToJson(job.attempts);
    out += ",\"blocked_by\":";
    if (job.blockedBy.empty()) out += "null";
    else { out += "\""; out += JsonEscape(job.blockedBy); out += "\""; }
    out += ",\"created_at\":\""; out += JsonEscape(job.createdAt);
    out += "\",\"updated_at\":\""; out += JsonEscape(job.updatedAt);
    out += "\",\"notes\":\""; out += JsonEscape(job.notes);
    out += "\"}";
    return out;
}

TaskQueueJob* TaskQueueManager::FindJob(const std::string& id, TaskQueue** outQueue) {
    for (auto& [name, tq] : queues_) {
        (void)name;
        for (auto& job : tq.jobs) {
            if (job.id == id) {
                if (outQueue) *outQueue = &tq;
                return &job;
            }
        }
    }
    return nullptr;
}

const TaskQueueJob* TaskQueueManager::FindJobConst(const std::string& id) const {
    for (const auto& [name, tq] : queues_) {
        (void)name;
        for (const auto& job : tq.jobs) {
            if (job.id == id) return &job;
        }
    }
    return nullptr;
}


bool TaskQueueManager::IsDepSatisfied(const TaskQueueJob& dep, const std::string& on) {
    if (on == "done") {
        return dep.state == "succeeded" || dep.state == "failed" || dep.state == "cancelled";
    }
    return dep.state == "succeeded";
}

bool TaskQueueManager::HasDependencyCycle(const TaskQueue& tq, const std::string& jobId,
                                          const std::vector<TaskQueueDependency>& deps,
                                          const std::string& ignoreDependency) {
    std::vector<std::string> stack;
    std::vector<std::string> visited;
    for (const auto& d : deps) {
        if (d.id == ignoreDependency) continue;
        stack.push_back(d.id);
    }
    while (!stack.empty()) {
        std::string cur = stack.back();
        stack.pop_back();
        if (cur == jobId) return true;
        if (std::find(visited.begin(), visited.end(), cur) != visited.end()) continue;
        visited.push_back(cur);
        const TaskQueueJob* j = FindJobConst(cur);
        if (!j) {
            for (const auto& tj : tq.jobs) {
                if (tj.id == cur) { j = &tj; break; }
            }
        }
        if (j) {
            for (const auto& d : j->dependsOn) stack.push_back(d.id);
        }
    }
    return false;
}

} // namespace pc

namespace pc {

// ---------------------------------------------------------------------------
// Small self-contained SHA-256 (used for instructions versioning)
// ---------------------------------------------------------------------------

namespace {

struct Sha256Ctx {
    uint32_t h[8];
    uint64_t len = 0;
    uint8_t buf[64];
    size_t bufLen = 0;
};

static uint32_t Rotr32(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

static void Sha256Init(Sha256Ctx& c) {
    static const uint32_t kInit[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    for (int i = 0; i < 8; ++i) c.h[i] = kInit[i];
}

static void Sha256Block(Sha256Ctx& c, const uint8_t* p) {
    static const uint32_t k[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = Rotr32(w[i - 15], 7) ^ Rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = Rotr32(w[i - 2], 17) ^ Rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c.h[0], b = c.h[1], cc = c.h[2], d = c.h[3], e = c.h[4], f = c.h[5], g = c.h[6], h = c.h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t s1 = Rotr32(e, 6) ^ Rotr32(e, 11) ^ Rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + k[i] + w[i];
        uint32_t s0 = Rotr32(a, 2) ^ Rotr32(a, 13) ^ Rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c.h[0] += a; c.h[1] += b; c.h[2] += cc; c.h[3] += d;
    c.h[4] += e; c.h[5] += f; c.h[6] += g; c.h[7] += h;
}

static void Sha256Update(Sha256Ctx& c, const uint8_t* data, size_t len) {
    c.len += len;
    while (len > 0) {
        size_t take = 64 - c.bufLen;
        if (take > len) take = len;
        memcpy(c.buf + c.bufLen, data, take);
        c.bufLen += take;
        data += take;
        len -= take;
        if (c.bufLen == 64) {
            Sha256Block(c, c.buf);
            c.bufLen = 0;
        }
    }
}

static std::string Sha256Hex(const std::string& input) {
    Sha256Ctx c;
    Sha256Init(c);
    Sha256Update(c, reinterpret_cast<const uint8_t*>(input.data()), input.size());
    uint64_t bitLen = c.len * 8;
    uint8_t pad = 0x80;
    Sha256Update(c, &pad, 1);
    uint8_t zero = 0;
    while (c.bufLen != 56) Sha256Update(c, &zero, 1);
    uint8_t lenBuf[8];
    for (int i = 0; i < 8; ++i) lenBuf[i] = uint8_t((bitLen >> (56 - i * 8)) & 0xFF);
    Sha256Update(c, lenBuf, 8);
    char out[65];
    for (int i = 0; i < 8; ++i) snprintf(out + i * 8, 9, "%08x", c.h[i]);
    return std::string(out, 64);
}

static std::vector<std::string> ParseObjectArrayDirect(const std::string& arrJson) {
    size_t start = arrJson.find('[');
    if (start == std::string::npos) return {};
    int depth = 0;
    bool inStr = false;
    bool escaped = false;
    size_t end = std::string::npos;
    for (size_t i = start; i < arrJson.size(); ++i) {
        char ch = arrJson[i];
        if (inStr) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') inStr = false;
            continue;
        }
        if (ch == '"') { inStr = true; continue; }
        if (ch == '[') depth++;
        else if (ch == ']') { depth--; if (depth == 0) { end = i; break; } }
    }
    if (end == std::string::npos) return {};
    std::vector<std::string> out;
    for (const auto& it : SplitTopLevelArray(arrJson, start, end)) {
        std::string t = StripQuotes(it);
        if (t.size() >= 2 && t.front() == '{' && t.back() == '}') out.push_back(t);
    }
    return out;
}

} // namespace

} // namespace pc

namespace pc {

// ---------------------------------------------------------------------------
// Job definition parsing + validation
// ---------------------------------------------------------------------------

static bool ParseNewJob(const std::string& rawJson, const std::string& taskqueue,
                        int defaultMaxAttempts, int maxTimeoutSec,
                        TaskQueueJob& job, std::string& err) {
    const std::string j = NormalizeJobJsonKeys(rawJson);
    job = TaskQueueJob();
    job.id = JsonFieldStr(j, "id", "");
    if (job.id.empty()) {
        err = "job id is required";
        return false;
    }
    job.taskqueue = taskqueue;
    job.name = JsonFieldStr(j, "name", job.id);
    job.summary = JsonFieldStr(j, "summary", "");
    if (job.summary.empty()) {
        err = "job '" + job.id + "': summary is required";
        return false;
    }
    if (job.summary.find('\n') != std::string::npos || job.summary.find('\r') != std::string::npos) {
        err = "job '" + job.id + "': summary must be a single line";
        return false;
    }
    if (job.summary.size() > 200) {
        err = "job '" + job.id + "': summary must be 200 characters or less";
        return false;
    }
    job.command = JsonFieldStr(j, "command", "");
    if (job.command.empty()) {
        err = "job '" + job.id + "': command is required";
        return false;
    }
    std::string chainErr;
    if (!CheckChainGuard(job.command, chainErr)) {
        err = "job '" + job.id + "': " + chainErr;
        return false;
    }
    job.cwd = JsonFieldStr(j, "cwd", "");
    job.timeoutSec = static_cast<int>(JsonFieldInt(j, "timeout_sec", 0));
    if (job.timeoutSec <= 0) {
        err = "job '" + job.id + "': timeout_sec is required and must be greater than 0";
        return false;
    }
    if (maxTimeoutSec > 0 && job.timeoutSec > maxTimeoutSec) {
        err = "job '" + job.id + "': timeout_sec (" + std::to_string(job.timeoutSec) +
              ") exceeds max_timeout_sec (" + std::to_string(maxTimeoutSec) + ")";
        return false;
    }
    std::string idleObj = JsonFieldObj(j, "idle");
    job.idle.outputSec = static_cast<int>(JsonFieldInt(idleObj, "output_sec", 0));
    job.idle.cpuSec = static_cast<int>(JsonFieldInt(idleObj, "cpu_sec", 0));
    if (job.idle.outputSec < 0 || job.idle.cpuSec < 0) {
        err = "job '" + job.id + "': idle values must not be negative";
        return false;
    }
    job.maxAttempts = static_cast<int>(JsonFieldInt(j, "max_attempts", defaultMaxAttempts));
    if (job.maxAttempts < 1) job.maxAttempts = 1;
    job.retryDelaySec = static_cast<int>(JsonFieldInt(j, "retry_delay_sec", 0));
    if (job.retryDelaySec < 0) job.retryDelaySec = 0;
    std::string successObj = JsonFieldObj(j, "success");
    job.success.exitCode = static_cast<int>(JsonFieldInt(successObj, "exit_code", 0));
    job.success.filesNewer = JsonFieldStrArray(successObj, "files_newer");
    job.success.outputRegex = JsonFieldStr(successObj, "output_regex", "");
    job.killExclude = JsonFieldStrArray(j, "kill_exclude");
    job.priority = static_cast<int>(JsonFieldInt(j, "priority", 0));
    job.env = ParseJsonStringMap(JsonFieldObj(j, "env"));
    {
        size_t aStart = 0;
        size_t aEnd = 0;
        if (FindArrayBounds(j, "depends_on", aStart, aEnd)) {
            for (const auto& item : SplitTopLevelArray(j, aStart, aEnd)) {
                std::string t = TrimJsonStr(item);
                if (t.size() >= 2 && t.front() == '{') {
                    TaskQueueDependency dep;
                    dep.id = JsonFieldStr(t, "id", "");
                    dep.on = JsonFieldStr(t, "on", "success");
                    if (dep.on != "success" && dep.on != "done") {
                        err = "job '" + job.id + "': depends_on[].on must be 'success' or 'done'";
                        return false;
                    }
                    if (!dep.id.empty()) job.dependsOn.push_back(dep);
                } else {
                    std::string id = StripQuotes(t);
                    if (!id.empty()) {
                        TaskQueueDependency dep;
                        dep.id = id;
                        dep.on = "success";
                        job.dependsOn.push_back(dep);
                    }
                }
            }
        }
    }

    std::string instrFile = JsonFieldStr(j, "instructions_file", "");
    std::string instrText = JsonFieldStr(j, "instructions_text", "");
    if (!instrFile.empty()) {
        std::wstring pathW;
        bool absolute = instrFile.size() > 1 && (instrFile[1] == ':' || instrFile[0] == '\\' || instrFile[0] == '/');
        if (absolute) {
            pathW = ResolveAbsolutePathWide(Utf8ToWide(instrFile));
        } else {
            std::wstring base = job.cwd.empty() ? GetCurrentDirectoryWide() : ResolveAbsolutePathWide(Utf8ToWide(job.cwd));
            pathW = ResolveAbsolutePathWide(base + L"\\" + Utf8ToWide(instrFile));
        }
        if (GetFileAttributesW(pathW.c_str()) == INVALID_FILE_ATTRIBUTES) {
            err = "job '" + job.id + "': instructions_file not found: " + instrFile;
            return false;
        }
        std::string text = ReadFileUtf8Wide(pathW);
        if (text.size() > 1024 * 1024) {
            err = "job '" + job.id + "': instructions exceed the 1 MB limit";
            return false;
        }
        job.instructions.source = instrFile;
        job.instructions.text = text;
        job.instructions.sha256 = Sha256Hex(text);
    } else if (!instrText.empty()) {
        if (instrText.size() > 1024 * 1024) {
            err = "job '" + job.id + "': instructions exceed the 1 MB limit";
            return false;
        }
        job.instructions.text = instrText;
        job.instructions.sha256 = Sha256Hex(instrText);
    }
    job.instructions.rev = 1;
    job.instructions.capturedAt = GetCurrentTimestampStr();
    job.state = "pending";
    job.createdAt = GetCurrentTimestampStr();
    job.updatedAt = job.createdAt;
    return true;
}

} // namespace pc

namespace pc {

static std::wstring ResolveJobPathWide(const TaskQueueJob& job, const std::string& path) {
    bool absolute = path.size() > 1 && (path[1] == ':' || path[0] == '\\' || path[0] == '/');
    if (absolute) return ResolveAbsolutePathWide(Utf8ToWide(path));
    std::wstring base = job.cwd.empty() ? GetCurrentDirectoryWide() : ResolveAbsolutePathWide(Utf8ToWide(job.cwd));
    return ResolveAbsolutePathWide(base + L"\\" + Utf8ToWide(path));
}

std::string TaskQueueManager::TaskQueueAdd(const std::string& taskqueue, const std::string& jobsJson,
                                           int64_t concurrency, const std::string& notifyFile, std::string& outError) {
    outError.clear();
    if (taskqueue.empty()) {
        outError = "taskqueue is required";
        return BuildError(outError);
    }
    std::vector<std::string> jobObjs = ParseObjectArrayDirect(jobsJson);
    if (jobObjs.empty()) {
        outError = "jobs must be a non-empty array of job objects";
        return BuildError(outError);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    TaskQueue tq;
    auto existing = queues_.find(taskqueue);
    if (existing != queues_.end()) tq = existing->second;
    if (tq.name.empty()) tq.name = taskqueue;
    if (tq.concurrency <= 0) tq.concurrency = 1;
    if (tq.maxTimeoutSec <= 0) tq.maxTimeoutSec = 7200;
    if (tq.defaultMaxAttempts <= 0) tq.defaultMaxAttempts = 2;
    if (concurrency > 0) tq.concurrency = static_cast<int>(concurrency);
    if (!notifyFile.empty()) tq.notifyFile = notifyFile;

    std::vector<TaskQueueJob> newJobs;
    for (const auto& jo : jobObjs) {
        TaskQueueJob job;
        std::string err;
        if (!ParseNewJob(jo, taskqueue, tq.defaultMaxAttempts, tq.maxTimeoutSec, job, err)) {
            outError = err;
            return BuildError(outError);
        }
        if (FindJob(job.id, nullptr) != nullptr) {
            outError = "duplicate job id '" + job.id + "'";
            return BuildError(outError);
        }
        for (const auto& other : newJobs) {
            if (other.id == job.id) {
                outError = "duplicate job id '" + job.id + "'";
                return BuildError(outError);
            }
        }
        newJobs.push_back(job);
    }

    TaskQueue merged = tq;
    for (const auto& j : newJobs) merged.jobs.push_back(j);
    for (const auto& job : newJobs) {
        for (const auto& dep : job.dependsOn) {
            bool exists = false;
            for (const auto& mj : merged.jobs) {
                if (mj.id == dep.id) { exists = true; break; }
            }
            if (!exists) {
                outError = "job '" + job.id + "': unknown dependency '" + dep.id + "'";
                return BuildError(outError);
            }
        }
        if (HasDependencyCycle(merged, job.id, job.dependsOn, "")) {
            outError = "job '" + job.id + "': dependency cycle detected";
            return BuildError(outError);
        }
    }

    for (auto& j : newJobs) tq.jobs.push_back(j);
    for (auto& j : tq.jobs) {
        if (j.num <= 0) j.num = nextJobNum_++;
    }
    queues_[taskqueue] = tq;
    ReevaluateAll();
    SaveQueue(queues_[taskqueue]);
    cv_.notify_all();

    std::ostringstream oss;
    oss << "{\"ok\":true,\"taskqueue\":\"" << JsonEscape(taskqueue) << "\",\"added\":[";
    for (size_t i = 0; i < newJobs.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "\"" << JsonEscape(newJobs[i].id) << "\"";
    }
    oss << "],\"concurrency\":" << queues_[taskqueue].concurrency << "}";
    return oss.str();
}

std::string TaskQueueManager::TaskQueueList(const std::string& taskqueue, const std::string& state, std::string& outError) {
    outError.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    struct ListedJob { std::string queue; TaskQueueJob job; };
    std::vector<ListedJob> listed;
    for (const auto& [name, tq] : queues_) {
        (void)name;
        if (!taskqueue.empty() && name != taskqueue) continue;
        for (const auto& job : tq.jobs) {
            if (!state.empty() && job.state != state) continue;
            listed.push_back({name, job});
        }
    }
    std::stable_sort(listed.begin(), listed.end(), [](const ListedJob& a, const ListedJob& b) {
        if (a.job.createdAt != b.job.createdAt) return a.job.createdAt > b.job.createdAt;
        return false;
    });
    std::ostringstream oss;
    oss << "{\"ok\":true,\"jobs\":[";
    bool first = true;
    for (const auto& item : listed) {
        if (!first) oss << ",";
        first = false;
        oss << JobToJson(item.job, false);
    }
    oss << "]}";
    return oss.str();
}

} // namespace pc

namespace pc {

std::string TaskQueueManager::TaskQueueGet(const std::string& id, bool includeInstructions, int rev, std::string& outError) {
    outError.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    TaskQueueJob* found = FindJob(id, nullptr);
    if (!found) {
        outError = "job not found: " + id;
        return BuildError(outError);
    }
    TaskQueueJob job = *found;

    if (rev > 0 && rev != job.instructions.rev) {
        std::string text = ReadFileUtf8Wide(GetInstructionsFileWide(job.taskqueue, job.id, rev));
        if (!text.empty()) {
            job.instructions.text = text;
            job.instructions.rev = rev;
        }
    }

    bool sourceMissing = false;
    bool sourceChanged = false;
    if (!job.instructions.source.empty()) {
        std::wstring pathW = ResolveJobPathWide(job, job.instructions.source);
        if (GetFileAttributesW(pathW.c_str()) == INVALID_FILE_ATTRIBUTES) {
            sourceMissing = true;
        } else {
            std::string text = ReadFileUtf8Wide(pathW);
            sourceChanged = Sha256Hex(text) != job.instructions.sha256;
        }
    }

    std::ostringstream oss;
    oss << "{\"ok\":true,\"source_changed\":" << (sourceChanged ? "true" : "false")
        << ",\"source_missing\":" << (sourceMissing ? "true" : "false")
        << ",\"job\":" << JobToJson(job, includeInstructions) << "}";
    return oss.str();
}

std::string TaskQueueManager::TaskQueueRetry(const std::string& id, bool resetAttempts, bool refreshInstructions, std::string& outError) {
    outError.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    TaskQueue* tq = nullptr;
    TaskQueueJob* job = FindJob(id, &tq);
    if (!job) {
        outError = "job not found: " + id;
        return BuildError(outError);
    }
    if (job->state == "running") {
        outError = "job '" + id + "' is running; cancel it before retrying";
        return BuildError(outError);
    }
    if (job->state == "succeeded") {
        outError = "job '" + id + "' already succeeded";
        return BuildError(outError);
    }

    if (refreshInstructions && !job->instructions.source.empty()) {
        std::wstring pathW = ResolveJobPathWide(*job, job->instructions.source);
        if (GetFileAttributesW(pathW.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::string text = ReadFileUtf8Wide(pathW);
            std::string sha = Sha256Hex(text);
            if (sha != job->instructions.sha256) {
                job->instructions.text = text;
                job->instructions.sha256 = sha;
                job->instructions.rev++;
                job->instructions.capturedAt = GetCurrentTimestampStr();
                EmitEventLocked(*tq, "instructionsUpdated", job->id, 0);
            }
        }
    }

    if (resetAttempts) job->attempts.clear();
    job->state = "pending";
    job->blockedBy.clear();
    job->retryAtMs = 0;
    job->updatedAt = GetCurrentTimestampStr();
    ReevaluateAll();
    SaveQueue(*tq);
    cv_.notify_all();
    return "{\"ok\":true,\"source_changed\":false,\"job\":" + JobToJson(*job, true) + "}";
}

} // namespace pc

namespace pc {

std::string TaskQueueManager::TaskQueueUpdate(const std::string& id, const std::string& rawPatchJson, std::string& outError) {
    outError.clear();
    const std::string patchJson = NormalizeJobJsonKeys(rawPatchJson);
    std::lock_guard<std::mutex> lock(mutex_);
    TaskQueue* tq = nullptr;
    TaskQueueJob* job = FindJob(id, &tq);
    if (!job) {
        outError = "job not found: " + id;
        return BuildError(outError);
    }
    bool onlyNotes = HasJsonKey(patchJson, "notes");
    // Only "notes" may be updated after a job has finished or is running.
    if (onlyNotes) {
        for (const char* k : {"summary","command","cwd","timeout_sec","idle","max_attempts",
                              "retry_delay_sec","priority","kill_exclude","env","success",
                              "instructions_text","instructions_file","depends_on"}) {
            if (HasJsonKey(patchJson, k)) { onlyNotes = false; break; }
        }
    }
    if ((job->state == "running" || job->state == "succeeded" || job->state == "cancelled") && !onlyNotes) {
        outError = "job '" + id + "' can only be updated when pending, failed or blocked (except notes)";
        return BuildError(outError);
    }

    if (HasJsonKey(patchJson, "summary")) {
        std::string s = JsonFieldStr(patchJson, "summary", "");
        if (s.empty() || s.find('\n') != std::string::npos || s.find('\r') != std::string::npos || s.size() > 200) {
            outError = "summary must be a single line of 1..200 characters";
            return BuildError(outError);
        }
        job->summary = s;
    }
    if (HasJsonKey(patchJson, "command")) {
        std::string c = JsonFieldStr(patchJson, "command", "");
        if (c.empty()) {
            outError = "command must not be empty";
            return BuildError(outError);
        }
        std::string chainErr;
        if (!CheckChainGuard(c, chainErr)) {
            outError = chainErr;
            return BuildError(outError);
        }
        job->command = c;
    }
    if (HasJsonKey(patchJson, "cwd")) job->cwd = JsonFieldStr(patchJson, "cwd", "");
    if (HasJsonKey(patchJson, "timeout_sec")) {
        int t = static_cast<int>(JsonFieldInt(patchJson, "timeout_sec", 0));
        if (t <= 0) {
            outError = "timeout_sec must be greater than 0";
            return BuildError(outError);
        }
        if (tq->maxTimeoutSec > 0 && t > tq->maxTimeoutSec) {
            outError = "timeout_sec exceeds max_timeout_sec (" + std::to_string(tq->maxTimeoutSec) + ")";
            return BuildError(outError);
        }
        job->timeoutSec = t;
    }
    if (HasJsonKey(patchJson, "idle")) {
        std::string idleObj = JsonFieldObj(patchJson, "idle");
        job->idle.outputSec = static_cast<int>(JsonFieldInt(idleObj, "output_sec", job->idle.outputSec));
        job->idle.cpuSec = static_cast<int>(JsonFieldInt(idleObj, "cpu_sec", job->idle.cpuSec));
    }
    if (HasJsonKey(patchJson, "max_attempts")) {
        int m = static_cast<int>(JsonFieldInt(patchJson, "max_attempts", job->maxAttempts));
        if (m >= 1) job->maxAttempts = m;
    }
    if (HasJsonKey(patchJson, "retry_delay_sec")) {
        int r = static_cast<int>(JsonFieldInt(patchJson, "retry_delay_sec", job->retryDelaySec));
        job->retryDelaySec = (r > 0) ? r : 0;
    }
    if (HasJsonKey(patchJson, "priority")) job->priority = static_cast<int>(JsonFieldInt(patchJson, "priority", job->priority));
    if (HasJsonKey(patchJson, "kill_exclude")) job->killExclude = JsonFieldStrArray(patchJson, "kill_exclude");
    if (HasJsonKey(patchJson, "env")) job->env = ParseJsonStringMap(JsonFieldObj(patchJson, "env"));
    if (HasJsonKey(patchJson, "success")) {
        std::string successObj = JsonFieldObj(patchJson, "success");
        job->success.exitCode = static_cast<int>(JsonFieldInt(successObj, "exit_code", job->success.exitCode));
        job->success.filesNewer = JsonFieldStrArray(successObj, "files_newer");
        job->success.outputRegex = JsonFieldStr(successObj, "output_regex", "");
    }
    if (HasJsonKey(patchJson, "notes")) {
        job->notes = JsonFieldStr(patchJson, "notes", "");
    }

    std::string newText;
    bool haveNewText = false;
    if (HasJsonKey(patchJson, "instructions_text")) {
        newText = JsonFieldStr(patchJson, "instructions_text", "");
        haveNewText = true;
    } else if (HasJsonKey(patchJson, "instructions_file")) {
        std::string f = JsonFieldStr(patchJson, "instructions_file", "");
        std::wstring pathW = ResolveJobPathWide(*job, f);
        if (GetFileAttributesW(pathW.c_str()) == INVALID_FILE_ATTRIBUTES) {
            outError = "instructions_file not found: " + f;
            return BuildError(outError);
        }
        newText = ReadFileUtf8Wide(pathW);
        job->instructions.source = f;
        haveNewText = true;
    }
    if (haveNewText) {
        if (newText.size() > 1024 * 1024) {
            outError = "instructions exceed the 1 MB limit";
            return BuildError(outError);
        }
        job->instructions.text = newText;
        job->instructions.sha256 = Sha256Hex(newText);
        job->instructions.rev++;
        job->instructions.capturedAt = GetCurrentTimestampStr();
        EmitEventLocked(*tq, "instructionsUpdated", job->id, 0);
    }

    job->updatedAt = GetCurrentTimestampStr();
    ReevaluateAll();
    SaveQueue(*tq);
    cv_.notify_all();
    return "{\"ok\":true,\"job\":" + JobToJson(*job, true) + "}";
}

} // namespace pc

namespace pc {

std::string TaskQueueManager::TaskQueueCancel(const std::string& id, std::string& outError) {
    outError.clear();
    std::string resp;
    ProcessManager* pm = nullptr;
    int pid = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        TaskQueue* tq = nullptr;
        TaskQueueJob* job = FindJob(id, &tq);
        if (!job) {
            outError = "job not found: " + id;
            return BuildError(outError);
        }
        if (job->state == "cancelled") {
            return "{\"ok\":true,\"job\":" + JobToJson(*job, true) + "}";
        }
        if (job->state == "succeeded") {
            outError = "job '" + id + "' already succeeded";
            return BuildError(outError);
        }
        if (job->state == "running" && !job->attempts.empty()) {
            pid = job->attempts.back().processId;
        }
        job->state = "cancelled";
        job->blockedBy.clear();
        job->retryAtMs = 0;
        job->updatedAt = GetCurrentTimestampStr();
        EmitEventLocked(*tq, "jobCancelled", id, job->attempts.empty() ? 0 : job->attempts.back().n);
        ReevaluateAll();
        SaveQueue(*tq);
        resp = "{\"ok\":true,\"job\":" + JobToJson(*job, true) + "}";
        pm = pm_;
    }
    if (pm && pid > 0) {
        std::string err;
        pm->KillProcess(pid, err);
    }
    return resp;
}

std::string TaskQueueManager::TaskQueuePause(const std::string& taskqueue, bool paused, std::string& outError) {
    outError.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = queues_.find(taskqueue);
    if (it == queues_.end()) {
        outError = "task queue not found: " + taskqueue;
        return BuildError(outError);
    }
    it->second.paused = paused;
    it->second.version++;
    SaveQueue(it->second);
    if (!paused) cv_.notify_all();
    return "{\"ok\":true,\"taskqueue\":\"" + JsonEscape(taskqueue) + "\",\"paused\":" +
           (paused ? "true" : "false") + ",\"version\":" + std::to_string(it->second.version) + "}";
}

} // namespace pc

namespace pc {

std::string TaskQueueManager::TaskQueueWait(const std::string& taskqueue, int sinceVersion, int timeoutMs, std::string& outError) {
    outError.clear();
    uint64_t start = GetNowTickMs();
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        auto it = queues_.find(taskqueue);
        int64_t ver = (it == queues_.end()) ? 0 : it->second.version;
        if (ver != sinceVersion) break;
        if (timeoutMs >= 0 && static_cast<int64_t>(GetNowTickMs() - start) >= timeoutMs) break;
        cv_.wait_for(lock, std::chrono::milliseconds(100));
    }
    auto it = queues_.find(taskqueue);
    if (it == queues_.end()) {
        outError = "task queue not found: " + taskqueue;
        return BuildError(outError);
    }
    std::ostringstream oss;
    oss << "{\"ok\":true,\"taskqueue\":\"" << JsonEscape(taskqueue) << "\""
        << ",\"version\":" << it->second.version
        << ",\"since_version\":" << sinceVersion << ",\"jobs\":[";
    for (size_t i = 0; i < it->second.jobs.size(); ++i) {
        if (i > 0) oss << ",";
        oss << JobToJson(it->second.jobs[i], false);
    }
    oss << "]}";
    return oss.str();
}

std::string TaskQueueManager::TaskQueuePurge(const std::string& taskqueue, const std::string& statesJson, std::string& outError) {
    outError.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = queues_.find(taskqueue);
    if (it == queues_.end()) {
        outError = "task queue not found: " + taskqueue;
        return BuildError(outError);
    }
    std::vector<std::string> states;
    if (!statesJson.empty()) {
        states = JsonFieldStrArray("{\"states\":" + statesJson + "}", "states");
        if (states.empty()) states = JsonFieldStrArray(statesJson, "states");
    }
    if (states.empty()) states = {"succeeded", "failed", "cancelled"};

    size_t before = it->second.jobs.size();
    auto& jobs = it->second.jobs;
    jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [&](const TaskQueueJob& j) {
        return std::find(states.begin(), states.end(), j.state) != states.end();
    }), jobs.end());
    size_t purged = before - jobs.size();
    it->second.version++;
    SaveQueue(it->second);
    return "{\"ok\":true,\"taskqueue\":\"" + JsonEscape(taskqueue) + "\",\"purged\":" + std::to_string(purged) + "}";
}

} // namespace pc

namespace pc {

std::string TaskQueueManager::TaskQueueLink(const std::string& jobId, const std::string& depId, const std::string& on, std::string& outError) {
    outError.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    TaskQueue* tq = nullptr;
    TaskQueueJob* job = FindJob(jobId, &tq);
    if (!job) {
        outError = "job not found: " + jobId;
        return BuildError(outError);
    }
    if (job->state == "running" || job->state == "succeeded" || job->state == "cancelled") {
        outError = "dependencies can only be changed while pending, blocked or failed";
        return BuildError(outError);
    }
    if (depId.empty() || depId == jobId) {
        outError = "invalid dependency id";
        return BuildError(outError);
    }
    if (!FindJobConst(depId)) {
        outError = "unknown dependency: " + depId;
        return BuildError(outError);
    }
    for (const auto& d : job->dependsOn) {
        if (d.id == depId) {
            outError = "dependency already exists: " + depId;
            return BuildError(outError);
        }
    }
    std::string onVal = on.empty() ? "success" : on;
    if (onVal != "success" && onVal != "done") {
        outError = "on must be 'success' or 'done'";
        return BuildError(outError);
    }
    std::vector<TaskQueueDependency> newDeps = job->dependsOn;
    TaskQueueDependency nd;
    nd.id = depId;
    nd.on = onVal;
    newDeps.push_back(nd);
    if (HasDependencyCycle(*tq, jobId, newDeps, "")) {
        outError = "dependency cycle detected";
        return BuildError(outError);
    }
    job->dependsOn = newDeps;
    job->updatedAt = GetCurrentTimestampStr();
    EmitEventLocked(*tq, "dependencyLinked", jobId, 0);
    ReevaluateAll();
    SaveQueue(*tq);
    cv_.notify_all();
    return "{\"ok\":true,\"job\":" + JobToJson(*job, true) + "}";
}

std::string TaskQueueManager::TaskQueueUnlink(const std::string& jobId, const std::string& depId, std::string& outError) {
    outError.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    TaskQueue* tq = nullptr;
    TaskQueueJob* job = FindJob(jobId, &tq);
    if (!job) {
        outError = "job not found: " + jobId;
        return BuildError(outError);
    }
    if (job->state == "running" || job->state == "succeeded" || job->state == "cancelled") {
        outError = "dependencies can only be changed while pending, blocked or failed";
        return BuildError(outError);
    }
    auto& deps = job->dependsOn;
    auto it = std::remove_if(deps.begin(), deps.end(), [&](const TaskQueueDependency& d) { return d.id == depId; });
    if (it == deps.end()) {
        outError = "dependency not found: " + depId;
        return BuildError(outError);
    }
    deps.erase(it, deps.end());
    job->updatedAt = GetCurrentTimestampStr();
    EmitEventLocked(*tq, "dependencyUnlinked", jobId, 0);
    ReevaluateAll();
    SaveQueue(*tq);
    cv_.notify_all();
    return "{\"ok\":true,\"job\":" + JobToJson(*job, true) + "}";
}

} // namespace pc

namespace pc {

void TaskQueueManager::SaveAllLocked() {
    for (const auto& [name, tq] : queues_) {
        (void)name;
        SaveQueue(tq);
    }
}

bool TaskQueueManager::ReevaluateJob(TaskQueue& tq, TaskQueueJob& job) {
    if (job.state == "running" || job.state == "succeeded" || job.state == "cancelled") return false;

    std::string blocker;
    for (const auto& d : job.dependsOn) {
        const TaskQueueJob* dep = FindJobConst(d.id);
        if (!dep) {
            for (const auto& tj : tq.jobs) {
                if (tj.id == d.id) { dep = &tj; break; }
            }
        }
        if (!dep) { blocker = d.id; break; }
        if (d.on == "success" &&
            (dep->state == "failed" || dep->state == "cancelled" || dep->state == "blocked")) {
            blocker = dep->id;
            break;
        }
    }

    if (!blocker.empty()) {
        if (job.state != "blocked" || job.blockedBy != blocker) {
            job.state = "blocked";
            job.blockedBy = blocker;
            job.updatedAt = GetCurrentTimestampStr();
            EmitEventLocked(tq, "jobBlocked", job.id, 0);
            return true;
        }
        return false;
    }

    if (job.state == "blocked") {
        job.state = "pending";
        job.blockedBy.clear();
        job.updatedAt = GetCurrentTimestampStr();
        return true;
    }
    return false;
}

void TaskQueueManager::ReevaluateAll() {
    for (int iter = 0; iter < 64; ++iter) {
        bool changed = false;
        for (auto& [name, tq] : queues_) {
            (void)name;
            for (auto& job : tq.jobs) {
                if (ReevaluateJob(tq, job)) changed = true;
            }
        }
        if (!changed) break;
    }
}

void TaskQueueManager::ReevaluateDependents(const std::string& jobId) {
    (void)jobId;
    ReevaluateAll();
}

void TaskQueueManager::CheckQueueNotices(TaskQueue& tq) {
    int pendingCount = 0;
    int runningCount = 0;
    bool anyReady = false;
    int64_t now = static_cast<int64_t>(GetNowTickMs());
    for (const auto& job : tq.jobs) {
        if (job.state == "pending") {
            pendingCount++;
            bool allSat = true;
            for (const auto& d : job.dependsOn) {
                const TaskQueueJob* dep = FindJobConst(d.id);
                if (!dep || !IsDepSatisfied(*dep, d.on)) { allSat = false; break; }
            }
            if (allSat && job.retryAtMs <= now) anyReady = true;
        } else if (job.state == "running") {
            runningCount++;
        }
    }

    std::string notice;
    if (!tq.jobs.empty() && pendingCount == 0 && runningCount == 0) {
        notice = "taskqueueDrained";
    } else if (pendingCount > 0 && !anyReady && !tq.paused) {
        notice = "taskqueueStalled";
    }

    std::string& last = lastNotice_[tq.name];
    if (notice != last) {
        last = notice;
        if (!notice.empty()) EmitEventLocked(tq, notice, "", 0);
    }
}

} // namespace pc

namespace pc {

namespace {

void CollectUpstream(const std::map<std::string, TaskQueue>& queues, const std::string& id, std::set<std::string>& keep) {
    if (!keep.insert(id).second) return;
    for (const auto& [name, tq] : queues) {
        (void)name;
        for (const auto& job : tq.jobs) {
            if (job.id != id) continue;
            for (const auto& d : job.dependsOn) CollectUpstream(queues, d.id, keep);
        }
    }
}

void CollectDownstream(const std::map<std::string, TaskQueue>& queues, const std::string& id, std::set<std::string>& keep) {
    if (!keep.insert(id).second) return;
    for (const auto& [name, tq] : queues) {
        (void)name;
        for (const auto& job : tq.jobs) {
            for (const auto& d : job.dependsOn) {
                if (d.id == id) CollectDownstream(queues, job.id, keep);
            }
        }
    }
}

} // namespace

std::string TaskQueueManager::TaskQueueGraph(const std::string& taskqueue, const std::string& root, std::string& outError) {
    outError.clear();
    std::lock_guard<std::mutex> lock(mutex_);

    std::set<std::string> keep;
    if (!root.empty()) {
        if (!FindJobConst(root)) {
            outError = "job not found: " + root;
            return BuildError(outError);
        }
        CollectUpstream(queues_, root, keep);
        CollectDownstream(queues_, root, keep);
    }

    std::ostringstream oss;
    oss << "{\"ok\":true,\"jobs\":[";
    bool first = true;
    int64_t now = static_cast<int64_t>(GetNowTickMs());
    for (const auto& [name, tq] : queues_) {
        if (!taskqueue.empty() && name != taskqueue) continue;
        for (const auto& job : tq.jobs) {
            if (!root.empty() && keep.find(job.id) == keep.end()) continue;

            std::vector<std::string> waiting;
            bool allSat = true;
            for (const auto& d : job.dependsOn) {
                const TaskQueueJob* dep = FindJobConst(d.id);
                if (!dep || !IsDepSatisfied(*dep, d.on)) {
                    allSat = false;
                    waiting.push_back(d.id);
                }
            }
            bool ready = (job.state == "pending") && allSat && job.retryAtMs <= now;
            std::vector<std::string> dependents;
            for (const auto& [n2, tq2] : queues_) {
                (void)n2;
                for (const auto& j2 : tq2.jobs) {
                    for (const auto& d2 : j2.dependsOn) {
                        if (d2.id == job.id) { dependents.push_back(j2.id); break; }
                    }
                }
            }

            std::string jj = JobToJson(job, false);
            if (!jj.empty() && jj.back() == '}') jj.pop_back();
            if (!first) oss << ",";
            first = false;
            oss << jj << ",\"ready\":" << (ready ? "true" : "false")
                << ",\"waiting_for\":" << StrArrayToJson(waiting)
                << ",\"dependents\":" << StrArrayToJson(dependents) << "}";
        }
    }
    oss << "]}";
    return oss.str();
}

} // namespace pc

namespace pc {

static uint64_t NowFiletime() {
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

bool TaskQueueManager::EvaluateSuccess(const TaskQueueJob& job, TaskQueueAttempt& attempt,
                                       const std::string& endKind, int exitCode,
                                       const std::string& logFile, std::string& detail) {
    if (endKind != "completed") {
        detail = endKind;
        return false;
    }
    if (exitCode != job.success.exitCode) {
        detail = "exit code " + std::to_string(exitCode) + " (expected " + std::to_string(job.success.exitCode) + ")";
        return false;
    }
    for (const auto& rel : job.success.filesNewer) {
        std::wstring pathW = ResolveJobPathWide(job, rel);
        HANDLE h = CreateFileW(pathW.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            detail = "missing file: " + rel;
            return false;
        }
        FILETIME ft = {};
        bool ok = GetFileTime(h, nullptr, nullptr, &ft) != FALSE;
        CloseHandle(h);
        if (!ok) {
            detail = "cannot read mtime: " + rel;
            return false;
        }
        uint64_t fileTime = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        if (attempt.startFiletime > 0 && fileTime <= attempt.startFiletime) {
            detail = "file not newer than attempt start: " + rel;
            return false;
        }
    }
    if (!job.success.outputRegex.empty()) {
        std::string text;
        if (!logFile.empty()) text = ReadFileUtf8Wide(Utf8ToWide(logFile));
        text = GetTailLines(text, 200);
        try {
            std::regex re(job.success.outputRegex);
            if (!std::regex_search(text, re)) {
                detail = "output_regex did not match";
                return false;
            }
        } catch (const std::regex_error&) {
            detail = "invalid output_regex";
            return false;
        }
    }
    detail = "ok";
    return true;
}

void TaskQueueManager::ApplyAttemptOutcome(TaskQueue& tq, TaskQueueJob& job, bool success,
                                           const std::string& reason, int exitCode, const std::string& checkDetail) {
    if (job.attempts.empty()) return;
    TaskQueueAttempt& a = job.attempts.back();
    a.end = GetCurrentTimestampStr();
    a.reason = reason;
    a.exitCode = exitCode;
    a.checkDetail = checkDetail;
    job.updatedAt = a.end;

    if (job.state == "cancelled") {
        SaveQueue(tq);
        cv_.notify_all();
        return;
    }

    if (success) {
        job.state = "succeeded";
        job.blockedBy.clear();
        job.retryAtMs = 0;
        EmitEventLocked(tq, "jobSucceeded", job.id, a.n);
        ReevaluateAll();
        SaveAllLocked();
        CheckQueueNotices(tq);
        cv_.notify_all();
        return;
    }

    EmitEventLocked(tq, "jobAttemptFailed", job.id, a.n);
    // An attempt cut short by a ProcessCommander restart is not the job's fault,
    // so it does not use up max_attempts (spec: interrupted jobs are re-run).
    int countedAttempts = 0;
    for (const auto& att : job.attempts) {
        if (att.reason != "interrupted") countedAttempts++;
    }
    if (countedAttempts < job.maxAttempts) {
        job.state = "pending";
        job.retryAtMs = static_cast<int64_t>(GetNowTickMs()) + static_cast<int64_t>(job.retryDelaySec) * 1000;
    } else {
        job.state = "failed";
        job.retryAtMs = 0;
        EmitEventLocked(tq, "jobFailed", job.id, a.n);
        ReevaluateAll();
    }
    SaveAllLocked();
    CheckQueueNotices(tq);
    cv_.notify_all();
}

} // namespace pc

namespace pc {

static std::wstring GetTaskQueueLogDirWide() {
    std::wstring dir = GetLogsDirectoryWide() + L"\\taskqueue";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

static std::wstring GetAttemptLogPathWide(const std::string& jobId, int n) {
    return GetTaskQueueLogDirWide() + L"\\" + Utf8ToWide(SanitizeFileNameComponent(jobId)) +
           L"." + std::to_wstring(n) + L".log";
}

static std::string QuoteCommandArg(const std::string& s) {
    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : s) {
        if (c == '\\') {
            backslashes++;
            out.push_back(c);
        } else if (c == '"') {
            out.append(backslashes + 1, '\\');
            out.push_back('"');
            backslashes = 0;
        } else {
            backslashes = 0;
            out.push_back(c);
        }
    }
    out.append(backslashes, '\\');
    out.push_back('"');
    return out;
}

bool TaskQueueManager::BuildLaunchRequest(TaskQueue& tq, TaskQueueJob& job, int attemptN, LaunchItem& out) {
    out.taskqueue = tq.name;
    out.jobId = job.id;
    out.attemptN = attemptN;

    std::string instrPathUtf8;
    int rev = job.instructions.rev > 0 ? job.instructions.rev : 1;
    if (!job.instructions.text.empty()) {
        std::wstring instrW = GetInstructionsFileWide(tq.name, job.id, rev);
        WriteFileAtomicWide(instrW, job.instructions.text);
        instrPathUtf8 = WideToUtf8(instrW);
    }

    std::string cmd = job.command;
    size_t pos = 0;
    while ((pos = cmd.find("{instructions}", pos)) != std::string::npos) {
        cmd.replace(pos, 14, instrPathUtf8);
        pos += instrPathUtf8.size();
    }
    pos = 0;
    std::string quotedSummary = QuoteCommandArg(job.summary);
    while ((pos = cmd.find("{summary}", pos)) != std::string::npos) {
        cmd.replace(pos, 9, quotedSummary);
        pos += quotedSummary.size();
    }

    StartProcessRequest req;
    req.name = job.name.empty() ? job.id : job.name;
    req.commandLine = cmd;
    req.workingDir = job.cwd;
    req.mode = ExecMode::Pipe;
    req.openStdin = false;
    req.timeoutSec = job.timeoutSec;
    req.logFile = WideToUtf8(GetAttemptLogPathWide(job.id, attemptN));
    req.jobKey = job.id;
    req.jobId = job.num;
    req.attemptN = attemptN;
    req.supervisorGoal = job.summary;
    req.supervisorNotes = "Task ID: " + job.id + (job.summary.empty() ? "" : " - " + job.summary);
    req.agentType = DetectAgentType(req.commandLine, req.name);
    if (!req.agentType.empty()) {
        req.jsonStream = true;
    }
    req.env = job.env;
    req.killExclude = job.killExclude;
    if (job.idle.outputSec > 0 && job.idle.cpuSec > 0) {
        req.idleTimeoutSec = job.idle.outputSec;
        req.idleCpuSec = job.idle.cpuSec;
    }

    req.env["PC_JOB_ID"] = job.id;
    req.env["PC_ATTEMPT"] = std::to_string(attemptN);
    req.env["PC_JOB_SUMMARY"] = job.summary;
    if (!instrPathUtf8.empty()) {
        req.env["PC_JOB_INSTRUCTIONS"] = instrPathUtf8;
        req.env["PC_JOB_INSTRUCTIONS_REV"] = std::to_string(rev);
    }
    if (job.attempts.size() >= 2) {
        const TaskQueueAttempt& prev = job.attempts[job.attempts.size() - 2];
        req.env["PC_PREV_REASON"] = prev.reason;
        req.env["PC_PREV_EXIT"] = std::to_string(prev.exitCode);
        if (!prev.lastLinesFile.empty()) req.env["PC_PREV_TAIL"] = prev.lastLinesFile;
    }

    out.req = std::move(req);
    return true;
}

} // namespace pc

namespace pc {

void TaskQueueManager::ProcessSchedulerTick() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pm_) return;
    int64_t now = static_cast<int64_t>(GetNowTickMs());

    for (auto& [name, tq] : queues_) {
        (void)name;
        if (tq.paused) continue;
        int running = 0;
        for (const auto& j : tq.jobs) {
            if (j.state == "running") running++;
        }
        if (running >= tq.concurrency) continue;

        std::vector<size_t> ready;
        for (size_t i = 0; i < tq.jobs.size(); ++i) {
            TaskQueueJob& j = tq.jobs[i];
            if (j.state != "pending" || j.retryAtMs > now) continue;
            bool allSat = true;
            for (const auto& d : j.dependsOn) {
                const TaskQueueJob* dep = FindJobConst(d.id);
                if (!dep || !IsDepSatisfied(*dep, d.on)) { allSat = false; break; }
            }
            if (allSat) ready.push_back(i);
        }
        std::stable_sort(ready.begin(), ready.end(), [&](size_t a, size_t b) {
            if (tq.jobs[a].priority != tq.jobs[b].priority) return tq.jobs[a].priority > tq.jobs[b].priority;
            return tq.jobs[a].createdAt < tq.jobs[b].createdAt;
        });

        bool changed = false;
        for (size_t idx : ready) {
            if (running >= tq.concurrency) break;
            TaskQueueJob& job = tq.jobs[idx];
            int n = static_cast<int>(job.attempts.size()) + 1;
            LaunchItem item;
            BuildLaunchRequest(tq, job, n, item);
            TaskQueueAttempt a;
            a.n = n;
            a.start = GetCurrentTimestampStr();
            a.instructionsRev = job.instructions.rev;
            a.startFiletime = NowFiletime();
            job.attempts.push_back(a);
            job.state = "running";
            job.updatedAt = a.start;
            EmitEventLocked(tq, "jobStarted", job.id, n);

            std::string err;
            int pid = pm_->StartProcess(item.req, err);
            if (pid > 0) {
                job.attempts.back().processId = pid;
                running++;
            } else {
                ApplyAttemptOutcome(tq, job, false, "launchFailed", -1, err);
            }
            changed = true;
        }
        if (changed) SaveAllLocked();
    }
}

void TaskQueueManager::SchedulerLoop() {
    while (schedulerRunning_.load()) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(300));
        }
        if (!schedulerRunning_.load()) break;
        ProcessSchedulerTick();
    }
}

void TaskQueueManager::OnProcessFinished(int processId, int exitCode, const std::string& endKind) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [name, tq] : queues_) {
        (void)name;
        for (auto& job : tq.jobs) {
            if (job.attempts.empty()) continue;
            TaskQueueAttempt& a = job.attempts.back();
            if (a.processId != processId) continue;
            if (job.state != "running" && job.state != "cancelled") return;

            std::wstring logPath = GetAttemptLogPathWide(job.id, a.n);
            std::string logText = ReadFileUtf8Wide(logPath);
            if (!logText.empty()) {
                std::wstring tailPath = GetTaskQueueLogDirWide() + L"\\" +
                    Utf8ToWide(SanitizeFileNameComponent(job.id)) + L"." + std::to_wstring(a.n) + L".tail.txt";
                WriteFileAtomicWide(tailPath, GetTailLines(logText, 50));
                a.lastLinesFile = WideToUtf8(tailPath);
            }

            if (job.state == "cancelled") {
                a.end = GetCurrentTimestampStr();
                a.reason = "killed";
                a.exitCode = exitCode;
                a.checkDetail = "cancelled";
                job.updatedAt = a.end;
                tq.version++;
                std::string ev = "{\"version\":" + std::to_string(tq.version) +
                                 ",\"type\":\"jobCancelled\",\"taskqueue\":\"" + JsonEscape(tq.name) +
                                 "\",\"job_id\":\"" + JsonEscape(job.id) +
                                 "\",\"attempt\":" + std::to_string(a.n) +
                                 ",\"timestamp\":\"" + GetCurrentTimestampStr() + "\"}\r\n";
                AppendToLogFileWide(GetTaskEventsFileWide(tq.name), ev);
                if (!tq.notifyFile.empty()) {
                    std::string notify = ev;
                    if (notify.size() >= 2 && notify[notify.size()-2]=='\r' && notify[notify.size()-1]=='\n') notify = notify.substr(0, notify.size()-2);
                    WriteFileAtomicWide(Utf8ToWide(tq.notifyFile), notify);
                }
                SaveQueue(tq);
                CheckQueueNotices(tq);
                cv_.notify_all();
                return;
            }

            std::string detail;
            bool ok = EvaluateSuccess(job, a, endKind, exitCode, WideToUtf8(logPath), detail);
            std::string reason;
            if (ok) reason = "ok";
            else if (endKind == "completed") reason = "checkFailed";
            else reason = endKind;
            ApplyAttemptOutcome(tq, job, ok, reason, exitCode, detail);
            return;
        }
    }
}

} // namespace pc
