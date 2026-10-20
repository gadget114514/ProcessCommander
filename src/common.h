#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace pc {

constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\ProcessCommander_IPC_v1";
constexpr const wchar_t* kMutexName = L"Local\\ProcessCommander_GUI_Mutex_v1";

enum class ExecMode {
    Pipe = 0,     // Anonymous pipes (non-interactive / standard CLI)
    ConPTY = 1,   // Windows Pseudo Console (interactive TUI: opencode, claude)
    Console = 2   // Separate visible console window (CREATE_NEW_CONSOLE)
};

enum class ProcState {
    Running = 0,
    Completed = 1,
    Failed = 2,
    TimedOut = 3,
    IdleTimedOut = 4,
    Killed = 5
};

std::wstring Utf8ToWide(const std::string& utf8);
std::string WideToUtf8(const std::wstring& wide);

std::string ExecModeToString(ExecMode mode);
ExecMode StringToExecMode(const std::string& s);
std::string ProcStateToString(ProcState state);
ProcState StringToProcState(const std::string& s);
std::wstring ProcStateToDisplayWide(ProcState state, bool isIdleWarning);

// Strip ANSI/VT100 escape codes from ConPTY output for clean display & LLM reading
std::string StripAnsiCodes(const std::string& input);

// Normalize line endings (\r\n) for Win32 Edit control display
std::wstring NormalizeCrLfWide(const std::string& utf8Clean);

// Extract last meaningful non-empty line for dashboard preview
std::string GetLastNonEmptyLine(const std::string& text, size_t maxLen = 120);

// Tail helper: return last N lines of text
std::string GetTailLines(const std::string& text, int maxLines);

// Simple JSON escaping and lightweight key-value extraction for IPC & MCP
std::string JsonEscape(const std::string& s);
std::string JsonGetString(const std::string& json, const std::string& key, const std::string& defaultVal = "");
int64_t JsonGetInt(const std::string& json, const std::string& key, int64_t defaultVal = 0);
bool JsonGetBool(const std::string& json, const std::string& key, bool defaultVal = false);

uint64_t GetNowTickMs();
std::string GetCurrentTimestampStr();
std::string GetFileTimestampStr();
std::string FormatDurationSec(uint64_t ms);
std::string FormatUptimeHMS(uint64_t ms);
std::string FormatBytes(uint64_t bytes);

// Persistent logging helpers (used by MCP, CLI, IPC, and ProcessManager)
std::wstring GetLogsDirectoryWide();
std::string GetLogsDirectoryUtf8();
std::string SanitizeFileNameComponent(const std::string& s);
void AppendToLogFileWide(const std::wstring& filePath, const std::string& data);
void WriteFileAtomicWide(const std::wstring& filePath, const std::string& data);
std::string ReadFileUtf8Wide(const std::wstring& filePath);
void WriteAuditLog(const std::string& category, const std::string& message);

} // namespace pc
