#include "ipc_client.h"
#include "common.h"
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")

namespace pc {

static bool TryConnectPipe(HANDLE& outPipe) {
    const std::wstring pipeName = GetPipeNameWide();
    while (true) {
        HANDLE hPipe = CreateFileW(
            pipeName.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );
        if (hPipe != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr);
            outPipe = hPipe;
            return true;
        }
        if (GetLastError() != ERROR_PIPE_BUSY) {
            return false;
        }
        if (!WaitNamedPipeW(pipeName.c_str(), 500)) {
            return false;
        }
    }
}

// Resolve the GUI daemon executable. pcder.exe (console edition) lives next to
// ProcessCommander.exe, so prefer the sibling GUI binary when present.
static std::wstring ResolveGuiExePath() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    std::wstring current = exePath;
    auto slash = current.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        std::wstring candidate = current.substr(0, slash + 1) + L"ProcessCommander.exe";
        if (_wcsicmp(candidate.c_str(), current.c_str()) == 0) {
            return current;
        }
        DWORD attr = GetFileAttributesW(candidate.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            return candidate;
        }
    }
    return current;
}

bool SendIpcRequest(const std::string& reqJson, std::string& outRespJson, std::string& outError, bool autoStartGui) {
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    if (!TryConnectPipe(hPipe)) {
        if (!autoStartGui) {
            outError = "ProcessCommander GUI daemon is not running.";
            return false;
        }
        // Auto-spawn GUI Dashboard process via WMI Win32_Process.Create so that it is
        // created by WmiPrvSE.exe completely outside the CLI caller's process tree,
        // Job Object, and stdout/stderr pipes (preventing Claude's Bash tool from blocking).
        std::wstring guiExePath = ResolveGuiExePath();

        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};

        std::wstring guiCmdLine = L"\"" + guiExePath + L"\" gui";
        if (HasCustomDataDirectory()) {
            guiCmdLine += L" --data-dir \"" + GetEffectiveDataDirectoryWide() + L"\"";
        }

        auto EscapeSingleQuotes = [](const std::wstring& in) {
            std::wstring outText = in;
            size_t pos = 0;
            while ((pos = outText.find(L'\'', pos)) != std::wstring::npos) {
                outText.replace(pos, 1, L"''");
                pos += 2;
            }
            return outText;
        };

        std::wstring psScript = L"Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{CommandLine='" + EscapeSingleQuotes(guiCmdLine) + L"'} | Out-Null";

        DWORD b64Len = 0;
        CryptBinaryToStringW((const BYTE*)psScript.data(), (DWORD)(psScript.size() * sizeof(wchar_t)), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &b64Len);
        std::wstring encodedCmd(b64Len, L'\0');
        CryptBinaryToStringW((const BYTE*)psScript.data(), (DWORD)(psScript.size() * sizeof(wchar_t)), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &encodedCmd[0], &b64Len);

        if (!encodedCmd.empty() && encodedCmd.back() == L'\0') {
            encodedCmd.pop_back();
        }

        wchar_t psPath[MAX_PATH] = {};
        ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
                                  psPath, MAX_PATH);

        std::wstring wmiCmd = psPath;
        wmiCmd += L" -NoLogo -NoProfile -NonInteractive -WindowStyle Hidden -EncodedCommand ";
        wmiCmd += encodedCmd;

        std::vector<wchar_t> wmiBuf(wmiCmd.begin(), wmiCmd.end());
        wmiBuf.push_back(L'\0');

        if (CreateProcessW(nullptr, wmiBuf.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 4000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }

        // Wait up to 4 seconds for GUI Named Pipe server to come online
        bool connected = false;
        for (int attempt = 0; attempt < 40; ++attempt) {
            Sleep(100);
            if (TryConnectPipe(hPipe)) {
                connected = true;
                break;
            }
        }
        if (!connected) {
            outError = "Failed to connect to ProcessCommander Named Pipe server after auto-starting GUI.";
            return false;
        }
    }

    DWORD written = 0;
    if (!WriteFile(hPipe, reqJson.data(), static_cast<DWORD>(reqJson.size()), &written, nullptr)) {
        CloseHandle(hPipe);
        outError = "WriteFile to Named Pipe failed.";
        return false;
    }

    outRespJson.clear();
    char buf[16384];
    DWORD bytesRead = 0;
    while (true) {
        BOOL ok = ReadFile(hPipe, buf, sizeof(buf), &bytesRead, nullptr);
        if (bytesRead > 0) {
            outRespJson.append(buf, bytesRead);
        }
        if (ok) break;
        if (GetLastError() != ERROR_MORE_DATA) break;
    }

    CloseHandle(hPipe);
    return !outRespJson.empty();
}

} // namespace pc
