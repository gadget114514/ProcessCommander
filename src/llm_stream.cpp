#include "llm_stream.h"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>

namespace pc {

namespace {

static std::string ToLowerStr(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

static bool WordMatches(const std::string& text, const std::string& word) {
    size_t p = text.find(word);
    while (p != std::string::npos) {
        bool leftOk = (p == 0) || !std::isalnum(static_cast<unsigned char>(text[p - 1]));
        bool rightOk = (p + word.size() >= text.size()) || !std::isalnum(static_cast<unsigned char>(text[p + word.size()]));
        if (leftOk && rightOk) return true;
        p = text.find(word, p + 1);
    }
    return false;
}

static std::string TrimStr(const std::string& s) {
    size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

} // namespace

std::string DetectAgentType(const std::string& commandLine, const std::string& name) {
    std::string lcmd = ToLowerStr(commandLine);
    std::string lname = ToLowerStr(name);

    if (WordMatches(lcmd, "claude") || WordMatches(lname, "claude")) return "claude";
    if (WordMatches(lcmd, "agy") || WordMatches(lcmd, "antigravity") ||
        WordMatches(lname, "agy") || WordMatches(lname, "antigravity")) return "agy";
    if (WordMatches(lcmd, "opencode") || WordMatches(lname, "opencode")) return "opencode";
    if (WordMatches(lcmd, "codex") || WordMatches(lname, "codex")) return "codex";
    if (WordMatches(lcmd, "aider") || WordMatches(lname, "aider")) return "aider";
    if (WordMatches(lcmd, "ollama") || WordMatches(lname, "ollama")) return "ollama";

    return "";
}

bool ParseLlmStreamLine(const std::string& line, const std::string& defaultAgentType, LlmStreamEvent& outEvent) {
    std::string trimmed = TrimStr(line);
    if (trimmed.size() < 2 || trimmed.front() != '{' || trimmed.back() != '}') {
        return false;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(trimmed);
    } catch (...) {
        return false;
    }
    if (!j.is_object()) return false;

    outEvent = LlmStreamEvent{};
    outEvent.rawJson = trimmed;
    outEvent.agentType = defaultAgentType.empty() ? "generic" : defaultAgentType;

    auto getStr = [&](const nlohmann::json& obj, const std::string& key) -> std::string {
        if (obj.contains(key)) {
            if (obj[key].is_string()) return obj[key].get<std::string>();
            if (obj[key].is_number() || obj[key].is_boolean()) return obj[key].dump();
        }
        return "";
    };

    if (j.contains("agent") && j["agent"].is_string()) {
        outEvent.agentType = j["agent"].get<std::string>();
    }

    // Step index
    if (j.contains("step_index") && j["step_index"].is_number()) {
        outEvent.stepIndex = j["step_index"].get<int>();
    } else if (j.contains("step") && j["step"].is_number()) {
        outEvent.stepIndex = j["step"].get<int>();
    }

    // Timestamp
    outEvent.timestamp = getStr(j, "timestamp");
    if (outEvent.timestamp.empty()) outEvent.timestamp = getStr(j, "created_at");
    if (outEvent.timestamp.empty()) outEvent.timestamp = getStr(j, "time");
    if (outEvent.timestamp.empty()) outEvent.timestamp = GetCurrentTimestampStr();

    std::string rawType = getStr(j, "type");
    if (rawType.empty()) rawType = getStr(j, "event");
    if (rawType.empty()) rawType = getStr(j, "role");
    std::string lType = ToLowerStr(rawType);

    // 1. Tool Call / Tool Use (Claude, agy, OpenCode, Codex)
    if (rawType == "PLANNER_RESPONSE" || j.contains("tool_calls")) {
        outEvent.type = "tool_call";
        outEvent.status = "calling_tool";
        if (j.contains("thinking") && j["thinking"].is_string()) {
            outEvent.content = j["thinking"].get<std::string>();
        }
        if (j.contains("tool_calls") && j["tool_calls"].is_array() && !j["tool_calls"].empty()) {
            const auto& tc = j["tool_calls"][0];
            if (tc.is_object()) {
                outEvent.toolName = getStr(tc, "toolName");
                if (outEvent.toolName.empty()) outEvent.toolName = getStr(tc, "name");
                if (outEvent.toolName.empty() && tc.contains("function")) {
                    outEvent.toolName = getStr(tc["function"], "name");
                }
                outEvent.toolAction = getStr(tc, "toolAction");
                if (outEvent.toolAction.empty()) outEvent.toolAction = getStr(tc, "toolSummary");
                if (outEvent.toolAction.empty()) outEvent.toolAction = getStr(tc, "action");
                if (tc.contains("parameters")) outEvent.toolInput = tc["parameters"].dump();
                else if (tc.contains("arguments")) outEvent.toolInput = tc["arguments"].dump();
                else if (tc.contains("input")) outEvent.toolInput = tc["input"].dump();
            }
        }
        if (outEvent.toolAction.empty()) outEvent.toolAction = outEvent.toolName;
        return true;
    }

    if (lType == "tool_use" || lType == "tool_call" || lType == "call" ||
        lType == "function_call" || j.contains("tool_call")) {
        outEvent.type = "tool_call";
        outEvent.status = "calling_tool";

        const nlohmann::json* targetObj = &j;
        if (j.contains("tool_call") && j["tool_call"].is_object()) {
            targetObj = &j["tool_call"];
        }

        outEvent.toolName = getStr(*targetObj, "name");
        if (outEvent.toolName.empty()) outEvent.toolName = getStr(*targetObj, "tool");
        if (outEvent.toolName.empty()) outEvent.toolName = getStr(*targetObj, "tool_name");
        if (outEvent.toolName.empty() && targetObj->contains("function") && (*targetObj)["function"].is_object()) {
            outEvent.toolName = getStr((*targetObj)["function"], "name");
        }

        outEvent.toolAction = getStr(*targetObj, "toolAction");
        if (outEvent.toolAction.empty()) outEvent.toolAction = getStr(*targetObj, "action");
        if (outEvent.toolAction.empty()) outEvent.toolAction = getStr(*targetObj, "summary");

        if (targetObj->contains("input")) {
            outEvent.toolInput = (*targetObj)["input"].is_string() ? (*targetObj)["input"].get<std::string>() : (*targetObj)["input"].dump();
        } else if (targetObj->contains("arguments")) {
            outEvent.toolInput = (*targetObj)["arguments"].is_string() ? (*targetObj)["arguments"].get<std::string>() : (*targetObj)["arguments"].dump();
        } else if (targetObj->contains("parameters")) {
            outEvent.toolInput = (*targetObj)["parameters"].is_string() ? (*targetObj)["parameters"].get<std::string>() : (*targetObj)["parameters"].dump();
        }

        if (outEvent.toolAction.empty() && !outEvent.toolName.empty()) {
            outEvent.toolAction = "Calling " + outEvent.toolName;
        }
        return true;
    }

    // 2. Tool Result
    if (lType == "tool_result" || lType == "function_result" || lType == "tool_response") {
        outEvent.type = "tool_result";
        outEvent.status = "tool_result";
        outEvent.toolName = getStr(j, "name");
        if (outEvent.toolName.empty()) outEvent.toolName = getStr(j, "tool");
        if (j.contains("content")) {
            outEvent.content = j["content"].is_string() ? j["content"].get<std::string>() : j["content"].dump();
        } else if (j.contains("output")) {
            outEvent.content = j["output"].is_string() ? j["output"].get<std::string>() : j["output"].dump();
        } else if (j.contains("result")) {
            outEvent.content = j["result"].is_string() ? j["result"].get<std::string>() : j["result"].dump();
        }
        return true;
    }

    // 3. Thinking / Chain-of-Thought (Claude, agy, OpenAI o1/o3/codex reasoning)
    if (lType == "thinking" || lType == "thought" || lType == "reasoning" || j.contains("thinking") || j.contains("thought")) {
        outEvent.type = "thinking";
        outEvent.status = "thinking";
        outEvent.content = getStr(j, "thinking");
        if (outEvent.content.empty()) outEvent.content = getStr(j, "thought");
        if (outEvent.content.empty()) outEvent.content = getStr(j, "reasoning");
        if (outEvent.content.empty()) outEvent.content = getStr(j, "message");
        if (outEvent.content.empty()) outEvent.content = getStr(j, "content");
        return true;
    }

    // 4. Message / Assistant response / Delta
    if (lType == "message" || lType == "assistant" || lType == "content_block_delta" ||
        lType == "delta" || lType == "user" || lType == "user_input" || j.contains("message")) {
        outEvent.type = (lType == "user" || lType == "user_input") ? "user_input" : "message";
        outEvent.status = "generating";
        if (j.contains("delta") && j["delta"].is_object()) {
            outEvent.content = getStr(j["delta"], "content");
            if (outEvent.content.empty()) outEvent.content = getStr(j["delta"], "text");
        } else {
            outEvent.content = getStr(j, "content");
            if (outEvent.content.empty()) outEvent.content = getStr(j, "message");
            if (outEvent.content.empty()) outEvent.content = getStr(j, "text");
        }
        return true;
    }

    // 5. Status / Progress
    if (lType == "status" || lType == "progress" || lType == "step" || lType == "step_start" || lType == "step_finish" || j.contains("status")) {
        outEvent.type = (lType.find("step") != std::string::npos) ? "step" : "status";
        std::string st = getStr(j, "status");
        if (st.empty()) st = getStr(j, "state");
        outEvent.status = st.empty() ? "running" : st;
        std::string summary = getStr(j, "summary");
        if (summary.empty()) summary = getStr(j, "action");
        if (summary.empty()) summary = getStr(j, "message");
        if (summary.empty()) summary = getStr(j, "description");
        outEvent.content = summary;
        return true;
    }

    // 6. Cost / Usage
    if (lType == "cost" || lType == "usage" || j.contains("usage") || j.contains("total_tokens")) {
        outEvent.type = "usage";
        outEvent.status = "idle";
        const nlohmann::json* u = j.contains("usage") ? &j["usage"] : &j;
        if (u->contains("total_tokens") && (*u)["total_tokens"].is_number()) {
            outEvent.totalTokens = (*u)["total_tokens"].get<uint64_t>();
        } else if (u->contains("tokens") && (*u)["tokens"].is_number()) {
            outEvent.totalTokens = (*u)["tokens"].get<uint64_t>();
        }
        if (u->contains("cost_usd") && (*u)["cost_usd"].is_number()) {
            outEvent.costUsd = (*u)["cost_usd"].get<double>();
        }
        return true;
    }

    // 7. Error
    if (lType == "error" || j.contains("error")) {
        outEvent.type = "error";
        outEvent.status = "error";
        if (j["error"].is_string()) {
            outEvent.content = j["error"].get<std::string>();
        } else if (j["error"].is_object()) {
            outEvent.content = getStr(j["error"], "message");
            if (outEvent.content.empty()) outEvent.content = j["error"].dump();
        }
        return true;
    }

    // 8. Result / Done
    if (lType == "result" || lType == "done" || lType == "finish" || lType == "completed") {
        outEvent.type = "result";
        outEvent.status = "completed";
        outEvent.content = getStr(j, "summary");
        if (outEvent.content.empty()) outEvent.content = getStr(j, "result");
        return true;
    }

    // Generic fallback for any valid JSON object
    outEvent.type = rawType.empty() ? "event" : rawType;
    outEvent.content = getStr(j, "content");
    if (outEvent.content.empty()) outEvent.content = getStr(j, "summary");
    if (outEvent.content.empty()) outEvent.content = getStr(j, "text");
    outEvent.status = "running";
    return true;
}

std::string LlmEventsToJson(const std::vector<LlmStreamEvent>& events, const LlmAgentState& state) {
    nlohmann::json root;
    root["ok"] = true;
    root["agent_type"] = state.agentType;
    root["status"] = state.status;
    root["current_tool"] = state.currentTool;
    root["current_tool_action"] = state.currentToolAction;
    root["last_thought"] = state.lastThought;
    root["last_message"] = state.lastMessage;
    root["step_count"] = state.stepCount;
    root["event_count"] = state.eventCount;
    root["total_tokens"] = state.totalTokens;
    root["cost_usd"] = state.costUsd;
    root["stream_file"] = state.streamFilePath;
    root["supervisor_notes"] = state.supervisorNotes;
    root["supervisor_goal"] = state.supervisorGoal;
    root["updated_at"] = state.updatedAt;

    nlohmann::json evArr = nlohmann::json::array();
    for (const auto& ev : events) {
        nlohmann::json item;
        item["seq"] = ev.seq;
        item["timestamp"] = ev.timestamp;
        item["type"] = ev.type;
        item["agent"] = ev.agentType;
        item["tool_name"] = ev.toolName;
        item["tool_action"] = ev.toolAction;
        item["tool_input"] = ev.toolInput;
        item["content"] = ev.content;
        item["status"] = ev.status;
        item["step"] = ev.stepIndex;
        if (ev.totalTokens > 0) item["total_tokens"] = ev.totalTokens;
        if (ev.costUsd > 0.0) item["cost_usd"] = ev.costUsd;
        evArr.push_back(item);
    }
    root["events"] = evArr;
    return root.dump();
}

std::string LlmEventsToSummary(const std::vector<LlmStreamEvent>& events, const LlmAgentState& state) {
    std::ostringstream oss;
    oss << "[Agent: " << (state.agentType.empty() ? "LLM" : state.agentType)
        << " | Status: " << state.status;
    if (!state.currentTool.empty()) {
        oss << " | Tool: " << state.currentTool;
    }
    if (!state.supervisorGoal.empty()) {
        oss << " | Goal: " << state.supervisorGoal;
    }
    oss << "]\n";

    for (const auto& ev : events) {
        std::string timeStr = ev.timestamp;
        if (timeStr.size() >= 19) timeStr = timeStr.substr(11, 8); // Extract HH:MM:SS
        oss << "[" << (timeStr.empty() ? "--:--:--" : timeStr) << "] ";

        if (ev.type == "tool_call") {
            oss << "TOOL CALL: " << ev.toolName;
            if (!ev.toolAction.empty() && ev.toolAction != ev.toolName) {
                oss << " (" << ev.toolAction << ")";
            }
        } else if (ev.type == "tool_result") {
            oss << "TOOL RESULT: " << ev.toolName;
            if (!ev.content.empty()) {
                std::string preview = ev.content.substr(0, 80);
                std::replace(preview.begin(), preview.end(), '\n', ' ');
                oss << " -> " << preview;
            }
        } else if (ev.type == "thinking") {
            std::string preview = ev.content.substr(0, 100);
            std::replace(preview.begin(), preview.end(), '\n', ' ');
            oss << "THINKING: " << preview;
        } else if (ev.type == "message") {
            std::string preview = ev.content.substr(0, 120);
            std::replace(preview.begin(), preview.end(), '\n', ' ');
            oss << "MESSAGE: " << preview;
        } else if (ev.type == "error") {
            oss << "ERROR: " << ev.content;
        } else {
            oss << ev.type << ": " << ev.content;
        }
        oss << "\n";
    }
    return oss.str();
}

void LlmStreamTracker::Init(int processId, const std::string& agentType, const std::string& streamFilePath,
                            const std::string& supervisorNotes, const std::string& supervisorGoal) {
    std::lock_guard<std::mutex> lock(mutex_);
    processId_ = processId;
    agentType_ = agentType;
    streamFilePath_ = streamFilePath;
    streamFilePathWide_ = Utf8ToWide(streamFilePath);

    state_.agentType = agentType;
    state_.streamFilePath = streamFilePath;
    state_.supervisorNotes = supervisorNotes;
    state_.supervisorGoal = supervisorGoal;
    state_.updatedAt = GetCurrentTimestampStr();
}

void LlmStreamTracker::UpdateSupervisorInfo(const std::string& notes, const std::string& goal) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!notes.empty()) state_.supervisorNotes = notes;
    if (!goal.empty()) state_.supervisorGoal = goal;
    state_.updatedAt = GetCurrentTimestampStr();
}

void LlmStreamTracker::IngestChunk(const char* data, size_t len) {
    if (!data || len == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    lineBuffer_.append(data, len);

    size_t pos = 0;
    while ((pos = lineBuffer_.find('\n')) != std::string::npos) {
        std::string line = lineBuffer_.substr(0, pos);
        lineBuffer_.erase(0, pos + 1);
        ProcessLine(line);
    }

    // Safety guard against huge single-line buffer without newlines
    if (lineBuffer_.size() > 64 * 1024) {
        ProcessLine(lineBuffer_);
        lineBuffer_.clear();
    }
}

void LlmStreamTracker::ProcessLine(const std::string& line) {
    LlmStreamEvent ev;
    if (!ParseLlmStreamLine(line, agentType_, ev)) {
        return;
    }

    ev.seq = nextSeq_++;
    ev.timestamp = GetCurrentTimestampStr();
    if (ev.agentType.empty() || ev.agentType == "generic") {
        if (!agentType_.empty()) ev.agentType = agentType_;
    }

    // Save line to persistent stream file (.jsonl)
    if (!streamFilePathWide_.empty()) {
        std::string writeLine = line;
        if (writeLine.empty() || writeLine.back() != '\n') writeLine += "\r\n";
        AppendToLogFileWide(streamFilePathWide_, writeLine);
    }

    // Update agent state
    state_.eventCount = static_cast<int>(events_.size()) + 1;
    state_.updatedAt = ev.timestamp;

    if (!ev.agentType.empty() && ev.agentType != "generic") {
        state_.agentType = ev.agentType;
    }
    if (ev.stepIndex > 0) {
        state_.stepCount = ev.stepIndex;
    }

    if (ev.type == "tool_call") {
        state_.status = "calling_tool";
        state_.currentTool = ev.toolName;
        state_.currentToolAction = ev.toolAction;
    } else if (ev.type == "tool_result") {
        state_.status = "tool_finished";
    } else if (ev.type == "thinking") {
        state_.status = "thinking";
        if (!ev.content.empty()) {
            state_.lastThought = ev.content.substr(0, 160);
        }
    } else if (ev.type == "message") {
        state_.status = "generating";
        if (!ev.content.empty()) {
            state_.lastMessage = ev.content.substr(0, 160);
        }
    } else if (ev.type == "status") {
        if (!ev.status.empty()) state_.status = ev.status;
    } else if (ev.type == "error") {
        state_.status = "error";
        state_.lastError = ev.content;
    } else if (ev.type == "result") {
        state_.status = "completed";
    }

    events_.push_back(ev);
    while (events_.size() > kMaxEvents) {
        events_.pop_front();
    }
}

LlmAgentState LlmStreamTracker::GetState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::vector<LlmStreamEvent> LlmStreamTracker::GetEvents(int tail, const std::string& typeFilter, uint64_t sinceSeq) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LlmStreamEvent> out;
    out.reserve(events_.size());

    for (const auto& ev : events_) {
        if (ev.seq <= sinceSeq) continue;
        if (!typeFilter.empty() && ev.type != typeFilter) continue;
        out.push_back(ev);
    }

    if (tail > 0 && out.size() > static_cast<size_t>(tail)) {
        out.erase(out.begin(), out.begin() + (out.size() - tail));
    }
    return out;
}

std::string LlmStreamTracker::GetEventsJson(int tail, const std::string& typeFilter, uint64_t sinceSeq) const {
    auto evs = GetEvents(tail, typeFilter, sinceSeq);
    auto st = GetState();
    return LlmEventsToJson(evs, st);
}

std::string LlmStreamTracker::GetEventsSummary(int tail, const std::string& typeFilter) const {
    auto evs = GetEvents(tail, typeFilter, 0);
    auto st = GetState();
    return LlmEventsToSummary(evs, st);
}

std::string LlmStreamTracker::GetEventsRawJsonl(int tail) const {
    auto evs = GetEvents(tail, "", 0);
    std::string out;
    for (const auto& ev : evs) {
        out += ev.rawJson + "\n";
    }
    return out;
}

bool LlmStreamTracker::HasEvents() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !events_.empty();
}

uint64_t LlmStreamTracker::EventCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint64_t>(events_.size());
}

} // namespace pc
