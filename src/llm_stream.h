#pragma once

#include "common.h"
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <cstdint>

namespace pc {

// Structured representation of a single streamed JSON event from Claude, agy, or OpenCode.
struct LlmStreamEvent {
    uint64_t seq = 0;
    std::string timestamp;
    std::string rawJson;
    std::string type;          // "tool_call", "tool_result", "thinking", "message", "status", "step", "error", "usage", "other"
    std::string agentType;     // "claude", "agy", "opencode", "generic"
    std::string model;         // model name/id if present in stream
    std::string toolName;      // e.g. "view_file", "edit_file", "Bash"
    std::string toolAction;    // e.g. "Viewing file", "Editing src/gui_dashboard.cpp"
    std::string toolInput;     // arguments or parameters
    std::string content;       // thinking reasoning or assistant message text
    std::string status;        // "thinking", "calling_tool", "generating", "idle", "completed", "error"
    std::string messageId;     // Claude message.id; usage is per message, not cumulative
    int stepIndex = 0;
    uint64_t totalTokens = 0;
    uint64_t inputTokens = 0;
    uint64_t outputTokens = 0;
    uint64_t thinkingTokens = 0;
    uint64_t cacheReadTokens = 0;
    uint64_t cacheWriteTokens = 0;
    double costUsd = 0.0;
};

// Summary state of an LLM agent process
struct LlmAgentState {
    std::string agentType;      // "claude", "agy", "opencode", or empty
    std::string model;          // model name/id if present in stream
    std::string status = "idle"; // "idle", "thinking", "calling_tool", "generating", "completed", "error"
    std::string currentTool;
    std::string currentToolAction;
    std::string lastThought;
    std::string lastMessage;
    std::string lastError;
    int stepCount = 0;
    int eventCount = 0;
    uint64_t totalTokens = 0;
    double costUsd = 0.0;
    std::string streamFilePath;
    std::string supervisorNotes;
    std::string supervisorGoal;
    std::string updatedAt;
};

// Auto-detect agent type ("claude", "agy", "opencode") from command line or process name.
std::string DetectAgentType(const std::string& commandLine, const std::string& name = "");

// Parse a single raw JSON line emitted by claude, agy, or opencode.
// Returns true if successfully parsed as a JSON event.
bool ParseLlmStreamLine(const std::string& line, const std::string& defaultAgentType, LlmStreamEvent& outEvent);

// Format events as JSON array or human-readable summary
std::string LlmEventsToJson(const std::vector<LlmStreamEvent>& events, const LlmAgentState& state);
std::string LlmEventsToSummary(const std::vector<LlmStreamEvent>& events, const LlmAgentState& state);

// Human-readable usage summary (tokens / cache / cost) for one event. Empty if no usage data.
std::string LlmEventUsageText(const LlmStreamEvent& ev);

// Process-wide token usage history used for dashboard graphs.
struct LlmUsagePoint {
    int64_t epochSec = 0;
    uint64_t inputTokens = 0;
    uint64_t outputTokens = 0;
    uint64_t thinkingTokens = 0;
    uint64_t cacheReadTokens = 0;
    uint64_t cacheWriteTokens = 0;
    uint64_t totalTokens = 0;
    double costUsd = 0.0;
    std::string agentType;
    std::string model;
};

void RecordLlmUsagePoint(int64_t epochSec, const LlmUsagePoint& point);
std::vector<LlmUsagePoint> GetLlmUsageHistory(int64_t sinceEpochSec);
void SetUsageHistoryPersistence(bool enabled);
bool IsUsageHistoryPersistence();
uint64_t GetLlmUsageRecordCalls();

// Thread-safe tracker that ingests raw output chunks, extracts JSONL lines,
// persists them to logs/proc_<id>_<name>_stream.jsonl, and maintains the current state.
class LlmStreamTracker {
public:
    LlmStreamTracker() = default;

    void Init(int processId, const std::string& agentType, const std::string& streamFilePath,
              const std::string& supervisorNotes = "", const std::string& supervisorGoal = "",
              const std::string& defaultModel = "");

    // Ingests raw chunks from process stdout (pipe or ConPTY)
    void IngestChunk(const char* data, size_t len);

    // Update supervisor context
    void UpdateSupervisorInfo(const std::string& notes, const std::string& goal);

    // Query state and events
    LlmAgentState GetState() const;
    std::vector<LlmStreamEvent> GetEvents(int tail = 20, const std::string& typeFilter = "", uint64_t sinceSeq = 0) const;
    std::string GetEventsJson(int tail = 20, const std::string& typeFilter = "", uint64_t sinceSeq = 0) const;
    std::string GetEventsSummary(int tail = 20, const std::string& typeFilter = "") const;
    std::string GetEventsRawJsonl(int tail = 20) const;

    bool HasEvents() const;
    uint64_t EventCount() const;

private:
    void ProcessLine(const std::string& line);

    mutable std::mutex mutex_;
    int processId_ = 0;
    std::string agentType_;
    std::string conversationId_;
    uint64_t lastAgyModelProbeMs_ = 0;
    std::string lastMessageId_;
    std::vector<LlmUsagePoint> pendingUsage_;  // recorded by IngestChunk after releasing mutex_
    std::string streamFilePath_;
    std::wstring streamFilePathWide_;
    std::string lineBuffer_;
    bool discardingLine_ = false;
    std::deque<LlmStreamEvent> events_;
    LlmAgentState state_;
    uint64_t nextSeq_ = 1;
    uint64_t lastUsageTotal_ = 0;
    uint64_t lastInput_ = 0;
    uint64_t lastOutput_ = 0;
    uint64_t lastThinking_ = 0;
    uint64_t lastCacheRead_ = 0;
    uint64_t lastCacheWrite_ = 0;
    double lastUsageCost_ = 0.0;
    static constexpr size_t kMaxEvents = 500;
    static constexpr uint64_t kAgyModelProbeIntervalMs = 5000;
    // Claude tool_result lines (file contents etc.) routinely exceed 64KB.
    static constexpr size_t kMaxLineBytes = 16 * 1024 * 1024;
};

} // namespace pc
