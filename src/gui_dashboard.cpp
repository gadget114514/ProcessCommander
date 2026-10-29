#include "gui_dashboard.h"
#include "common.h"
#include "TerminalView.h"
#include "TerminalGridView.h"
#include "nlohmann/json.hpp"
#include <commctrl.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <unordered_map>
#include <ctime>
#include <cstdio>

// ---------------------------------------------------------------------------
// Globals required by src/Terminal/src/TerminalGridView.cpp
// ---------------------------------------------------------------------------
struct AppTabInfo {
    HWND hwnd = nullptr;
    std::wstring label;
    int type;
    void* data = nullptr;
    HANDLE hProcess = nullptr;
};
std::vector<AppTabInfo> g_appTabs;
int g_activeAppTab = 0;
HWND g_mainHwnd = nullptr;
HWND g_tabHwnd = nullptr;
bool g_suppressTabChange = false;
void UpdateMenu(HWND) {}
void UpdateTabs(HWND) {}

#define TAB_TYPE_TERMINAL 10
#define WM_DEFERRED_FOCUS (WM_USER + 207)

namespace pc {

constexpr UINT WM_PC_IPC_EVENT        = WM_APP + 101;
constexpr UINT WM_PC_CREATE_TERMVIEW  = WM_APP + 102;
constexpr UINT WM_PC_DESTROY_TERMVIEW = WM_APP + 103;
constexpr UINT WM_PC_TRAYICON         = WM_APP + 104;
constexpr UINT_PTR TIMER_REFRESH_ID   = 1;
constexpr UINT_PTR TIMER_KPI_ANIM_ID  = 2;
constexpr UINT TRAY_ICON_ID           = 1;
constexpr int  APP_ICON_RESOURCE_ID   = 1;

static HICON LoadAppIcon() {
    HICON hIcon = static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(APP_ICON_RESOURCE_ID),
        IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
    return hIcon ? hIcon : LoadIconW(nullptr, IDI_APPLICATION);
}

static HICON LoadAppSmallIcon() {
    HICON hIcon = static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(APP_ICON_RESOURCE_ID),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_SHARED));
    return hIcon ? hIcon : LoadIconW(nullptr, IDI_APPLICATION);
}

enum class UiLanguage { Japanese, English, Spanish };
enum class UiTheme { System, Dark, Light, Pastel, HighContrast };

struct ThemeColors {
    COLORREF windowBg;
    COLORREF panel;
    COLORREF border;
    COLORREF text;
    COLORREF muted;
    COLORREF accent;
    COLORREF terminalBg;
    COLORREF emptyBg;
    COLORREF listText;
    COLORREF rowSelected;
    COLORREF rowRunning;
    COLORREF rowIdle;
    COLORREF rowCompleted;
    COLORREF rowFailed;
};

static UiLanguage g_uiLanguage = UiLanguage::English;
static UiTheme g_uiTheme = UiTheme::System;
static ThemeColors g_theme = {};
static HBRUSH g_dialogBgBrush = nullptr;

static UiLanguage DetectDefaultLanguage() {
    wchar_t buf[16] = {};
    DWORD n = GetEnvironmentVariableW(L"PC_UI_LANG", buf, 16);
    if (n > 0) {
        if (_wcsnicmp(buf, L"ja", 2) == 0) return UiLanguage::Japanese;
        if (_wcsnicmp(buf, L"es", 2) == 0) return UiLanguage::Spanish;
        return UiLanguage::English;
    }
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
        case LANG_JAPANESE: return UiLanguage::Japanese;
        case LANG_SPANISH:  return UiLanguage::Spanish;
        default:            return UiLanguage::English;
    }
}

static bool IsSystemDarkMode() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &key) == ERROR_SUCCESS) {
        RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(&value), &size);
        RegCloseKey(key);
    }
    return value == 0;
}

static COLORREF GetSystemAccentColor() {
    DWORD color = 0;
    BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&color, &opaque))) {
        return RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
    }
    return GetSysColor(COLOR_HIGHLIGHT);
}

static ThemeColors ThemeFor(UiTheme theme) {
    switch (theme) {
        case UiTheme::System: {
            ThemeColors base = ThemeFor(IsSystemDarkMode() ? UiTheme::Dark : UiTheme::Light);
            base.accent = GetSystemAccentColor();
            return base;
        }
        case UiTheme::Light:
            return {
                RGB(241, 245, 249), RGB(255, 255, 255), RGB(203, 213, 225),
                RGB(15, 23, 42), RGB(71, 85, 105), RGB(2, 132, 199),
                RGB(15, 23, 42), RGB(226, 232, 240), RGB(15, 23, 42), RGB(229, 231, 235),
                RGB(220, 252, 231), RGB(254, 243, 199), RGB(219, 234, 254), RGB(254, 226, 226)
            };
        case UiTheme::Pastel:
            return {
                RGB(248, 244, 250), RGB(255, 251, 253), RGB(214, 204, 224),
                RGB(74, 66, 84), RGB(130, 118, 140), RGB(178, 148, 210),
                RGB(34, 30, 40), RGB(243, 236, 248), RGB(74, 66, 84), RGB(232, 230, 235),
                RGB(220, 244, 226), RGB(252, 238, 210), RGB(220, 235, 250), RGB(250, 220, 224)
            };
        case UiTheme::HighContrast:
            return {
                RGB(0, 0, 0), RGB(20, 20, 20), RGB(255, 255, 255),
                RGB(255, 255, 255), RGB(220, 220, 220), RGB(255, 255, 0),
                RGB(0, 0, 0), RGB(0, 0, 0), RGB(255, 255, 255), RGB(60, 60, 60),
                RGB(0, 40, 0), RGB(50, 40, 0), RGB(0, 20, 50), RGB(50, 0, 0)
            };
        case UiTheme::Dark:
        default:
            return {
                RGB(10, 10, 12), RGB(18, 18, 20), RGB(50, 50, 54),
                RGB(202, 202, 208), RGB(145, 145, 150), RGB(95, 125, 145),
                RGB(0, 0, 0), RGB(12, 12, 14), RGB(202, 202, 208), RGB(55, 55, 60),
                RGB(18, 30, 24), RGB(34, 30, 20), RGB(20, 28, 36), RGB(36, 22, 24)
            };
    }
}

static void ApplyTheme() {
    g_theme = ThemeFor(g_uiTheme);
    if (g_dialogBgBrush) {
        DeleteObject(g_dialogBgBrush);
    }
    g_dialogBgBrush = CreateSolidBrush(g_theme.panel);
}
static bool IsDarkThemeActive() {
    return g_uiTheme == UiTheme::Dark || g_uiTheme == UiTheme::HighContrast ||
           (g_uiTheme == UiTheme::System && IsSystemDarkMode());
}

using SetPreferredAppModeFn = int(WINAPI*)(int);
using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND, BOOL);

static void ApplyDarkModeApi(HWND hwnd, bool dark) {
    static bool initialized = false;
    static SetPreferredAppModeFn setPreferredAppMode = nullptr;
    static AllowDarkModeForWindowFn allowDarkModeForWindow = nullptr;
    if (!initialized) {
        HMODULE ux = LoadLibraryW(L"uxtheme.dll");
        if (ux) {
            setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(
                GetProcAddress(ux, MAKEINTRESOURCEA(135)));
            allowDarkModeForWindow = reinterpret_cast<AllowDarkModeForWindowFn>(
                GetProcAddress(ux, MAKEINTRESOURCEA(133)));
        }
        initialized = true;
    }
    if (setPreferredAppMode) setPreferredAppMode(dark ? 2 : 3);
    if (allowDarkModeForWindow && hwnd) allowDarkModeForWindow(hwnd, dark ? TRUE : FALSE);
}

static void ApplyNativeControlTheme(HWND hwnd, bool dark) {
    if (!hwnd) return;
    ApplyDarkModeApi(hwnd, dark);
    SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : L"", dark ? nullptr : L"");
}

static void ApplyNativeThemeToWindow(HWND hwnd) {
    if (!hwnd) return;
    bool dark = IsDarkThemeActive();
    BOOL darkFlag = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20, &darkFlag, sizeof(darkFlag));
    DwmSetWindowAttribute(hwnd, 19, &darkFlag, sizeof(darkFlag));
    ApplyNativeControlTheme(hwnd, dark);
    for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        ApplyNativeControlTheme(child, dark);
        if (GetWindow(child, GW_CHILD)) {
            for (HWND grand = GetWindow(child, GW_CHILD); grand; grand = GetWindow(grand, GW_HWNDNEXT)) {
                ApplyNativeControlTheme(grand, dark);
            }
        }
    }
}

static void DrawHeaderItem(DRAWITEMSTRUCT* dis) {
    if (!dis || !dis->hwndItem) return;
    wchar_t text[256] = {};
    HDITEMW hdiText = {};
    hdiText.mask = HDI_TEXT;
    hdiText.pszText = text;
    hdiText.cchTextMax = 256;
    SendMessageW(dis->hwndItem, HDM_GETITEMW, dis->itemID, reinterpret_cast<LPARAM>(&hdiText));
    HBRUSH bgBrush = CreateSolidBrush(g_theme.windowBg);
    FillRect(dis->hDC, &dis->rcItem, bgBrush);
    DeleteObject(bgBrush);

    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, g_theme.text);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(GetParent(dis->hwndItem), WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = SelectObject(dis->hDC, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    RECT rc = dis->rcItem;
    InflateRect(&rc, -8, -1);
    DrawTextW(dis->hDC, text, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dis->hDC, oldFont);

    HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
    HGDIOBJ oldPen = SelectObject(dis->hDC, borderPen);
    MoveToEx(dis->hDC, dis->rcItem.right - 1, dis->rcItem.top, nullptr);
    LineTo(dis->hDC, dis->rcItem.right - 1, dis->rcItem.bottom);
    MoveToEx(dis->hDC, dis->rcItem.left, dis->rcItem.bottom - 1, nullptr);
    LineTo(dis->hDC, dis->rcItem.right, dis->rcItem.bottom - 1);
    SelectObject(dis->hDC, oldPen);
    DeleteObject(borderPen);
}

static LRESULT CALLBACK HeaderSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                           UINT_PTR, DWORD_PTR) {
    if (msg == WM_ERASEBKGND) {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc = {};
        GetClientRect(hwnd, &rc);
        HBRUSH brush = CreateSolidBrush(g_theme.windowBg);
        FillRect(hdc, &rc, brush);
        DeleteObject(brush);
        return 1;
    }
    LRESULT res = DefSubclassProc(hwnd, msg, wParam, lParam);
    if (msg == WM_PAINT) {
        int count = Header_GetItemCount(hwnd);
        if (count > 0) {
            RECT rcLast = {};
            if (Header_GetItemRect(hwnd, count - 1, &rcLast)) {
                RECT rcClient = {};
                GetClientRect(hwnd, &rcClient);
                if (rcLast.right < rcClient.right) {
                    HDC hdc = GetDC(hwnd);
                    RECT rcEmpty = { rcLast.right, rcClient.top, rcClient.right, rcClient.bottom };
                    HBRUSH brush = CreateSolidBrush(g_theme.windowBg);
                    FillRect(hdc, &rcEmpty, brush);
                    DeleteObject(brush);
                    HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
                    HGDIOBJ oldPen = SelectObject(hdc, borderPen);
                    MoveToEx(hdc, rcEmpty.left, rcEmpty.bottom - 1, nullptr);
                    LineTo(hdc, rcEmpty.right, rcEmpty.bottom - 1);
                    SelectObject(hdc, oldPen);
                    DeleteObject(borderPen);
                    ReleaseDC(hwnd, hdc);
                }
            }
        }
    }
    return res;
}

static LRESULT CALLBACK ListViewSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                             UINT_PTR, DWORD_PTR) {
    if (msg == WM_DRAWITEM) {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (dis && dis->CtlType == ODT_HEADER) {
            DrawHeaderItem(dis);
            return TRUE;
        }
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK TabSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR, DWORD_PTR) {
    if (msg == WM_ERASEBKGND) {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc = {};
        GetClientRect(hwnd, &rc);
        HBRUSH brush = CreateSolidBrush(g_theme.windowBg);
        FillRect(hdc, &rc, brush);
        DeleteObject(brush);
        return 1;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static COLORREF BlendColor(COLORREF from, COLORREF to, float t) {
    auto mix = [t](int a, int b) { return static_cast<int>(a + (b - a) * t + 0.5f); };
    return RGB(mix(GetRValue(from), GetRValue(to)),
               mix(GetGValue(from), GetGValue(to)),
               mix(GetBValue(from), GetBValue(to)));
}

// Low-saturation variant of a theme row color (keeps brightness, mutes the hue).
static COLORREF MutedRowColor(COLORREF c) {
    int r = GetRValue(c);
    int g = GetGValue(c);
    int b = GetBValue(c);
    int gray = (r * 299 + g * 587 + b * 114) / 1000;
    return BlendColor(c, RGB(gray, gray, gray), 0.6f);
}


static const wchar_t* Tr(const wchar_t* ja, const wchar_t* en, const wchar_t* es = nullptr) {
    if (g_uiLanguage == UiLanguage::Spanish) return es ? es : en;
    if (g_uiLanguage == UiLanguage::English) return en;
    return ja;
}

static const char* LanguageCode(UiLanguage lang) {
    switch (lang) {
        case UiLanguage::Japanese: return "ja";
        case UiLanguage::Spanish:  return "es";
        default:                   return "en";
    }
}

static UiLanguage ParseLanguage(const std::string& code) {
    if (code == "ja") return UiLanguage::Japanese;
    if (code == "es") return UiLanguage::Spanish;
    if (code == "en") return UiLanguage::English;
    return DetectDefaultLanguage();
}

static const char* ThemeCode(UiTheme theme) {
    switch (theme) {
        case UiTheme::Dark:         return "dark";
        case UiTheme::Light:        return "light";
        case UiTheme::Pastel:       return "pastel";
        case UiTheme::HighContrast: return "high_contrast";
        default:                    return "system";
    }
}
static UiTheme ParseTheme(const std::string& code) {
    if (code == "dark") return UiTheme::Dark;
    if (code == "light") return UiTheme::Light;
    if (code == "pastel") return UiTheme::Pastel;
    if (code == "high_contrast") return UiTheme::HighContrast;
    return UiTheme::System;
}

// Compact dashboard header / splitter layout constants
constexpr int kHeaderKpiTop        = 6;
constexpr int kHeaderKpiRow1Bottom = 32;
constexpr int kHeaderKpiRow2Top    = 36;
constexpr int kHeaderKpiBottom     = 62;
constexpr int kToolbarTop          = 68;
constexpr int kToolbarH            = 24;
constexpr int kListTop             = 98;
constexpr int kSplitterH           = 6;
constexpr int kRowSplitterH        = 6;
constexpr int kMinRowListH         = 64;
constexpr int kTabH                = 26;
constexpr int kStatusBarH          = 26;
constexpr int kMinListH            = 60;
constexpr int kMinTerminalH        = 200;
constexpr int kNumKpiCards         = 14;
constexpr int kUsagePanelH         = 110;

enum ControlId {
    IDC_BTN_START = 1005,
    IDC_BTN_KILL_SEL = 1006,
    IDC_BTN_KILL_ALL = 1007,
    IDC_BTN_CLEAR = 1008,
    IDC_LIST_PROCS = 1009,
    IDC_BTN_GRID_VIEW = 1015,
    IDC_TAB_TERMINALS = 1016,
    IDC_BTN_HIDE_TRAY = 1017,
    IDC_BTN_HIDE_ENDED = 1019,
    IDC_BTN_TOGGLE_LIST = 1020,
    IDC_BTN_TOGGLE_TERM = 1021,
    IDC_BTN_SETTINGS = 1023,
    IDC_CLOCK = 1024,
    IDC_BTN_VIEW_TASKS = 1025,
    IDC_LIST_TASKS = 1026,
    IDC_BTN_USAGE = 1027,
    IDC_SET_LANG_JA = 6001,
    IDC_SET_LANG_EN = 6002,
    IDC_SET_LANG_ES = 6003,
    IDC_SET_THEME_SYSTEM = 6010,
    IDC_SET_THEME_DARK = 6011,
    IDC_SET_THEME_LIGHT = 6012,
    IDC_SET_THEME_HC = 6013,
    IDC_SET_THEME_PASTEL = 6014,
    IDC_SET_SHELL_CONFIG = 6022,
    IDC_SET_RESET_IDS = 6021,
    IDC_SET_CLEAR_ENDED = 6023,
    IDC_SET_HIDE_ENDED = 6024,
    IDC_SET_SOUND_ENABLED = 6030,
    IDC_SET_SOUND_PATH = 6031,
    IDC_SET_SOUND_BROWSE = 6032,
    IDC_SET_SOUND_TEST = 6033,
    IDM_TRAY_RESTORE = 4001,
    IDM_TRAY_GRID_VIEW = 4002,
    IDM_TRAY_HIDE = 4003,
    IDM_TRAY_EXIT = 4004,
    IDM_TRAY_USAGE = 4005
};

enum class ShellType {
    Cmd = 0,
    PowerShell = 1,
    Bash = 2
};

struct CreateTermViewParams {
    std::wstring resolvedCmd;
    std::wstring cwd;
    std::function<void(const char*, size_t)> onOutput;
    TerminalView* outView = nullptr;
    std::wstring outError;
};

struct TermSlotBanner {
    RECT rcBanner;
    RECT rcBorder;
    int procId;
};

enum class ListDisplayMode {
    TwoRows = 0,     // Default: top is Process List, bottom is Task Queue List
    ProcessOnly = 1, // Only Process List
    TaskOnly = 2     // Only Task Queue List
};

struct DashboardState {
    ProcessManager* pm = nullptr;
    IpcServer* ipc = nullptr;
    TaskQueueManager* tqm = nullptr;
    DWORD guiThreadId = 0;

    HWND hwndMain = nullptr;
    HWND hwndBtnStart = nullptr;
    HWND hwndBtnSettings = nullptr;
    HWND hwndBtnKillSel = nullptr;
    HWND hwndBtnKillAll = nullptr;
    HWND hwndBtnClear = nullptr;
    HWND hwndBtnGridView = nullptr;
    HWND hwndBtnHideTray = nullptr;
    HWND hwndBtnToggleList = nullptr;
    HWND hwndBtnToggleTerm = nullptr;
    HWND hwndBtnUsage = nullptr;
    HWND hwndUsagePanel = nullptr;
    bool showUsagePanel = true;
    int usageRangeIndex = 1;
    RECT rcUsagePanel = {};
    HWND hwndList = nullptr;
    HWND hwndTaskList = nullptr;
    HWND hwndBtnViewTasks = nullptr;
    ListDisplayMode listDisplayMode = ListDisplayMode::TwoRows;
    float procRowRatio = 0.5f;
    RECT rcRowsSplitter = {};
    bool rowsSplitterDragging = false;
    int lastTotalListHeight = 0;
    std::vector<std::string> taskJobIds;
    std::vector<std::string> taskJobStates;
    HWND hwndTab = nullptr;
    HWND hwndClock = nullptr;
    HWND hwndKpiTip = nullptr;
    HWND hwndToolbarTip = nullptr;
    RECT kpiCardRects[kNumKpiCards] = {};
    std::wstring kpiTipText[kNumKpiCards];
    int activeRunningCount = 0;

    bool hideProcessList = false;
    bool hideTerminalGrid = true;

    ShellType defaultShell = ShellType::PowerShell;
    std::string cmdCommand = "cmd.exe";
    std::string powershellCommand = "powershell.exe";
    std::string bashCommand = "bash.exe";

    bool hideEndedProcesses = false;
    int sortColumn = 0;        // Default: Column 0 (ID)
    bool sortAscending = false; // Default: Descending (newest processes at the top)

    bool completionSoundEnabled = true;
    std::string completionSoundPath;
    std::unordered_map<int, ProcState> lastProcStates;

    std::unique_ptr<TerminalGridView> gridView;

    HFONT hFontUi = nullptr;
    HFONT hFontUiBold = nullptr;
    HFONT hFontKpiVal = nullptr;

    int selectedProcId = -1;
    bool allowExit = false;
    NOTIFYICONDATAW nid = {};
    bool trayAdded = false;

    std::string dashboardStartTimeStr;
    uint64_t dashboardStartTickMs = 0;
    int termProgressState = 0;
    int termProgressValue = 0;

    std::vector<ProcessSnapshot> allSnapshots;
    std::vector<ProcessSnapshot> cachedSnapshots;
    RECT rcKpiArea = {};
    RECT rcTerminalArea = {10, 300, 1380, 810};
    RECT rcStatusBar = {0, 814, 1400, 840};
    RECT rcSplitter = {};
    int processListHeight = 260;
    int processListTop = kListTop;
    bool splitterDragging = false;
    std::vector<TermSlotBanner> termBanners;
};

static DashboardState* GetState(HWND hwnd) {
    return reinterpret_cast<DashboardState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

static std::wstring StateLabelWide(ProcState state, bool idleWarning) {
    switch (state) {
        case ProcState::Running:
            return idleWarning ? Tr(L"アイドル / 待機", L"IDLE / WAITING", L"INACTIVO / ESPERANDO")
                               : Tr(L"稼働中", L"RUNNING", L"EN EJECUCIÓN");
        case ProcState::Completed:    return Tr(L"完了 (OK)", L"COMPLETED (OK)", L"COMPLETADO (OK)");
        case ProcState::Failed:       return Tr(L"失敗", L"FAILED", L"FALLIDO");
        case ProcState::TimedOut:     return Tr(L"タイムアウト", L"TIMEOUT", L"TIEMPO AGOTADO");
        case ProcState::IdleTimedOut: return Tr(L"ハングタイムアウト", L"HANG TIMEOUT", L"TIEMPO DE INACTIVIDAD");
        case ProcState::Killed:       return Tr(L"強制終了", L"KILLED", L"TERMINADO");
    }
    return Tr(L"不明", L"UNKNOWN", L"DESCONOCIDO");
}

static std::wstring FormatRemainingTime(int64_t remainingSec) {
    if (remainingSec <= 0) {
        return Tr(L"タイムアウト直前", L"expiring now", L"a punto de expirar");
    }
    int64_t hrs = remainingSec / 3600;
    int64_t mins = (remainingSec % 3600) / 60;
    int64_t secs = remainingSec % 60;
    wchar_t buf[128] = {};
    if (hrs > 0) {
        swprintf_s(buf, Tr(L"残り %lld時間%lld分", L"%lldh %lldm left", L"Quedan %lldh %lldm"), hrs, mins);
    } else if (mins > 0) {
        swprintf_s(buf, Tr(L"残り %lld分%lld秒", L"%lldm %llds left", L"Quedan %lldm %llds"), mins, secs);
    } else {
        swprintf_s(buf, Tr(L"残り %lld秒", L"%llds left", L"Quedan %llds"), secs);
    }
    return buf;
}

static std::wstring FormatRemainingTimeShort(int64_t remainingSec) {
    if (remainingSec <= 0) {
        return L"0s";
    }
    int64_t hrs = remainingSec / 3600;
    int64_t mins = (remainingSec % 3600) / 60;
    int64_t secs = remainingSec % 60;
    wchar_t buf[64] = {};
    if (hrs > 0) {
        swprintf_s(buf, L"~%lldh%lldm", hrs, mins);
    } else if (mins > 0) {
        swprintf_s(buf, L"~%lldm", mins);
    } else {
        swprintf_s(buf, L"~%llds", secs);
    }
    return buf;
}

static COLORREF GetStateBadgeColor(const ProcessSnapshot& s) {
    if (g_uiTheme == UiTheme::HighContrast) {
        switch (s.state) {
            case ProcState::Running:
                return (s.idleMs >= 10000) ? RGB(255, 255, 0) : RGB(0, 255, 0);
            case ProcState::Completed:
                return RGB(0, 200, 255);
            case ProcState::Failed:
            case ProcState::TimedOut:
            case ProcState::IdleTimedOut:
            case ProcState::Killed:
                return RGB(255, 0, 0);
        }
        return RGB(255, 255, 255);
    }
    if (g_uiTheme == UiTheme::Pastel) {
        switch (s.state) {
            case ProcState::Running:
                return (s.idleMs >= 10000) ? RGB(190, 150, 100) : RGB(110, 160, 130);
            case ProcState::Completed:
                return RGB(110, 140, 190);
            case ProcState::Failed:
            case ProcState::TimedOut:
            case ProcState::IdleTimedOut:
            case ProcState::Killed:
                return RGB(190, 110, 120);
        }
        return RGB(140, 130, 150);
    }
    if (IsDarkThemeActive()) {
        switch (s.state) {
            case ProcState::Running:
                return (s.idleMs >= 10000) ? RGB(118, 98, 64) : RGB(68, 100, 82);
            case ProcState::Completed:
                return RGB(68, 96, 124);
            case ProcState::Failed:
            case ProcState::TimedOut:
            case ProcState::IdleTimedOut:
            case ProcState::Killed:
                return RGB(122, 74, 74);
        }
        return RGB(98, 98, 104);
    }
    switch (s.state) {
        case ProcState::Running:
            return (s.idleMs >= 10000) ? RGB(217, 119, 6) : RGB(22, 163, 74);
        case ProcState::Completed:
            return RGB(37, 99, 235);
        case ProcState::Failed:
        case ProcState::TimedOut:
        case ProcState::IdleTimedOut:
        case ProcState::Killed:
            return RGB(220, 38, 38);
    }
    return RGB(100, 116, 139);
}

static COLORREF GetStateRowBgColor(const ProcessSnapshot& s) {
    switch (s.state) {
        case ProcState::Running:
            return (s.idleMs >= 10000) ? g_theme.rowIdle : g_theme.rowRunning;
        case ProcState::Completed:
            return g_theme.rowCompleted;
        case ProcState::Failed:
        case ProcState::TimedOut:
        case ProcState::IdleTimedOut:
        case ProcState::Killed:
            return g_theme.rowFailed;
    }
    return g_theme.panel;
}

static COLORREF KpiAccent(int index) {
    static const COLORREF darkAccents[7] = {
        RGB(125, 128, 134), RGB(78, 112, 92), RGB(120, 104, 74),
        RGB(78, 104, 128), RGB(124, 84, 84), RGB(104, 94, 118),
        RGB(140, 100, 60)
    };
    static const COLORREF lightAccents[7] = {
        RGB(100, 116, 139), RGB(22, 163, 74), RGB(217, 119, 6),
        RGB(37, 99, 235), RGB(220, 38, 38), RGB(168, 85, 247),
        RGB(234, 88, 12)
    };
    static const COLORREF pastelAccents[7] = {
        RGB(166, 180, 198), RGB(154, 198, 168), RGB(222, 190, 150),
        RGB(156, 186, 220), RGB(222, 160, 168), RGB(190, 166, 210),
        RGB(226, 180, 140)
    };
    static const COLORREF hcAccents[7] = {
        RGB(255, 255, 255), RGB(0, 255, 0), RGB(255, 255, 0),
        RGB(0, 200, 255), RGB(255, 0, 0), RGB(255, 0, 255),
        RGB(255, 128, 0)
    };
    int idx = (index >= 0) ? (index % 7) : ((index % 7 + 7) % 7);
    if (g_uiTheme == UiTheme::HighContrast) return hcAccents[idx];
    if (g_uiTheme == UiTheme::Pastel) return pastelAccents[idx];
    return IsDarkThemeActive() ? darkAccents[idx] : lightAccents[idx];
}

static float g_kpiBarPhase = 0.0f;

static void DrawKpiCard(HDC hdc, const RECT& rc, const wchar_t* title, const std::wstring& value,
                        COLORREF accentColor, HFONT hFontUi, HFONT hFontKpiVal,
                        float marqueePhase = -1.0f, const std::wstring& subText = L"") {
    HBRUSH bgBrush = CreateSolidBrush(g_theme.panel);
    HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
    HGDIOBJ oldBrush = SelectObject(hdc, bgBrush);
    HGDIOBJ oldPen = SelectObject(hdc, borderPen);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 6, 6);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(bgBrush);
    DeleteObject(borderPen);

    RECT rcStripe = {rc.left + 3, rc.top + 1, rc.right - 3, rc.top + 3};
    HBRUSH stripeBrush = CreateSolidBrush(accentColor);
    FillRect(hdc, &rcStripe, stripeBrush);
    DeleteObject(stripeBrush);

    if (marqueePhase >= 0.0f) {
        int stripeLeft = static_cast<int>(rcStripe.left);
        int stripeRight = static_cast<int>(rcStripe.right);
        int stripeW = stripeRight - stripeLeft;
        int bandW = (std::max)(14, stripeW / 3);
        int travel = stripeW + bandW;
        int bandLeft = stripeLeft - bandW + static_cast<int>(marqueePhase * travel);
        RECT rcBand = {(std::max)(stripeLeft, bandLeft), rcStripe.top,
                       (std::min)(stripeRight, bandLeft + bandW), rcStripe.bottom};
        if (rcBand.right > rcBand.left) {
            HBRUSH bandBrush = CreateSolidBrush(BlendColor(accentColor, RGB(255, 255, 255), 0.55f));
            FillRect(hdc, &rcBand, bandBrush);
            DeleteObject(bandBrush);
        }
    }

    SetBkMode(hdc, TRANSPARENT);

    // Measure value text width with hFontKpiVal
    HGDIOBJ oldFont = SelectObject(hdc, hFontKpiVal);
    SIZE szVal = {};
    GetTextExtentPoint32W(hdc, value.c_str(), static_cast<int>(value.size()), &szVal);

    // Measure subText if provided
    SIZE szSub = {};
    bool showSub = false;
    if (!subText.empty()) {
        SelectObject(hdc, hFontUi);
        GetTextExtentPoint32W(hdc, subText.c_str(), static_cast<int>(subText.size()), &szSub);
        int cardW = rc.right - rc.left;
        if (cardW - 16 - szVal.cx - szSub.cx >= 45) {
            showSub = true;
        }
    }

    int padX = 7;
    int rightEdge = rc.right - padX;

    // Draw Value on the right (bold, g_theme.text)
    RECT rcVal = {rightEdge - szVal.cx, rc.top + 2, rightEdge, rc.bottom - 1};
    SetTextColor(hdc, g_theme.text);
    SelectObject(hdc, hFontKpiVal);
    DrawTextW(hdc, value.c_str(), -1, &rcVal, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    int titleRight = rcVal.left - 4;

    // Draw subText to the left of value if space permits
    if (showSub) {
        RECT rcSub = {rcVal.left - szSub.cx - 4, rc.top + 2, rcVal.left - 4, rc.bottom - 1};
        SetTextColor(hdc, g_theme.muted);
        SelectObject(hdc, hFontUi);
        DrawTextW(hdc, subText.c_str(), -1, &rcSub, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        titleRight = rcSub.left - 4;
    }

    // Draw Title on the left (g_theme.muted, hFontUi, single line with ellipsis)
    RECT rcTitle = {rc.left + padX, rc.top + 2, (std::max)(rc.left + padX + 10, static_cast<LONG>(titleRight)), rc.bottom - 1};
    SetTextColor(hdc, g_theme.muted);
    SelectObject(hdc, hFontUi);
    DrawTextW(hdc, title, -1, &rcTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(hdc, oldFont);
}

static void UpdateKpiTooltips(DashboardState* st, uint32_t totalTreeProcs,
                              int activeRunning, int idleWaiting, int completedOk, int failedOrKilled,
                              const TaskQueueCounts& tqCounts,
                              int64_t procMinRemainSec = -1, int64_t taskMinRemainSec = -1) {
    if (!st || !st->hwndKpiTip) return;

    // Row 0: Processes (Cards 0..6)
    st->kpiTipText[0] = Tr(
        L"管理対象のターミナル/プロセスの総数。",
        L"Total number of managed terminals/processes.",
        L"Total de terminales/procesos gestionados.");
    if (totalTreeProcs > 0) {
        st->kpiTipText[0] += std::wstring(L"\n") + Tr(L"全ツリー子プロセス数: ", L"Total tree child processes: ", L"Subprocesos totales: ") + std::to_wstring(totalTreeProcs);
    }

    st->kpiTipText[1] = std::wstring(Tr(
        L"出力またはCPU消費がある実行中プロセス。",
        L"Running processes producing output or consuming CPU.",
        L"Procesos en ejecución con actividad.")) + L" (" + std::to_wstring(activeRunning) + L")";

    st->kpiTipText[2] = std::wstring(Tr(
        L"10秒以上無活動の実行中プロセス (アイドル)。",
        L"Running processes with no output or CPU activity >= 10s (idle).",
        L"Procesos en ejecución sin actividad >= 10 s (inactivos).")) + L" (" + std::to_wstring(idleWaiting) + L")";

    st->kpiTipText[3] = std::wstring(Tr(
        L"正常終了したプロセス (終了コード 0)。",
        L"Processes that exited successfully (exit code 0).",
        L"Procesos finalizados correctamente (código 0).")) + L" (" + std::to_wstring(completedOk) + L")";

    st->kpiTipText[4] = std::wstring(Tr(
        L"失敗・タイムアウト・強制終了したプロセス。",
        L"Processes that failed, timed out, or were killed.",
        L"Procesos fallidos, agotados o terminados.")) + L" (" + std::to_wstring(failedOrKilled) + L")";

    st->kpiTipText[5] = Tr(
        L"全 Job Object の合計メモリ消費量とプロセス数。",
        L"Total memory and process count across all Job Objects.",
        L"Memoria total y recuento de procesos de todos los Job Objects.");

    st->kpiTipText[6] = Tr(
        L"稼働中プロセスの中で最も近いタイムアウトまでの残り時間。",
        L"Shortest remaining time before timeout among running processes.",
        L"Tiempo restante más corto antes del tiempo límite entre procesos activos.");
    if (procMinRemainSec >= 0) {
        st->kpiTipText[6] += std::wstring(L"\n") + Tr(L"残り時間: ", L"Remaining: ", L"Restante: ") + FormatRemainingTime(procMinRemainSec);
    }

    // Row 1: Tasks (Cards 7..13)
    st->kpiTipText[7] = Tr(
        L"タスクキューに登録されている全ジョブの総数。",
        L"Total number of jobs in the task queue.",
        L"Total de tareas en la cola de tareas.");

    st->kpiTipText[8] = Tr(
        L"現在実行中のタスク数。",
        L"Number of tasks currently running.",
        L"Número de tareas en ejecución actualmente.");

    std::wstring tqWaitTip = std::to_wstring(tqCounts.waiting);
    if (tqCounts.pending > 0) {
        tqWaitTip += std::wstring(L" (+") + std::to_wstring(tqCounts.pending) +
            Tr(L" 依存関係保留)", L" pending for deps)", L" pendientes por dependencias)");
    }
    st->kpiTipText[9] = std::wstring(Tr(
        L"依存関係が満たされ実行準備が整っているタスク。",
        L"Tasks ready to run with all dependencies satisfied.",
        L"Tareas listas para ejecutarse con dependencias cumplidas.")) + L" (" + tqWaitTip + L")";

    st->kpiTipText[10] = Tr(
        L"正常に成功・完了したタスク。",
        L"Tasks that finished successfully.",
        L"Tareas finalizadas con éxito.");

    st->kpiTipText[11] = Tr(
        L"試行回数上限またはタイムアウト等で失敗したタスク、またはキャンセルされたタスク。",
        L"Tasks that failed after max attempts/timeout, or were cancelled.",
        L"Tareas fallidas tras agotar intentos/tiempo límite o canceladas.");

    st->kpiTipText[12] = Tr(
        L"未完了の先行タスク待ち (依存保留)、または先行タスク失敗によりブロックされたタスク。",
        L"Tasks waiting for unfinished dependencies, or blocked by upstream failure.",
        L"Tareas en espera de dependencias o bloqueadas por fallo aguas arriba.");

    st->kpiTipText[13] = Tr(
        L"未完了タスク（実行中・待機・ブロック）の残り時間の合計。実行中は残りタイムアウト、未起動はタイムアウト値で見積もります。",
        L"Total remaining time across unfinished tasks (running, waiting, blocked). Running tasks use the remaining timeout; not-yet-started tasks use their timeout.",
        L"Tiempo restante total de las tareas sin terminar (en ejecución, en espera, bloqueadas). Las tareas activas usan el tiempo límite restante; las no iniciadas usan su tiempo límite.");
    if (tqCounts.sumRemainingSec >= 0) {
        st->kpiTipText[13] += std::wstring(L"\n") + Tr(L"合計残り時間: ", L"Total remaining: ", L"Restante total: ") + FormatRemainingTime(tqCounts.sumRemainingSec);
    }
    if (taskMinRemainSec >= 0) {
        st->kpiTipText[13] += std::wstring(L"\n") + Tr(L"最短: ", L"Shortest: ", L"Más corto: ") + FormatRemainingTime(taskMinRemainSec);
    }

    for (int i = 0; i < kNumKpiCards; ++i) {
        TOOLINFOW ti = {};
        ti.cbSize = sizeof(ti);
        ti.hwnd = st->hwndMain;
        ti.uId = static_cast<UINT_PTR>(i + 1);
        ti.rect = st->kpiCardRects[i];
        ti.lpszText = const_cast<wchar_t*>(st->kpiTipText[i].c_str());
        SendMessageW(st->hwndKpiTip, TTM_NEWTOOLRECTW, ti.uId, reinterpret_cast<LPARAM>(&ti.rect));
        SendMessageW(st->hwndKpiTip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
    }
}

// Position the active tab's TerminalView Direct2D child window inside rcTerminalArea
// and synchronize the WC_TABCONTROLW tab bar (st->hwndTab / g_tabHwnd).
static void LayoutTerminalViews(DashboardState* st) {
    if (!st) return;

    g_appTabs.clear();
    std::vector<ProcessSnapshot> termProcs;
    int activeTabIdx = -1;
    bool foundSelected = false;

    for (const auto& s : st->cachedSnapshots) {
        if (s.terminalHwnd && IsWindow(s.terminalHwnd)) {
            AppTabInfo tab;
            tab.hwnd = s.terminalHwnd;
            std::wstring stateShort = (s.state == ProcState::Running) ? L"RUN" : L"END";
            tab.label = L"#" + std::to_wstring(s.id) + L" " + Utf8ToWide(s.name) + L" [" + stateShort + L"]";
            tab.type = TAB_TYPE_TERMINAL;
            tab.data = reinterpret_cast<void*>(static_cast<intptr_t>(s.id));
            g_appTabs.push_back(tab);

            if (s.id == st->selectedProcId) {
                activeTabIdx = static_cast<int>(g_appTabs.size()) - 1;
                foundSelected = true;
            }
            termProcs.push_back(s);
        }
    }

    if (!g_appTabs.empty()) {
        if (foundSelected) {
            g_activeAppTab = activeTabIdx;
        } else if (st->selectedProcId <= 0) {
            g_activeAppTab = 0;
            st->selectedProcId = static_cast<int>(reinterpret_cast<intptr_t>(g_appTabs[g_activeAppTab].data));
        } else if (g_activeAppTab < 0 || g_activeAppTab >= static_cast<int>(g_appTabs.size())) {
            // Keep the current terminal when the selected process has no TerminalView.
            g_activeAppTab = 0;
        }
    } else {
        g_activeAppTab = 0;
    }

    // Synchronize Win32 Tab Control (st->hwndTab)
    if (st->hwndTab && IsWindow(st->hwndTab)) {
        g_suppressTabChange = true;
        int curTabCount = TabCtrl_GetItemCount(st->hwndTab);
        int targetTabCount = static_cast<int>(g_appTabs.size());

        while (curTabCount > targetTabCount) {
            TabCtrl_DeleteItem(st->hwndTab, curTabCount - 1);
            curTabCount--;
        }
        for (int i = 0; i < targetTabCount; ++i) {
            std::wstring lbl = g_appTabs[i].label;
            TCITEMW tie = {};
            tie.mask = TCIF_TEXT | TCIF_PARAM;
            tie.pszText = lbl.data();
            tie.lParam = reinterpret_cast<LPARAM>(g_appTabs[i].data);
            if (i >= curTabCount) {
                TabCtrl_InsertItem(st->hwndTab, i, &tie);
            } else {
                wchar_t existingText[256] = {};
                TCITEMW curItem = {};
                curItem.mask = TCIF_TEXT;
                curItem.pszText = existingText;
                curItem.cchTextMax = 255;
                TabCtrl_GetItem(st->hwndTab, i, &curItem);
                if (lbl != existingText) {
                    TabCtrl_SetItem(st->hwndTab, i, &tie);
                }
            }
        }
        if (targetTabCount > 0 && TabCtrl_GetCurSel(st->hwndTab) != g_activeAppTab) {
            TabCtrl_SetCurSel(st->hwndTab, g_activeAppTab);
        }
        g_suppressTabChange = false;
    }

    st->termBanners.clear();

    if (st->hideTerminalGrid) {
        for (const auto& s : st->cachedSnapshots) {
            if (s.terminalHwnd && IsWindow(s.terminalHwnd) && IsWindowVisible(s.terminalHwnd)) {
                ShowWindow(s.terminalHwnd, SW_HIDE);
            }
        }
        return;
    }

    if (termProcs.empty()) return;

    const ProcessSnapshot& activeSnap = termProcs[g_activeAppTab];

    // Hide all other TerminalView child windows; show only the active tab's TerminalView
    for (const auto& s : termProcs) {
        if (s.id != activeSnap.id && IsWindowVisible(s.terminalHwnd)) {
            ShowWindow(s.terminalHwnd, SW_HIDE);
        }
    }

    RECT area = st->rcTerminalArea;
    int headerH = 26;
    RECT rcCell = area;
    RECT rcBanner = {rcCell.left, rcCell.top, rcCell.right, rcCell.top + headerH};
    st->termBanners.push_back({rcBanner, rcCell, activeSnap.id});

    RECT rcTv = {rcCell.left + 2, rcCell.top + headerH, rcCell.right - 2, rcCell.bottom - 2};
    int tvW = (std::max)(200L, rcTv.right - rcTv.left);
    int tvH = (std::max)(120L, rcTv.bottom - rcTv.top);

    HWND hTv = activeSnap.terminalHwnd;
    RECT rcCur;
    GetWindowRect(hTv, &rcCur);
    MapWindowPoints(nullptr, st->hwndMain, reinterpret_cast<POINT*>(&rcCur), 2);
    if (rcCur.left != rcTv.left || rcCur.top != rcTv.top ||
        (rcCur.right - rcCur.left) != tvW || (rcCur.bottom - rcCur.top) != tvH) {
        MoveWindow(hTv, rcTv.left, rcTv.top, tvW, tvH, TRUE);
    }
    if (!IsWindowVisible(hTv)) {
        ShowWindow(hTv, SW_SHOW);
    }
}

static void DrawDashboardCanvas(DashboardState* st, HDC hdcWindow) {
    RECT rcClient;
    GetClientRect(st->hwndMain, &rcClient);
    int width = rcClient.right - rcClient.left;
    int height = rcClient.bottom - rcClient.top;
    if (width <= 0 || height <= 0) return;

    HDC hdc = CreateCompatibleDC(hdcWindow);
    HBITMAP hbm = CreateCompatibleBitmap(hdcWindow, width, height);
    HGDIOBJ oldBm = SelectObject(hdc, hbm);

    HBRUSH bgBrush = CreateSolidBrush(g_theme.windowBg);
    FillRect(hdc, &rcClient, bgBrush);
    DeleteObject(bgBrush);

    const auto& kpiSnaps = st->allSnapshots.empty() ? st->cachedSnapshots : st->allSnapshots;
    int totalCount = static_cast<int>(kpiSnaps.size());
    int activeRunning = 0;
    int idleWaiting = 0;
    int completedOk = 0;
    int failedOrKilled = 0;
    uint32_t totalTreeProcs = 0;
    uint64_t totalMemBytes = 0;

    for (const auto& s : kpiSnaps) {
        if (s.state == ProcState::Running) {
            if (s.idleMs >= 10000) idleWaiting++;
            else activeRunning++;
            totalTreeProcs += s.treeProcessCount;
            totalMemBytes += s.treeMemoryBytes;
        } else if (s.state == ProcState::Completed) {
            completedOk++;
        } else {
            failedOrKilled++;
        }
    }
    TaskQueueCounts tqCounts;
    if (st->tqm) {
        tqCounts = st->tqm->GetJobCounts();
    }
    st->activeRunningCount = activeRunning + tqCounts.running;

    int64_t minRemainingSec = -1;
    for (const auto& s : kpiSnaps) {
        if (s.state == ProcState::Running && s.timeoutSec > 0) {
            int64_t remainingMs = static_cast<int64_t>(s.timeoutSec) * 1000 - static_cast<int64_t>(s.elapsedMs);
            int64_t remSec = (remainingMs + 999) / 1000;
            if (remSec < 0) remSec = 0;
            if (minRemainingSec < 0 || remSec < minRemainingSec) {
                minRemainingSec = remSec;
            }
        }
    }

    // 1. Top KPI Cards (2 rows: Row 0 = Processes, Row 1 = Tasks)
    int kpiMargin = 10;
    int kpiGap = 7;
    int numCols = 7;
    int availW = width - kpiMargin * 2 - kpiGap * (numCols - 1);
    int cardW = (std::max)(90, availW / numCols);

    for (int row = 0; row < 2; ++row) {
        int cardTop = (row == 0) ? kHeaderKpiTop : kHeaderKpiRow2Top;
        int cardBottom = (row == 0) ? kHeaderKpiRow1Bottom : kHeaderKpiBottom;

        for (int col = 0; col < numCols; ++col) {
            int i = row * numCols + col;
            RECT rcCard;
            rcCard.left = kpiMargin + col * (cardW + kpiGap);
            rcCard.right = (col == numCols - 1) ? (width - kpiMargin) : (rcCard.left + cardW);
            rcCard.top = cardTop;
            rcCard.bottom = cardBottom;
            st->kpiCardRects[i] = rcCard;

            if (row == 0) {
                // Top Row: Processes
                if (col == 0) {
                    std::wstring sub = (totalTreeProcs > static_cast<uint32_t>(totalCount))
                        ? (std::to_wstring(totalTreeProcs) + Tr(L" 子", L" child", L" sub")) : L"";
                    DrawKpiCard(hdc, rcCard, Tr(L"プロセス総数", L"TOTAL PROCESSES", L"PROCESOS TOTALES"),
                                std::to_wstring(totalCount), KpiAccent(0), st->hFontUi, st->hFontKpiVal,
                                -1.0f, sub);
                } else if (col == 1) {
                    DrawKpiCard(hdc, rcCard, Tr(L"稼働中プロセス", L"RUNNING PROCS", L"PROCESOS ACTIVOS"),
                                std::to_wstring(activeRunning), KpiAccent(1), st->hFontUi, st->hFontKpiVal,
                                (activeRunning > 0 ? g_kpiBarPhase : -1.0f));
                } else if (col == 2) {
                    DrawKpiCard(hdc, rcCard, Tr(L"待機 / アイドル", L"WAITING / IDLE", L"ESPERANDO / INACTIVO"),
                                std::to_wstring(idleWaiting), KpiAccent(2), st->hFontUi, st->hFontKpiVal);
                } else if (col == 3) {
                    DrawKpiCard(hdc, rcCard, Tr(L"完了 (OK)", L"COMPLETED (OK)", L"COMPLETADO (OK)"),
                                std::to_wstring(completedOk), KpiAccent(3), st->hFontUi, st->hFontKpiVal);
                } else if (col == 4) {
                    DrawKpiCard(hdc, rcCard, Tr(L"失敗 / 強制終了", L"FAILED / KILLED", L"FALLIDO / TERMINADO"),
                                std::to_wstring(failedOrKilled), KpiAccent(4), st->hFontUi, st->hFontKpiVal);
                } else if (col == 5) {
                    std::wstring memStr = Utf8ToWide(FormatBytes(totalMemBytes));
                    DrawKpiCard(hdc, rcCard, Tr(L"ジョブメモリ", L"JOB MEMORY", L"MEMORIA TOTAL"),
                                memStr, KpiAccent(5), st->hFontUi, st->hFontKpiVal);
                } else if (col == 6) {
                    std::wstring val = (minRemainingSec >= 0) ? FormatRemainingTimeShort(minRemainingSec) : Tr(L"なし", L"None", L"Ninguno");
                    DrawKpiCard(hdc, rcCard, Tr(L"残り時間", L"PROC TIMEOUT", L"TIEMPO RESTANTE"),
                                val, KpiAccent(6), st->hFontUi, st->hFontKpiVal);
                }
            } else {
                // Bottom Row: Tasks
                if (col == 0) {
                    DrawKpiCard(hdc, rcCard, Tr(L"タスク総数", L"TOTAL TASKS", L"TAREAS TOTALES"),
                                std::to_wstring(tqCounts.total), KpiAccent(0), st->hFontUi, st->hFontKpiVal);
                } else if (col == 1) {
                    DrawKpiCard(hdc, rcCard, Tr(L"稼働中タスク", L"RUNNING TASKS", L"TAREAS EN EJECUCIÓN"),
                                std::to_wstring(tqCounts.running), KpiAccent(1), st->hFontUi, st->hFontKpiVal,
                                (tqCounts.running > 0 ? g_kpiBarPhase : -1.0f));
                } else if (col == 2) {
                    std::wstring sub = (tqCounts.pending > 0) ? (L"(+" + std::to_wstring(tqCounts.pending) + L")") : L"";
                    DrawKpiCard(hdc, rcCard, Tr(L"実行待ちタスク", L"WAITING TASKS", L"TAREAS EN ESPERA"),
                                std::to_wstring(tqCounts.waiting), KpiAccent(2), st->hFontUi, st->hFontKpiVal,
                                -1.0f, sub);
                } else if (col == 3) {
                    DrawKpiCard(hdc, rcCard, Tr(L"タスク成功 (OK)", L"SUCCEEDED TASKS", L"TAREAS EXITOSAS"),
                                std::to_wstring(tqCounts.succeeded), KpiAccent(3), st->hFontUi, st->hFontKpiVal);
                } else if (col == 4) {
                    std::wstring sub = (tqCounts.cancelled > 0) ? (L"(" + std::to_wstring(tqCounts.cancelled) + L")") : L"";
                    DrawKpiCard(hdc, rcCard, Tr(L"タスク失敗/中断", L"FAILED / CANCELLED", L"FALLIDAS / CANCELADAS"),
                                std::to_wstring(tqCounts.failed + tqCounts.cancelled), KpiAccent(4), st->hFontUi, st->hFontKpiVal,
                                -1.0f, sub);
                } else if (col == 5) {
                    std::wstring sub = (tqCounts.blocked > 0) ? (L"(" + std::to_wstring(tqCounts.blocked) + L")") : L"";
                    DrawKpiCard(hdc, rcCard, Tr(L"保留 / ブロック", L"PENDING / BLOCKED", L"PENDIENTES / BLOQ"),
                                std::to_wstring(tqCounts.pending + tqCounts.blocked), KpiAccent(5), st->hFontUi, st->hFontKpiVal,
                                -1.0f, sub);
                } else if (col == 6) {
                    std::wstring val = (tqCounts.sumRemainingSec >= 0) ? FormatRemainingTimeShort(tqCounts.sumRemainingSec) : Tr(L"なし", L"None", L"Ninguno");
                    DrawKpiCard(hdc, rcCard, Tr(L"残り時間", L"TASK REMAINING", L"RESTANTE TOTAL"),
                                val, KpiAccent(6), st->hFontUi, st->hFontKpiVal);
                }
            }
        }
    }
    UpdateKpiTooltips(st, totalTreeProcs, activeRunning, idleWaiting, completedOk, failedOrKilled,
                      tqCounts, minRemainingSec, tqCounts.minRemainingSec);

    // Draggable splitter between process list and task list (when 2 rows)
    if (st->rcRowsSplitter.bottom > st->rcRowsSplitter.top && st->listDisplayMode == ListDisplayMode::TwoRows) {
        HBRUSH spBrush = CreateSolidBrush(st->rowsSplitterDragging ? g_theme.border : g_theme.panel);
        FillRect(hdc, &st->rcRowsSplitter, spBrush);
        DeleteObject(spBrush);

        int gripY = (st->rcRowsSplitter.top + st->rcRowsSplitter.bottom) / 2;
        int gripCx = (st->rcRowsSplitter.left + st->rcRowsSplitter.right) / 2;
        HPEN gripPen = CreatePen(PS_SOLID, 1, st->rowsSplitterDragging ? g_theme.accent : g_theme.muted);
        HGDIOBJ oldGripPen = SelectObject(hdc, gripPen);
        for (int dx = -14; dx <= 14; dx += 14) {
            MoveToEx(hdc, gripCx + dx - 5, gripY, nullptr);
            LineTo(hdc, gripCx + dx + 5, gripY);
        }
        SelectObject(hdc, oldGripPen);
        DeleteObject(gripPen);
    }

    // Draggable splitter between the process list and the terminal area
    if (st->rcSplitter.bottom > st->rcSplitter.top) {
        HBRUSH spBrush = CreateSolidBrush(st->splitterDragging ? g_theme.border : g_theme.panel);
        FillRect(hdc, &st->rcSplitter, spBrush);
        DeleteObject(spBrush);

        int gripY = (st->rcSplitter.top + st->rcSplitter.bottom) / 2;
        int gripCx = (st->rcSplitter.left + st->rcSplitter.right) / 2;
        HPEN gripPen = CreatePen(PS_SOLID, 1, st->splitterDragging ? g_theme.accent : g_theme.muted);
        HGDIOBJ oldGripPen = SelectObject(hdc, gripPen);
        for (int dx = -14; dx <= 14; dx += 14) {
            MoveToEx(hdc, gripCx + dx - 5, gripY, nullptr);
            LineTo(hdc, gripCx + dx + 5, gripY);
        }
        SelectObject(hdc, oldGripPen);
        DeleteObject(gripPen);
    }

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, g_theme.text);
    HGDIOBJ oldFont = SelectObject(hdc, st->hFontUiBold);

    // 2. Draw Headers & Borders around the active TerminalView tab
    if (st->termBanners.empty()) {
        if (st->rcTerminalArea.bottom > st->rcTerminalArea.top) {
            RECT rcEmpty = st->rcTerminalArea;
            HBRUSH emptyBrush = CreateSolidBrush(g_theme.emptyBg);
            HPEN emptyPen = CreatePen(PS_SOLID, 1, g_theme.border);
            SelectObject(hdc, emptyBrush);
            SelectObject(hdc, emptyPen);
            RoundRect(hdc, rcEmpty.left, rcEmpty.top, rcEmpty.right, rcEmpty.bottom, 6, 6);
            DeleteObject(emptyBrush);
            DeleteObject(emptyPen);

            SetTextColor(hdc, g_theme.muted);
            SelectObject(hdc, st->hFontUiBold);
            DrawTextW(hdc, Tr(L"アクティブな TerminalView がありません。[+ Launch] または Claude CLI / MCP から起動してください。",
                                  L"No active TerminalView sessions. Click [+ New Shell] above or start via Claude CLI / MCP.",
                                  L"No hay sesiones TerminalView activas. Use [+ Launch] o inicie desde Claude CLI / MCP."),
                      -1, &rcEmpty, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    } else {
        for (const auto& tb : st->termBanners) {
            const ProcessSnapshot* snap = nullptr;
            for (const auto& s : st->cachedSnapshots) {
                if (s.id == tb.procId) { snap = &s; break; }
            }
            if (!snap) continue;

            bool isSelected = (snap->id == st->selectedProcId);
            COLORREF borderCol = isSelected ? g_theme.accent : g_theme.border;

            // Outer frame around TerminalView
            HPEN framePen = CreatePen(PS_SOLID, isSelected ? 2 : 1, borderCol);
            HBRUSH frameBrush = CreateSolidBrush(g_theme.panel);
            SelectObject(hdc, framePen);
            SelectObject(hdc, frameBrush);
            Rectangle(hdc, tb.rcBorder.left, tb.rcBorder.top, tb.rcBorder.right, tb.rcBorder.bottom);
            DeleteObject(framePen);
            DeleteObject(frameBrush);

            // Status badge on right of banner
            COLORREF badgeCol = GetStateBadgeColor(*snap);
            RECT rcBadge = {tb.rcBanner.right - 120, tb.rcBanner.top + 3, tb.rcBanner.right - 6, tb.rcBanner.bottom - 3};
            HBRUSH badgeBrush = CreateSolidBrush(badgeCol);
            FillRect(hdc, &rcBadge, badgeBrush);
            DeleteObject(badgeBrush);

            SetTextColor(hdc, RGB(255, 255, 255));
            SelectObject(hdc, st->hFontUiBold);
            std::wstring stateStr = StateLabelWide(snap->state, snap->idleMs >= 10000);
            DrawTextW(hdc, stateStr.c_str(), -1, &rcBadge, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            // Title text on left of banner (includes Start Time & Uptime HH:MM:SS)
            std::wstringstream ss;
            ss << L"#" << snap->id;
            if (!snap->agentType.empty()) {
                ss << L" [" << Utf8ToWide(snap->agentType) << L"]";
            }
            ss << L" [" << Utf8ToWide(snap->name) << L"]  PID: " << snap->rootPid
               << L" (" << snap->treeProcessCount << L" procs, " << Utf8ToWide(FormatBytes(snap->treeMemoryBytes))
               << Tr(L")  開始: ", L")  Start: ", L")  Inicio: ") << Utf8ToWide(snap->startTimeStr)
               << Tr(L"  稼働時間: ", L"  Uptime: ", L"  Tiempo activo: ") << Utf8ToWide(FormatUptimeHMS(snap->elapsedMs));
            if (snap->state == ProcState::Running) {
                if (snap->timeoutSec > 0) {
                    int64_t remainingMs = static_cast<int64_t>(snap->timeoutSec) * 1000 - static_cast<int64_t>(snap->elapsedMs);
                    int64_t remSec = (remainingMs + 999) / 1000;
                    if (remSec < 0) remSec = 0;
                    ss << Tr(L"  残り時間: ", L"  Left: ", L"  Restante: ") << FormatRemainingTime(remSec);
                } else if (!snap->estimatedEndTimeStr.empty()) {
                    ss << Tr(L"  予定終了: ~", L"  Est. End: ~", L"  Fin est.: ~") << Utf8ToWide(snap->estimatedEndTimeStr);
                }
            }
            ss << L"  —  " << Utf8ToWide(snap->commandLine);
            RECT rcTitle = {tb.rcBanner.left + 8, tb.rcBanner.top + 2, rcBadge.left - 8, tb.rcBanner.bottom - 2};
            SetTextColor(hdc, isSelected ? g_theme.accent : g_theme.text);
            DrawTextW(hdc, ss.str().c_str(), -1, &rcTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            // Terminal viewport backing (used when PrintWindow captures parent GDI surface)
            RECT rcTv = {tb.rcBorder.left + 2, tb.rcBanner.bottom, tb.rcBorder.right - 2, tb.rcBorder.bottom - 2};
            HBRUSH tvBrush = CreateSolidBrush(g_theme.terminalBg);
            FillRect(hdc, &rcTv, tvBrush);
            DeleteObject(tvBrush);
            if (st->pm) {
                TerminalView* tv = st->pm->GetTerminalView(snap->id);
                if (tv) {
                    std::wstring termText = tv->ExtractScreenText(0);
                    if (!termText.empty()) {
                        HFONT hFontMono = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                                      FIXED_PITCH | FF_MODERN, L"Consolas");
                        HGDIOBJ oldMono = SelectObject(hdc, hFontMono);
                        SetTextColor(hdc, g_theme.text);
                        RECT rcPad = {rcTv.left + 8, rcTv.top + 6, rcTv.right - 8, rcTv.bottom - 6};
                        DrawTextW(hdc, termText.c_str(), -1, &rcPad, DT_LEFT | DT_TOP | DT_NOPREFIX);
                        SelectObject(hdc, oldMono);
                        DeleteObject(hFontMono);
                    }
                }
            }
        }
    }

    // 3. Bottom Status Bar (replaces the old Send Input / Ctrl+C / Focus Term area)
    RECT rcSb = st->rcStatusBar;
    HBRUSH sbBrush = CreateSolidBrush(g_theme.panel);
    FillRect(hdc, &rcSb, sbBrush);
    DeleteObject(sbBrush);

    HPEN sbTopPen = CreatePen(PS_SOLID, 1, g_theme.border);
    SelectObject(hdc, sbTopPen);
    MoveToEx(hdc, rcSb.left, rcSb.top, nullptr);
    LineTo(hdc, rcSb.right, rcSb.top);
    DeleteObject(sbTopPen);

    const ProcessSnapshot* selSnap = nullptr;
    for (const auto& s : st->cachedSnapshots) {
        if (s.id == st->selectedProcId) { selSnap = &s; break; }
    }

    std::wstringstream sbLeft;
    if (selSnap) {
        sbLeft << Tr(L"選択: #", L"Selected: #", L"Seleccionado: #") << selSnap->id << L" [" << Utf8ToWide(selSnap->name) << L"]  |  "
               << StateLabelWide(selSnap->state, selSnap->idleMs >= 10000)
               << L"  |  " << Tr(L"開始: ", L"Start: ", L"Inicio: ") << Utf8ToWide(selSnap->startTimeStr)
               << L"  |  " << Tr(L"稼働時間: ", L"Uptime: ", L"Tiempo activo: ") << Utf8ToWide(FormatUptimeHMS(selSnap->elapsedMs));
        if (selSnap->state == ProcState::Running && selSnap->timeoutSec > 0) {
            int64_t remainingMs = static_cast<int64_t>(selSnap->timeoutSec) * 1000 - static_cast<int64_t>(selSnap->elapsedMs);
            int64_t remSec = (remainingMs + 999) / 1000;
            if (remSec < 0) remSec = 0;
            sbLeft << L"  |  " << Tr(L"残り時間: ", L"Left: ", L"Restante: ") << FormatRemainingTime(remSec);
        }
        if (!selSnap->logFilePath.empty()) {
            sbLeft << L"  |  " << Tr(L"ログ: ", L"Log: ", L"Registro: ") << Utf8ToWide(selSnap->logFilePath);
        }
        if (st->pm) {
            TerminalView* tv = st->pm->GetTerminalView(selSnap->id);
            if (tv && !tv->Title().empty()) {
                sbLeft << L"  |  " << Tr(L"タイトル: ", L"Title: ", L"Título: ") << tv->Title();
            }
        }
        if (st->termProgressState > 0) {
            sbLeft << L"  |  " << Tr(L"進捗: ", L"Progress: ", L"Progreso: ") << st->termProgressValue << L"%";
        }
    } else {
        sbLeft << Tr(L"待機 — ログ保存先: ", L"Ready — Logs saved to: ", L"Listo — Registros en: ") << GetLogsDirectoryWide();
    }
    sbLeft << Tr(L"  |  Ctrl+T: プロセス / タスクキュー切替",
                 L"  |  Ctrl+T: switch Processes / Task Queue",
                 L"  |  Ctrl+T: cambiar Procesos / Cola de tareas");

    uint64_t dashElapsedMs = GetNowTickMs() - st->dashboardStartTickMs;
    std::wstringstream sbRight;
    sbRight << Tr(L"ダッシュボード起動: ", L"Dashboard Started: ", L"Panel iniciado: ") << Utf8ToWide(st->dashboardStartTimeStr)
            << L"  |  " << Tr(L"稼働時間: ", L"Uptime: ", L"Tiempo activo: ") << Utf8ToWide(FormatUptimeHMS(dashElapsedMs))
            << L"  |  IPC: \\\\.\\pipe\\ProcessCommanderIPC";

    SelectObject(hdc, st->hFontUi);
    RECT rcSbRight = {rcSb.right - 520, rcSb.top + 2, rcSb.right - 10, rcSb.bottom - 2};
    SetTextColor(hdc, g_theme.muted);
    DrawTextW(hdc, sbRight.str().c_str(), -1, &rcSbRight, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT rcSbLeft = {rcSb.left + 10, rcSb.top + 2, rcSbRight.left - 12, rcSb.bottom - 2};
    SetTextColor(hdc, g_theme.text);
    DrawTextW(hdc, sbLeft.str().c_str(), -1, &rcSbLeft, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(hdc, oldFont);
    BitBlt(hdcWindow, 0, 0, width, height, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, oldBm);
    DeleteObject(hbm);
    DeleteDC(hdc);
}

static const wchar_t* ColumnTitle(int index) {
    static const wchar_t* const ja[13] = {
        L"ID", L"エージェント / 名前", L"状態", L"開始時刻", L"終了時刻 / 残り時間", L"稼働時間",
        L"PID (ツリー)", L"メモリ", L"CPU時間", L"アイドル", L"最新出力プレビュー",
        L"コマンドライン", L"ジョブ"
    };
    static const wchar_t* const en[13] = {
        L"ID", L"Agent / Name", L"Status", L"Start Time", L"End Time / Left", L"Uptime",
        L"PID (Tree)", L"Memory", L"CPU Time", L"Idle", L"Latest Output Preview",
        L"Command Line", L"Job"
    };
    static const wchar_t* const es[13] = {
        L"ID", L"Agente / Nombre", L"Estado", L"Hora de inicio", L"Fin / Restante", L"Tiempo activo",
        L"PID (Árbol)", L"Memoria", L"Tiempo de CPU", L"Inactivo", L"Última salida",
        L"Línea de comandos", L"Trabajo"
    };
    if (g_uiLanguage == UiLanguage::Spanish) return es[index];
    if (g_uiLanguage == UiLanguage::English) return en[index];
    return ja[index];
}
static void UpdateListHeaderSortIndicator(DashboardState* st) {
    if (!st || !st->hwndList) return;
    HWND hHeader = ListView_GetHeader(st->hwndList);
    for (int i = 0; i < 13; ++i) {
        std::wstring title = ColumnTitle(i);
        if (i == st->sortColumn) {
            title += st->sortAscending ? L" ▲" : L" ▼";
        }
        LVCOLUMNW lvc = {};
        lvc.mask = LVCF_TEXT;
        lvc.pszText = title.data();
        ListView_SetColumn(st->hwndList, i, &lvc);

        if (hHeader) {
            HDITEMW hdi = {};
            hdi.mask = HDI_FORMAT;
            if (Header_GetItem(hHeader, i, &hdi)) {
                hdi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
                if (i == st->sortColumn) {
                    hdi.fmt |= st->sortAscending ? HDF_SORTUP : HDF_SORTDOWN;
                }
                Header_SetItem(hHeader, i, &hdi);
            }
        }
    }
}

static const wchar_t* TaskColumnTitle(int index) {
    static const wchar_t* ja[9] = {
        L"ID", L"サマリ", L"状態", L"開始時刻", L"終了 / 残り時間",
        L"試行上限", L"タイムアウト", L"依存 / 理由", L"キュー"
    };
    static const wchar_t* en[9] = {
        L"ID", L"Summary", L"State", L"Start", L"End / Left",
        L"Max Attempts", L"Timeout", L"Depends / Reason", L"Queue"
    };
    static const wchar_t* es[9] = {
        L"ID", L"Resumen", L"Estado", L"Inicio", L"Fin / Restante",
        L"Intentos", L"Tiempo limite", L"Dependencias", L"Cola"
    };
    if (g_uiLanguage == UiLanguage::Spanish) return es[index];
    if (g_uiLanguage == UiLanguage::English) return en[index];
    return ja[index];
}

static void UpdateTaskListHeaders(DashboardState* st) {
    if (!st || !st->hwndTaskList) return;
    HWND hHeader = ListView_GetHeader(st->hwndTaskList);
    for (int i = 0; i < 9; ++i) {
        LVCOLUMNW lvc = {};
        lvc.mask = LVCF_TEXT;
        lvc.pszText = const_cast<wchar_t*>(TaskColumnTitle(i));
        ListView_SetColumn(st->hwndTaskList, i, &lvc);
        if (hHeader) {
            HDITEMW hdi = {};
            hdi.mask = HDI_FORMAT;
            if (Header_GetItem(hHeader, i, &hdi)) {
                hdi.fmt |= HDF_OWNERDRAW;
                Header_SetItem(hHeader, i, &hdi);
            }
        }
    }
}

static void UpdateControlTooltip(HWND hwndTip, HWND hwndParent, HWND hwndCtrl, const wchar_t* text) {
    if (!hwndTip || !hwndCtrl) return;
    TOOLINFOW ti = {};
    ti.cbSize = sizeof(ti);
    ti.hwnd = hwndParent;
    ti.uId = reinterpret_cast<UINT_PTR>(hwndCtrl);
    ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
    ti.lpszText = const_cast<wchar_t*>(text ? text : L"");
    SendMessageW(hwndTip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
}

static void UpdatePanelToggleLabels(DashboardState* st) {
    if (!st) return;
    if (st->hwndBtnToggleList) {
        SetWindowTextW(st->hwndBtnToggleList, L"☰");
    }
    if (st->hwndBtnToggleTerm) {
        SetWindowTextW(st->hwndBtnToggleTerm, L">_");
    }
    if (st->hwndToolbarTip) {
        if (st->hwndBtnToggleList) {
            UpdateControlTooltip(st->hwndToolbarTip, st->hwndMain, st->hwndBtnToggleList,
                st->hideProcessList ? Tr(L"リストを表示", L"Show List", L"Mostrar lista")
                                    : Tr(L"リストを非表示", L"Hide List", L"Ocultar lista"));
        }
        if (st->hwndBtnToggleTerm) {
            UpdateControlTooltip(st->hwndToolbarTip, st->hwndMain, st->hwndBtnToggleTerm,
                st->hideTerminalGrid ? Tr(L"ターミナルを表示", L"Show Terminal", L"Mostrar terminal")
                                     : Tr(L"ターミナルを非表示", L"Hide Terminal", L"Ocultar terminal"));
        }
    }
}

static void SortSnapshots(std::vector<ProcessSnapshot>& snaps, int col, bool asc) {
    std::sort(snaps.begin(), snaps.end(), [col, asc](const ProcessSnapshot& a, const ProcessSnapshot& b) {
        int cmp = 0;
        switch (col) {
            case 0: // ID
                cmp = (a.id < b.id) ? -1 : (a.id > b.id ? 1 : 0);
                break;
            case 1: // Name
                cmp = _stricmp(a.name.c_str(), b.name.c_str());
                break;
            case 2: // Status
                cmp = (static_cast<int>(a.state) < static_cast<int>(b.state)) ? -1 :
                      (static_cast<int>(a.state) > static_cast<int>(b.state) ? 1 : 0);
                break;
            case 3: // Start Time
                cmp = a.startTimeStr.compare(b.startTimeStr);
                break;
            case 4: // End Time
                cmp = a.endTimeStr.compare(b.endTimeStr);
                break;
            case 5: // Uptime
                cmp = (a.elapsedMs < b.elapsedMs) ? -1 : (a.elapsedMs > b.elapsedMs ? 1 : 0);
                break;
            case 6: // PID
                cmp = (a.rootPid < b.rootPid) ? -1 : (a.rootPid > b.rootPid ? 1 : 0);
                break;
            case 7: // Memory
                cmp = (a.treeMemoryBytes < b.treeMemoryBytes) ? -1 : (a.treeMemoryBytes > b.treeMemoryBytes ? 1 : 0);
                break;
            case 8: // CPU Time
                cmp = (a.cpuTimeMs < b.cpuTimeMs) ? -1 : (a.cpuTimeMs > b.cpuTimeMs ? 1 : 0);
                break;
            case 9: // Idle
                cmp = (a.idleMs < b.idleMs) ? -1 : (a.idleMs > b.idleMs ? 1 : 0);
                break;
            case 10: // Latest Output Preview
                cmp = _stricmp(a.lastLinePreview.c_str(), b.lastLinePreview.c_str());
                break;
            case 11: // Command Line
                cmp = _stricmp(a.commandLine.c_str(), b.commandLine.c_str());
                break;
            default:
                cmp = (a.id < b.id) ? -1 : (a.id > b.id ? 1 : 0);
                break;
        }
        if (cmp == 0) {
            cmp = (a.id < b.id) ? -1 : (a.id > b.id ? 1 : 0);
        }
        return asc ? (cmp < 0) : (cmp > 0);
    });
}


static void PlayCompletionSound(const DashboardState& st) {
    if (!st.completionSoundEnabled) return;
    if (!st.completionSoundPath.empty()) {
        std::wstring path = Utf8ToWide(st.completionSoundPath);
        if (PlaySoundW(path.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT)) {
            return;
        }
    }
    if (!PlaySoundW(L"Notification.Default", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT)) {
        if (!PlaySoundW(L"SystemAsterisk", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT)) {
            MessageBeep(MB_ICONASTERISK);
        }
    }
}


static void RefreshDashboardData(DashboardState* st);

static std::vector<std::string> SplitJsonObjectArray(const std::string& json, const std::string& key) {
    std::vector<std::string> objs;
    std::string pattern = "\"" + key + "\"";
    size_t keyPos = json.find(pattern);
    if (keyPos == std::string::npos) return objs;
    size_t arr = json.find('[', keyPos);
    if (arr == std::string::npos) return objs;
    int depth = 0;
    bool inStr = false;
    bool esc = false;
    size_t objStart = std::string::npos;
    for (size_t i = arr; i < json.size(); ++i) {
        char ch = json[i];
        if (inStr) {
            if (esc) esc = false;
            else if (ch == '\\') esc = true;
            else if (ch == '"') inStr = false;
            continue;
        }
        if (ch == '"') { inStr = true; continue; }
        if (ch == '{') {
            if (depth == 0) objStart = i;
            depth++;
        } else if (ch == '}') {
            if (depth > 0) depth--;
            if (depth == 0 && objStart != std::string::npos) {
                objs.push_back(json.substr(objStart, i - objStart + 1));
                objStart = std::string::npos;
            }
        } else if (ch == ']' && depth == 0) {
            break;
        }
    }
    return objs;
}

static std::string ExtractJsonArrayText(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    size_t keyPos = json.find(pattern);
    if (keyPos == std::string::npos) return "";
    size_t arr = json.find('[', keyPos);
    if (arr == std::string::npos) return "";
    int depth = 0;
    bool inStr = false;
    bool esc = false;
    for (size_t i = arr; i < json.size(); ++i) {
        char ch = json[i];
        if (inStr) {
            if (esc) esc = false;
            else if (ch == '\\') esc = true;
            else if (ch == '"') inStr = false;
            continue;
        }
        if (ch == '"') { inStr = true; continue; }
        if (ch == '[') depth++;
        else if (ch == ']') { depth--; if (depth == 0) return json.substr(arr, i - arr + 1); }
    }
    return "";
}

static std::wstring TaskQueueDepsDisplay(const std::string& jobJson) {
    std::string raw = ExtractJsonArrayText(jobJson, "depends_on");
    std::wstring out;
    size_t pos = 0;
    while ((pos = raw.find("\"id\":\"", pos)) != std::string::npos) {
        pos += 6;
        size_t endQuote = raw.find('"', pos);
        if (endQuote == std::string::npos) break;
        if (!out.empty()) out += L", ";
        out += Utf8ToWide(raw.substr(pos, endQuote - pos));
        pos = endQuote;
    }
    return out;
}

static std::string LastAttemptReason(const std::string& jobJson) {
    auto attempts = SplitJsonObjectArray(jobJson, "attempts");
    if (attempts.empty()) return "";
    return JsonGetString(attempts.back(), "reason", "");
}

static std::string LastAttemptField(const std::string& jobJson, const std::string& key) {
    auto attempts = SplitJsonObjectArray(jobJson, "attempts");
    if (attempts.empty()) return "";
    return JsonGetString(attempts.back(), key, "");
}

static std::string TimeOfDayText(const std::string& ts) {
    size_t space = ts.find(' ');
    if (space != std::string::npos && ts.size() >= space + 9) return ts.substr(space + 1);
    return ts;
}

static std::string AddSecondsToTimestamp(const std::string& ts, int seconds) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (sscanf_s(ts.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) return "";
    struct tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = s;
    t.tm_isdst = -1;
    time_t tt = mktime(&t);
    if (tt == static_cast<time_t>(-1)) return "";
    tt += seconds;
    struct tm out = {};
    localtime_s(&out, &tt);
    char buf[32] = {};
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             out.tm_year + 1900, out.tm_mon + 1, out.tm_mday,
             out.tm_hour, out.tm_min, out.tm_sec);
    return buf;
}

static std::vector<std::pair<std::string, std::string>> TaskQueueDepList(const std::string& jobJson) {
    std::vector<std::pair<std::string, std::string>> deps;
    for (const auto& d : SplitJsonObjectArray(jobJson, "depends_on")) {
        std::string id = JsonGetString(d, "id", "");
        std::string on = JsonGetString(d, "on", "success");
        if (!id.empty()) deps.emplace_back(id, on);
    }
    return deps;
}

static std::wstring TaskQueueStateLabel(const std::string& state, const std::string& jobJson,
                                        const std::unordered_map<std::string, std::string>& stateById) {
    if (state != "pending") return Utf8ToWide(state);
    bool waitingForDeps = false;
    for (const auto& dep : TaskQueueDepList(jobJson)) {
        auto it = stateById.find(dep.first);
        std::string depState = (it == stateById.end()) ? "" : it->second;
        bool satisfied = (dep.second == "done")
            ? (depState == "succeeded" || depState == "failed" || depState == "cancelled")
            : (depState == "succeeded");
        if (!satisfied) {
            waitingForDeps = true;
            break;
        }
    }
    // waiting: queued and ready to run (waiting for a free slot).
    // pending: not in the run queue (dependencies not satisfied yet).
    return waitingForDeps ? std::wstring(L"pending") : std::wstring(L"waiting");
}

static void ShowTaskQueueJobDetails(DashboardState* st, const std::string& jobId) {
    if (!st || !st->tqm || jobId.empty()) return;
    std::string err;
    std::string resp = st->tqm->TaskQueueGet(jobId, true, 0, err);
    MessageBoxW(st->hwndMain, Utf8ToWide(resp).c_str(), Utf8ToWide(jobId).c_str(), MB_OK | MB_ICONINFORMATION);
}

static void ShowTaskQueueContextMenu(DashboardState* st, const std::string& jobId, POINT ptScreen) {
    if (!st || !st->tqm || jobId.empty()) return;
    std::string err;
    std::string resp = st->tqm->TaskQueueGet(jobId, false, 0, err);
    std::string jobState = JsonGetString(resp, "state", "");

    HMENU hMenu = CreatePopupMenu();
    if (jobState == "running") {
        AppendMenuW(hMenu, MF_STRING, 3107, Tr(L"ツリー終了 (ジョブ中断)", L"Kill Tree (Cancel Job)", L"Terminar árbol (Cancelar)"));
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(hMenu, MF_STRING, 3101, Tr(L"再実行 (Retry)", L"Retry", L"Reintentar"));
    AppendMenuW(hMenu, MF_STRING, 3102, Tr(L"キャンセル (Cancel)", L"Cancel", L"Cancelar"));
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, 3103, Tr(L"キューを一時停止", L"Pause queue", L"Pausar cola"));
    AppendMenuW(hMenu, MF_STRING, 3104, Tr(L"キューを再開", L"Resume queue", L"Reanudar cola"));
    AppendMenuW(hMenu, MF_STRING, 3105, Tr(L"終了済みを消去", L"Purge finished", L"Purgar finalizados"));
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, 3106, Tr(L"詳細を表示", L"Show details", L"Ver detalles"));

    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, ptScreen.x, ptScreen.y, 0, st->hwndMain, nullptr);
    DestroyMenu(hMenu);

    if (cmd == 3107) {
        if (MessageBoxW(st->hwndMain,
                Tr(L"このタスクのプロセスツリーを強制終了（ジョブ中断）しますか？",
                   L"Are you sure you want to kill this task's process tree?",
                   L"¿Seguro que desea terminar el árbol de procesos de esta tarea?"),
                Tr(L"ツリー終了の確認", L"Confirm Kill Tree", L"Confirmar cierre"),
                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
            st->tqm->TaskQueueCancel(jobId, err);
        }
    } else if (cmd == 3101) {
        st->tqm->TaskQueueRetry(jobId, true, true, err);
    } else if (cmd == 3102) {
        st->tqm->TaskQueueCancel(jobId, err);
    } else if (cmd == 3103 || cmd == 3104) {
        resp = st->tqm->TaskQueueGet(jobId, false, 0, err);
        std::string tqName = JsonGetString(resp, "taskqueue", "");
        if (!tqName.empty()) st->tqm->TaskQueuePause(tqName, cmd == 3103, err);
    } else if (cmd == 3105) {
        resp = st->tqm->TaskQueueList("", "", err);
        auto jobs = SplitJsonObjectArray(resp, "jobs");
        std::vector<std::string> queuesDone;
        for (const auto& j : jobs) {
            std::string tqName = JsonGetString(j, "taskqueue", "");
            if (tqName.empty()) continue;
            if (std::find(queuesDone.begin(), queuesDone.end(), tqName) != queuesDone.end()) continue;
            queuesDone.push_back(tqName);
            st->tqm->TaskQueuePurge(tqName, "[\"succeeded\",\"failed\",\"cancelled\"]", err);
        }
    } else if (cmd == 3106) {
        ShowTaskQueueJobDetails(st, jobId);
    }
    RefreshDashboardData(st);
}

static void RefreshTaskQueueData(DashboardState* st) {
    if (!st || !st->tqm || !st->hwndTaskList) return;
    if (st->listDisplayMode == ListDisplayMode::ProcessOnly) return;
    std::string err;
    std::string resp = st->tqm->TaskQueueList("", "", err);
    auto jobs = SplitJsonObjectArray(resp, "jobs");

    std::unordered_map<std::string, std::string> stateById;
    stateById.reserve(jobs.size());
    for (const auto& j : jobs) {
        stateById[JsonGetString(j, "id", "")] = JsonGetString(j, "state", "");
    }

    auto depsSatisfied = [&](const std::string& jobJson) {
        for (const auto& dep : TaskQueueDepList(jobJson)) {
            auto it = stateById.find(dep.first);
            std::string depState = (it == stateById.end()) ? "" : it->second;
            bool satisfied = (dep.second == "done")
                ? (depState == "succeeded" || depState == "failed" || depState == "cancelled")
                : (depState == "succeeded");
            if (!satisfied) return false;
        }
        return true;
    };
    auto jobRank = [&](const std::string& jobJson) {
        std::string st = JsonGetString(jobJson, "state", "");
        if (st == "running") return 0;
        if (st == "pending") return depsSatisfied(jobJson) ? 1 : 2;
        return 3;
    };
    std::stable_sort(jobs.begin(), jobs.end(), [&](const std::string& a, const std::string& b) {
        return jobRank(a) < jobRank(b);
    });

    st->taskJobIds.clear();
    st->taskJobStates.clear();
    SendMessageW(st->hwndTaskList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(st->hwndTaskList);

    int row = 0;
    for (const auto& j : jobs) {
        std::string jobId = JsonGetString(j, "id", "");
        std::string state = JsonGetString(j, "state", "");
        std::string lastReason = LastAttemptReason(j);
        std::string kind = "other";
        if (state == "running") {
            kind = "running";
        } else if (state == "failed") {
            kind = (lastReason == "timed_out" || lastReason == "idle_timed_out") ? "timeout" : "failed";
        }
        st->taskJobIds.push_back(jobId);
        st->taskJobStates.push_back(kind);

        std::wstring idStr = Utf8ToWide(jobId);
        std::wstring summaryStr = Utf8ToWide(JsonGetString(j, "summary", ""));
        std::wstring stateStr = TaskQueueStateLabel(state, j, stateById);
        std::wstring attemptsStr = std::to_wstring(JsonGetInt(j, "max_attempts", 0));
        std::wstring timeoutStr = std::to_wstring(JsonGetInt(j, "timeout_sec", 0)) + L"s";
        std::string attemptStart = LastAttemptField(j, "start");
        std::string attemptEnd = LastAttemptField(j, "end");
        std::wstring startStr = L"-";
        std::wstring endStr = L"-";
        if (!attemptStart.empty()) {
            startStr = Utf8ToWide(TimeOfDayText(attemptStart));
            if (!attemptEnd.empty()) {
                endStr = Utf8ToWide(TimeOfDayText(attemptEnd));
            } else if (state == "running") {
                int timeoutSec = static_cast<int>(JsonGetInt(j, "timeout_sec", 0));
                if (timeoutSec > 0) {
                    std::string est = AddSecondsToTimestamp(attemptStart, timeoutSec);
                    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
                    if (sscanf_s(attemptStart.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &sec) == 6) {
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
                            int64_t remSec = static_cast<int64_t>(timeoutSec) - elapsed;
                            if (remSec < 0) remSec = 0;
                            if (!est.empty()) {
                                endStr = L"~" + Utf8ToWide(TimeOfDayText(est)) + L" (" + FormatRemainingTime(remSec) + L")";
                            } else {
                                endStr = FormatRemainingTime(remSec);
                            }
                        }
                    }
                    if (endStr.empty() && !est.empty()) {
                        endStr = L"~" + Utf8ToWide(TimeOfDayText(est));
                    }
                }
            }
        } else if (state == "pending") {
            int timeoutSec = static_cast<int>(JsonGetInt(j, "timeout_sec", 0));
            if (timeoutSec > 0) {
                endStr = FormatRemainingTime(timeoutSec);
            }
        }
        std::wstring depsStr = TaskQueueDepsDisplay(j);
        std::string blockedBy = JsonGetString(j, "blocked_by", "");
        if (!blockedBy.empty()) {
            if (!depsStr.empty()) depsStr += L" ";
            depsStr += L"<- " + Utf8ToWide(blockedBy);
        }
        std::wstring queueStr = Utf8ToWide(JsonGetString(j, "taskqueue", ""));

        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = row;
        lvi.iSubItem = 0;
        lvi.pszText = idStr.data();
        ListView_InsertItem(st->hwndTaskList, &lvi);
        ListView_SetItemText(st->hwndTaskList, row, 1, summaryStr.data());
        ListView_SetItemText(st->hwndTaskList, row, 2, stateStr.data());
        ListView_SetItemText(st->hwndTaskList, row, 3, startStr.data());
        ListView_SetItemText(st->hwndTaskList, row, 4, endStr.data());
        ListView_SetItemText(st->hwndTaskList, row, 5, attemptsStr.data());
        ListView_SetItemText(st->hwndTaskList, row, 6, timeoutStr.data());
        ListView_SetItemText(st->hwndTaskList, row, 7, depsStr.data());
        ListView_SetItemText(st->hwndTaskList, row, 8, queueStr.data());
        row++;
    }
    SendMessageW(st->hwndTaskList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(st->hwndTaskList, nullptr, FALSE);
}

static void RefreshDashboardData(DashboardState* st) {
    if (!st || !st->pm) return;
    st->allSnapshots = st->pm->GetAllSnapshots();

    bool processFinished = false;
    std::unordered_map<int, ProcState> nextStates;
    nextStates.reserve(st->allSnapshots.size());
    for (const auto& s : st->allSnapshots) {
        auto it = st->lastProcStates.find(s.id);
        if (it != st->lastProcStates.end() && it->second == ProcState::Running &&
            s.state != ProcState::Running) {
            processFinished = true;
        }
        nextStates.emplace(s.id, s.state);
    }
    st->lastProcStates.swap(nextStates);
    if (processFinished) {
        PlayCompletionSound(*st);
    }

    st->cachedSnapshots.clear();
    for (const auto& s : st->allSnapshots) {
        if (st->hideEndedProcesses && s.state != ProcState::Running) {
            continue;
        }
        st->cachedSnapshots.push_back(s);
    }

    SortSnapshots(st->cachedSnapshots, st->sortColumn, st->sortAscending);

    auto getNewestId = [&]() -> int {
        int maxId = -1;
        for (const auto& s : st->cachedSnapshots) {
            if (s.id > maxId) maxId = s.id;
        }
        return maxId;
    };

    if (st->selectedProcId <= 0 && !st->cachedSnapshots.empty()) {
        st->selectedProcId = getNewestId();
    } else if (st->selectedProcId > 0) {
        bool exists = false;
        for (const auto& s : st->cachedSnapshots) {
            if (s.id == st->selectedProcId) { exists = true; break; }
        }
        if (!exists) {
            st->selectedProcId = getNewestId();
        }
    }

    LayoutTerminalViews(st);

    int currentCount = ListView_GetItemCount(st->hwndList);
    int targetCount = static_cast<int>(st->cachedSnapshots.size());

    SendMessageW(st->hwndList, WM_SETREDRAW, FALSE, 0);

    while (currentCount > targetCount) {
        ListView_DeleteItem(st->hwndList, currentCount - 1);
        currentCount--;
    }

    for (int i = 0; i < targetCount; ++i) {
        const auto& s = st->cachedSnapshots[i];
        std::wstring idStr = L"#" + std::to_wstring(s.id);
        std::wstring nameStr = Utf8ToWide(s.name);
        if (!s.agentType.empty()) {
            nameStr = L"[" + Utf8ToWide(s.agentType) + L"] " + nameStr;
        }
        std::wstring statusStr = StateLabelWide(s.state, s.idleMs >= 10000);
        if (!s.agentStatus.empty() && s.agentStatus != "idle" && s.state == ProcState::Running) {
            statusStr += L" (" + Utf8ToWide(s.agentStatus) + L")";
        } else if (s.state != ProcState::Running && s.exitCode != STILL_ACTIVE) {
            statusStr += L" (" + std::to_wstring(s.exitCode) + L")";
        }
        std::wstring startStr = Utf8ToWide(s.startTimeStr);
        std::wstring endStr;
        if (s.state == ProcState::Running) {
            if (s.timeoutSec > 0) {
                int64_t remainingMs = static_cast<int64_t>(s.timeoutSec) * 1000 -
                                      static_cast<int64_t>(s.elapsedMs);
                if (remainingMs < 0) remainingMs = 0;
                long long remainingSec = (remainingMs + 999) / 1000;
                endStr = FormatRemainingTime(remainingSec);
                if (!s.estimatedEndTimeStr.empty()) {
                    endStr = Utf8ToWide(s.estimatedEndTimeStr) + L" (" + endStr + L")";
                }
            } else if (!s.estimatedEndTimeStr.empty()) {
                endStr = L"~" + Utf8ToWide(s.estimatedEndTimeStr);
            }
        }
        if (endStr.empty()) {
            endStr = (s.state == ProcState::Running || s.endTimeStr.empty()) ? L"-" : Utf8ToWide(s.endTimeStr);
        }
        std::wstring uptimeStr = Utf8ToWide(FormatUptimeHMS(s.elapsedMs));
        if (s.timeoutSec > 0) {
            uptimeStr += L" / " + std::to_wstring(s.timeoutSec) + L"s";
        }
        std::wstring pidStr = std::to_wstring(s.rootPid) + L" (" + std::to_wstring(s.treeProcessCount) + L")";
        std::wstring memStr = Utf8ToWide(FormatBytes(s.treeMemoryBytes));
        std::wstring cpuStr = Utf8ToWide(FormatUptimeHMS(s.cpuTimeMs));
        std::wstring idleStr = L"-";
        if (s.state == ProcState::Running) {
            idleStr = Utf8ToWide(FormatDurationSec(s.idleMs));
            if (s.idleTimeoutSec > 0) {
                int64_t idleRemainSec = static_cast<int64_t>(s.idleTimeoutSec) - static_cast<int64_t>(s.idleMs / 1000);
                if (idleRemainSec < 0) idleRemainSec = 0;
                idleStr += L" (残: " + std::to_wstring(idleRemainSec) + L"s)";
            }
        }
        std::wstring previewStr = Utf8ToWide(s.lastLinePreview);
        std::wstring cmdStr = Utf8ToWide(s.commandLine);
        std::wstring jobStr = L"-";
        if (!s.jobKey.empty()) {
            jobStr = Utf8ToWide(s.jobKey) + L" #" + std::to_wstring(s.attemptN);
        }

        if (i >= currentCount) {
            LVITEMW lvi = {};
            lvi.mask = LVIF_TEXT | LVIF_PARAM;
            lvi.iItem = i;
            lvi.iSubItem = 0;
            lvi.pszText = idStr.data();
            lvi.lParam = s.id;
            ListView_InsertItem(st->hwndList, &lvi);
        } else {
            LVITEMW lvi = {};
            lvi.mask = LVIF_PARAM;
            lvi.iItem = i;
            lvi.lParam = s.id;
            ListView_SetItem(st->hwndList, &lvi);
            ListView_SetItemText(st->hwndList, i, 0, idStr.data());
        }

        ListView_SetItemText(st->hwndList, i, 1, nameStr.data());
        ListView_SetItemText(st->hwndList, i, 2, statusStr.data());
        ListView_SetItemText(st->hwndList, i, 3, startStr.data());
        ListView_SetItemText(st->hwndList, i, 4, endStr.data());
        ListView_SetItemText(st->hwndList, i, 5, uptimeStr.data());
        ListView_SetItemText(st->hwndList, i, 6, pidStr.data());
        ListView_SetItemText(st->hwndList, i, 7, memStr.data());
        ListView_SetItemText(st->hwndList, i, 8, cpuStr.data());
        ListView_SetItemText(st->hwndList, i, 9, idleStr.data());
        ListView_SetItemText(st->hwndList, i, 10, previewStr.data());
        ListView_SetItemText(st->hwndList, i, 11, cmdStr.data());
        ListView_SetItemText(st->hwndList, i, 12, jobStr.data());
    }

    int selIdx = -1;
    for (int i = 0; i < targetCount; ++i) {
        if (st->cachedSnapshots[i].id == st->selectedProcId) {
            selIdx = i;
            break;
        }
    }
    if (selIdx >= 0) {
        UINT selState = ListView_GetItemState(st->hwndList, selIdx, LVIS_SELECTED);
        if (!(selState & LVIS_SELECTED)) {
            ListView_SetItemState(st->hwndList, selIdx, LVIS_SELECTED, LVIS_SELECTED);
        }
    }

    SendMessageW(st->hwndList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(st->hwndList, nullptr, FALSE);
    RefreshTaskQueueData(st);
    InvalidateRect(st->hwndMain, nullptr, FALSE);
}

static void UpdateTaskListColumnWidths(DashboardState* st) {
    if (!st || !st->hwndTaskList) return;
    RECT rc = {};
    GetClientRect(st->hwndTaskList, &rc);
    int total = rc.right - rc.left;
    if (total < 400) return;

    // Summary takes ~half of the visible list width; the other columns share the rest.
    int summaryW = total / 2;
    const int base[8] = {90, 100, 105, 115, 90, 100, 190, 110}; // ID, State, Start, End, Attempts, Timeout, Depends, Queue
    int baseSum = 0;
    for (int i = 0; i < 8; ++i) baseSum += base[i];

    int remaining = total - summaryW;
    int widths[8] = {};
    int assigned = 0;
    for (int i = 0; i < 7; ++i) {
        widths[i] = base[i] * remaining / baseSum;
        if (widths[i] < 60) widths[i] = 60;
        assigned += widths[i];
    }
    widths[7] = remaining - assigned;
    if (widths[7] < 60) widths[7] = 60;

    ListView_SetColumnWidth(st->hwndTaskList, 0, widths[0]);
    ListView_SetColumnWidth(st->hwndTaskList, 1, summaryW);
    for (int i = 1; i < 8; ++i) {
        ListView_SetColumnWidth(st->hwndTaskList, i + 1, widths[i]);
    }
}

static void LayoutControls(DashboardState* st, int width, int height) {
    if (!st) return;
    int margin = 10;

    st->rcKpiArea = {margin, kHeaderKpiTop, width - margin, kHeaderKpiBottom};

    int usageOffset = 0;
    if (st->showUsagePanel) {
        int usageTop = kHeaderKpiBottom + 2;
        st->rcUsagePanel = {margin, usageTop, width - margin, usageTop + kUsagePanelH};
        usageOffset = kUsagePanelH + 8;
        if (st->hwndUsagePanel) {
            MoveWindow(st->hwndUsagePanel, margin, usageTop, (std::max)(200, width - margin * 2), kUsagePanelH, TRUE);
            ShowWindow(st->hwndUsagePanel, SW_SHOW);
        }
    } else {
        st->rcUsagePanel = {};
        if (st->hwndUsagePanel) ShowWindow(st->hwndUsagePanel, SW_HIDE);
    }

    // Toolbar buttons flow into multiple rows when the window is too narrow.
    struct ToolbarButton { HWND hwnd; int w; };
    const ToolbarButton buttons[] = {
        {st->hwndBtnViewTasks, 150},
        {st->hwndBtnStart, 160},
        {st->hwndBtnSettings, 90},
        {st->hwndBtnKillAll, 74},
        {st->hwndBtnGridView, 28},
        {st->hwndBtnToggleList, 28},
        {st->hwndBtnToggleTerm, 30},
        {st->hwndBtnUsage, 28},
        {st->hwndBtnHideTray, 28},
        {st->hwndClock, 80}
    };
    const int gap = 7;
    int xCur = margin;
    int yBar = kToolbarTop + usageOffset;
    for (const auto& b : buttons) {
        if (!b.hwnd) continue;
        if (xCur > margin && xCur + b.w > width - margin) {
            xCur = margin;
            yBar += kToolbarH + 4;
        }
        MoveWindow(b.hwnd, xCur, yBar, b.w, kToolbarH, TRUE);
        xCur += b.w + gap;
    }
    st->processListTop = yBar + kToolbarH + 6;

    int topList = st->processListTop;
    if (st->hideProcessList) {
        ShowWindow(st->hwndList, SW_HIDE);
        if (st->hwndTaskList) ShowWindow(st->hwndTaskList, SW_HIDE);
        st->rcSplitter = {};
        st->rcRowsSplitter = {};
    } else {
        int availVert = height - topList - kStatusBarH - margin;
        int maxListH = (std::max)(kMinListH, availVert - kTabH - 2 - kMinTerminalH);
        int listH = st->processListHeight;
        if (st->hideTerminalGrid) {
            listH = (std::max)(kMinListH, height - topList - kStatusBarH - 4);
        } else {
            if (listH < kMinListH) listH = kMinListH;
            if (listH > maxListH) listH = maxListH;
            st->processListHeight = listH;
        }
        st->lastTotalListHeight = listH;

        if (st->listDisplayMode == ListDisplayMode::TwoRows) {
            int availForRows = listH - kRowSplitterH;
            if (availForRows < kMinRowListH * 2) {
                st->rcRowsSplitter = {};
                if (st->hwndTaskList) ShowWindow(st->hwndTaskList, SW_HIDE);
                if (st->hwndList) {
                    ShowWindow(st->hwndList, SW_SHOW);
                    MoveWindow(st->hwndList, margin, topList, width - margin * 2, listH, TRUE);
                }
            } else {
                int procH = static_cast<int>(availForRows * st->procRowRatio);
                if (procH < kMinRowListH) procH = kMinRowListH;
                if (procH > availForRows - kMinRowListH) procH = availForRows - kMinRowListH;
                int taskH = availForRows - procH;
                if (taskH < kMinRowListH) {
                    taskH = kMinRowListH;
                    procH = availForRows - taskH;
                }
                int splitterTop = topList + procH;
                int taskTop = splitterTop + kRowSplitterH;

                st->rcRowsSplitter = {margin, splitterTop, width - margin, splitterTop + kRowSplitterH};

                if (st->hwndList) {
                    ShowWindow(st->hwndList, SW_SHOW);
                    MoveWindow(st->hwndList, margin, topList, width - margin * 2, procH, TRUE);
                }
                if (st->hwndTaskList) {
                    ShowWindow(st->hwndTaskList, SW_SHOW);
                    MoveWindow(st->hwndTaskList, margin, taskTop, width - margin * 2, taskH, TRUE);
                }
            }
        } else if (st->listDisplayMode == ListDisplayMode::ProcessOnly) {
            st->rcRowsSplitter = {};
            if (st->hwndTaskList) ShowWindow(st->hwndTaskList, SW_HIDE);
            if (st->hwndList) {
                ShowWindow(st->hwndList, SW_SHOW);
                MoveWindow(st->hwndList, margin, topList, width - margin * 2, listH, TRUE);
            }
        } else { // ListDisplayMode::TaskOnly
            st->rcRowsSplitter = {};
            if (st->hwndList) ShowWindow(st->hwndList, SW_HIDE);
            if (st->hwndTaskList) {
                ShowWindow(st->hwndTaskList, SW_SHOW);
                MoveWindow(st->hwndTaskList, margin, topList, width - margin * 2, listH, TRUE);
            }
        }

        st->rcSplitter = {margin, topList + listH + 1, width - margin, topList + listH + 1 + kSplitterH};
        if (st->hideTerminalGrid) st->rcSplitter = {};
    }

    int tabTop = st->rcSplitter.bottom > 0 ? st->rcSplitter.bottom + 1 : topList;
    if (st->hwndTab) {
        if (st->hideTerminalGrid) {
            ShowWindow(st->hwndTab, SW_HIDE);
        } else {
            ShowWindow(st->hwndTab, SW_SHOW);
            MoveWindow(st->hwndTab, margin, tabTop, width - margin * 2, kTabH, TRUE);
        }
    }

    if (st->hideTerminalGrid) {
        st->rcTerminalArea = {};
    } else {
        // Middle/Bottom Area: dedicated to embedded Direct2D TerminalView windows
        int termTop = tabTop + kTabH + 2;
        int termBottom = (std::max)(termTop + kMinTerminalH, height - kStatusBarH - 4);
        st->rcTerminalArea = {margin, termTop, width - margin, termBottom};
    }

    // Bottom Status Bar
    st->rcStatusBar = {0, height - kStatusBarH, width, height};

    UpdateTaskListColumnWidths(st);
    LayoutTerminalViews(st);
}

static void CopyTextToClipboard(HWND hwnd, const std::wstring& text) {
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hg) {
        memcpy(GlobalLock(hg), text.c_str(), bytes);
        GlobalUnlock(hg);
        SetClipboardData(CF_UNICODETEXT, hg);
    }
    CloseClipboard();
}

struct TaskDetailsDlgState {
    DashboardState* dash = nullptr;
    int procId = 0;
    ProcessSnapshot snap;
    std::wstring fullOutput;
    HFONT hFontUi = nullptr;
    HFONT hFontBold = nullptr;
    HFONT hFontMono = nullptr;
    HWND hwndEditCmd = nullptr;
    HWND hwndLblInfo = nullptr;
    HWND hwndTree = nullptr;
    HWND hwndEditInfo = nullptr;
    HWND hwndBtnCopyCmd = nullptr;
    HWND hwndBtnCopyJson = nullptr;
    HWND hwndBtnToggleMode = nullptr;
    HWND hwndBtnOpenLog = nullptr;
    HWND hwndBtnOpenStream = nullptr;
    HWND hwndBtnClose = nullptr;

    bool isJsonMode = false;
    bool showTreeView = true;
    nlohmann::json rootJson;
    std::unordered_map<HTREEITEM, std::string> nodeJsonMap;
    std::unordered_map<HTREEITEM, std::string> nodePathMap;
};

static void PopulateJsonTreeRecursive(
    HWND hTree,
    HTREEITEM hParent,
    const nlohmann::json& j,
    const std::string& keyName,
    const std::string& currentPath,
    std::unordered_map<HTREEITEM, std::string>& nodeJsonMap,
    std::unordered_map<HTREEITEM, std::string>& nodePathMap,
    int depth = 0
) {
    TVINSERTSTRUCTW tvis = {};
    tvis.hParent = hParent;
    tvis.hInsertAfter = TVI_LAST;
    tvis.item.mask = TVIF_TEXT;

    std::wstring label;
    if (j.is_object()) {
        std::wstringstream ss;
        if (!keyName.empty()) {
            ss << Utf8ToWide(keyName) << L": ";
        }
        ss << L"{ ... } (" << j.size() << Tr(L" 個のキー", L" keys", L" claves") << L")";
        label = ss.str();
    } else if (j.is_array()) {
        std::wstringstream ss;
        if (!keyName.empty()) {
            ss << Utf8ToWide(keyName) << L": ";
        }
        ss << L"[ ... ] (" << j.size() << Tr(L" 要素", L" items", L" elementos") << L")";
        label = ss.str();
    } else if (j.is_string()) {
        std::string s = j.get<std::string>();
        if (s.size() > 70) s = s.substr(0, 67) + "...";
        label = (keyName.empty() ? L"" : Utf8ToWide(keyName) + L": ") + L"\"" + Utf8ToWide(s) + L"\"";
    } else if (j.is_number()) {
        label = (keyName.empty() ? L"" : Utf8ToWide(keyName) + L": ") + Utf8ToWide(j.dump());
    } else if (j.is_boolean()) {
        label = (keyName.empty() ? L"" : Utf8ToWide(keyName) + L": ") + (j.get<bool>() ? L"true" : L"false");
    } else if (j.is_null()) {
        label = (keyName.empty() ? L"" : Utf8ToWide(keyName) + L": ") + L"null";
    }

    tvis.item.pszText = const_cast<wchar_t*>(label.c_str());
    tvis.item.cchTextMax = static_cast<int>(label.size());
    HTREEITEM hItem = TreeView_InsertItem(hTree, &tvis);
    if (!hItem) return;

    try {
        nodeJsonMap[hItem] = j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
    } catch (...) {
        nodeJsonMap[hItem] = "(JSON serialization error)";
    }
    nodePathMap[hItem] = currentPath;

    if (depth < 2) {
        TreeView_Expand(hTree, hItem, TVE_EXPAND);
    }

    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            std::string subPath = currentPath.empty() ? it.key() : (currentPath + "." + it.key());
            PopulateJsonTreeRecursive(hTree, hItem, it.value(), it.key(), subPath, nodeJsonMap, nodePathMap, depth + 1);
        }
    } else if (j.is_array()) {
        size_t idx = 0;
        for (const auto& item : j) {
            std::string itemKey = "[" + std::to_string(idx) + "]";
            std::string subPath = currentPath + itemKey;
            PopulateJsonTreeRecursive(hTree, hItem, item, itemKey, subPath, nodeJsonMap, nodePathMap, depth + 1);
            idx++;
        }
    }
}

static LRESULT CALLBACK TaskDetailsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ds = reinterpret_cast<TaskDetailsDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ds = reinterpret_cast<TaskDetailsDlgState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ds));

            HDC hdc = GetDC(hwnd);
            int logPixelsY = GetDeviceCaps(hdc, LOGPIXELSY);
            ReleaseDC(hwnd, hdc);
            int fontSize = -MulDiv(9, logPixelsY, 72);
            ds->hFontMono = CreateFontW(fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                FIXED_PITCH | FF_MODERN, L"Consolas");
            if (!ds->hFontMono) {
                ds->hFontMono = CreateFontW(fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    FIXED_PITCH | FF_MODERN, L"Courier New");
            }

            HWND hLblCmd = CreateWindowExW(0, L"STATIC", Tr(L"コマンドライン:", L"Command Line:", L"Línea de comandos:"),
                WS_CHILD | WS_VISIBLE, 14, 10, 400, 18, hwnd, nullptr, nullptr, nullptr);

            std::wstring cmdW = Utf8ToWide(ds->snap.commandLine);
            ds->hwndEditCmd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", cmdW.c_str(),
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                14, 30, 890, 60, hwnd, (HMENU)2001, nullptr, nullptr);

            ds->hwndLblInfo = CreateWindowExW(0, L"STATIC",
                ds->isJsonMode ? Tr(L"JSON ツリー / エディタ (ノード選択で展開・詳細表示):", L"JSON Tree / Editor (select node to view/edit):", L"Árbol / Editor JSON:")
                               : Tr(L"タスク詳細・出力:", L"Task Details & Output Preview:", L"Detalles de tarea y salida:"),
                WS_CHILD | WS_VISIBLE, 14, 96, 890, 20, hwnd, nullptr, nullptr, nullptr);

            ds->hwndTree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                WS_CHILD | (ds->isJsonMode && ds->showTreeView ? WS_VISIBLE : 0) |
                TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
                14, 122, 390, 470, hwnd, (HMENU)2008, nullptr, nullptr);
            TreeView_SetBkColor(ds->hwndTree, g_theme.panel);
            TreeView_SetTextColor(ds->hwndTree, g_theme.listText);
            TreeView_SetLineColor(ds->hwndTree, g_theme.border);

            ds->hwndEditInfo = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                ds->isJsonMode ? 414 : 14, 122, ds->isJsonMode ? 490 : 890, 470, hwnd, (HMENU)2002, nullptr, nullptr);

            if (ds->isJsonMode) {
                SendMessageW(ds->hwndTree, WM_SETREDRAW, FALSE, 0);
                PopulateJsonTreeRecursive(ds->hwndTree, TVI_ROOT, ds->rootJson, "root", "root", ds->nodeJsonMap, ds->nodePathMap, 0);
                SendMessageW(ds->hwndTree, WM_SETREDRAW, TRUE, 0);
                InvalidateRect(ds->hwndTree, nullptr, TRUE);

                std::wstring dumpW = NormalizeCrLfWide(ds->rootJson.dump(2, ' ', false, nlohmann::json::error_handler_t::replace));
                SetWindowTextW(ds->hwndEditInfo, dumpW.c_str());
            } else {
                std::wstringstream ss;
                ss << Tr(L"ID              : ", L"ID              : ", L"ID              : ") << L"#" << ds->snap.id << L"\r\n"
                   << Tr(L"名前            : ", L"Name            : ", L"Nombre          : ") << Utf8ToWide(ds->snap.name) << L"\r\n"
                   << Tr(L"状態            : ", L"Status          : ", L"Estado          : ") << StateLabelWide(ds->snap.state, ds->snap.idleMs >= 10000);
                if (ds->snap.state != ProcState::Running && ds->snap.exitCode != STILL_ACTIVE) {
                    ss << Tr(L" (終了コード: ", L" (Exit Code: ", L" (Código de salida: ") << ds->snap.exitCode << L")";
                }
                ss << L"\r\n"
                   << Tr(L"コマンドライン  : ", L"Command Line    : ", L"Línea de comandos: ") << Utf8ToWide(ds->snap.commandLine) << L"\r\n"
                   << Tr(L"作業ディレクトリ: ", L"Working Dir     : ", L"Directorio      : ")
                   << (ds->snap.workingDir.empty() ? Tr(L"(既定)", L"(default)", L"(predeterminado)") : Utf8ToWide(ds->snap.workingDir)) << L"\r\n"
                   << Tr(L"ルート PID      : ", L"Root PID        : ", L"PID raíz        : ") << ds->snap.rootPid
                   << L" (" << ds->snap.treeProcessCount << Tr(L" プロセス (Job Object)", L" processes in Job Object", L" procesos en Job Object") << L")\r\n"
                   << Tr(L"メモリ使用量    : ", L"Memory Usage    : ", L"Uso de memoria  : ") << Utf8ToWide(FormatBytes(ds->snap.treeMemoryBytes)) << L"\r\n"
                   << Tr(L"開始時刻        : ", L"Start Time      : ", L"Hora de inicio  : ") << Utf8ToWide(ds->snap.startTimeStr) << L"\r\n"
                   << Tr(L"終了時刻        : ", L"End Time        : ", L"Hora de fin     : ")
                   << (ds->snap.endTimeStr.empty() ? Tr(L"(実行中)", L"(running)", L"(en ejecución)") : Utf8ToWide(ds->snap.endTimeStr)) << L"\r\n";
                if (ds->snap.state == ProcState::Running && ds->snap.timeoutSec > 0) {
                    int64_t remainingMs = static_cast<int64_t>(ds->snap.timeoutSec) * 1000 - static_cast<int64_t>(ds->snap.elapsedMs);
                    int64_t remSec = (remainingMs + 999) / 1000;
                    if (remSec < 0) remSec = 0;
                    ss << Tr(L"残り時間        : ", L"Time Remaining  : ", L"Tiempo restante : ") << FormatRemainingTime(remSec)
                       << L" (" << Tr(L"タイムアウト: ", L"Timeout: ", L"Tiempo límite: ") << ds->snap.timeoutSec << L"s)\r\n";
                }
                ss << Tr(L"稼働時間        : ", L"Uptime          : ", L"Tiempo activo   : ") << Utf8ToWide(FormatUptimeHMS(ds->snap.elapsedMs))
                   << L" (" << Utf8ToWide(FormatDurationSec(ds->snap.elapsedMs)) << L")\r\n"
                   << Tr(L"CPU時間         : ", L"CPU Time        : ", L"Tiempo de CPU   : ") << Utf8ToWide(FormatUptimeHMS(ds->snap.cpuTimeMs)) << L"\r\n"
                   << Tr(L"ログファイル    : ", L"Log File        : ", L"Archivo de log  : ") << Utf8ToWide(ds->snap.logFilePath) << L"\r\n"
                   << Tr(L"完了ファイル    : ", L"Done File       : ", L"Archivo .done   : ") << Utf8ToWide(ds->snap.doneFilePath) << L"\r\n"
                   << L"--------------------------------------------------------------------------------\r\n"
                   << ds->fullOutput;
                SetWindowTextW(ds->hwndEditInfo, ss.str().c_str());
            }

            ds->hwndBtnCopyCmd = CreateWindowExW(0, L"BUTTON", Tr(L"コマンドラインをコピー", L"Copy Command Line", L"Copiar línea de comandos"),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 14, 600, 180, 30, hwnd, (HMENU)2003, nullptr, nullptr);
            ds->hwndBtnCopyJson = CreateWindowExW(0, L"BUTTON", Tr(L"JSONをコピー", L"Copy JSON", L"Copiar JSON"),
                WS_CHILD | (ds->isJsonMode ? WS_VISIBLE : 0) | BS_OWNERDRAW, 202, 600, 130, 30, hwnd, (HMENU)2006, nullptr, nullptr);
            ds->hwndBtnToggleMode = CreateWindowExW(0, L"BUTTON",
                ds->showTreeView ? Tr(L"生テキスト表示に切替", L"Switch to Raw Text", L"Cambiar a texto")
                                 : Tr(L"JSONツリー表示に切替", L"Switch to JSON Tree", L"Cambiar a árbol JSON"),
                WS_CHILD | (ds->isJsonMode ? WS_VISIBLE : 0) | BS_OWNERDRAW, 340, 600, 175, 30, hwnd, (HMENU)2005, nullptr, nullptr);
            ds->hwndBtnOpenLog = CreateWindowExW(0, L"BUTTON", Tr(L"ログファイルを開く", L"Open Log File", L"Abrir archivo de registro"),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 523, 600, 160, 30, hwnd, (HMENU)2004, nullptr, nullptr);
            ds->hwndBtnOpenStream = CreateWindowExW(0, L"BUTTON", Tr(L"ストリーム (jsonl) を開く", L"Open Stream File", L"Abrir flujo"),
                WS_CHILD | (!ds->snap.streamFilePath.empty() ? WS_VISIBLE : 0) | BS_OWNERDRAW, 691, 600, 160, 30, hwnd, (HMENU)2007, nullptr, nullptr);
            ds->hwndBtnClose = CreateWindowExW(0, L"BUTTON", Tr(L"閉じる", L"Close", L"Cerrar"),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 860, 600, 95, 30, hwnd, (HMENU)IDOK, nullptr, nullptr);

            SendMessageW(hLblCmd, WM_SETFONT, (WPARAM)ds->hFontBold, TRUE);
            SendMessageW(ds->hwndLblInfo, WM_SETFONT, (WPARAM)ds->hFontBold, TRUE);
            HWND uiCtrls[] = {ds->hwndEditCmd, ds->hwndTree, ds->hwndBtnCopyCmd, ds->hwndBtnCopyJson, ds->hwndBtnToggleMode, ds->hwndBtnOpenLog, ds->hwndBtnOpenStream, ds->hwndBtnClose};
            for (HWND h : uiCtrls) {
                if (h) SendMessageW(h, WM_SETFONT, (WPARAM)ds->hFontUi, TRUE);
            }
            if (ds->hFontMono) {
                SendMessageW(ds->hwndEditInfo, WM_SETFONT, (WPARAM)ds->hFontMono, TRUE);
            }

            ApplyNativeThemeToWindow(hwnd);
            return 0;
        }
        case WM_NOTIFY: {
            auto* pnmh = reinterpret_cast<NMHDR*>(lParam);
            if (ds && pnmh && ds->hwndTree && pnmh->hwndFrom == ds->hwndTree) {
                if (pnmh->code == TVN_SELCHANGEDW) {
                    auto* pnmtv = reinterpret_cast<NMTREEVIEWW*>(lParam);
                    HTREEITEM hSel = pnmtv->itemNew.hItem;
                    if (hSel) {
                        auto it = ds->nodeJsonMap.find(hSel);
                        if (it != ds->nodeJsonMap.end()) {
                            std::wstring jsonW = NormalizeCrLfWide(it->second);
                            SetWindowTextW(ds->hwndEditInfo, jsonW.c_str());
                        }
                        auto itP = ds->nodePathMap.find(hSel);
                        if (itP != ds->nodePathMap.end() && ds->hwndLblInfo) {
                            std::wstring pathLbl = std::wstring(Tr(L"JSON パス: ", L"JSON Path: ", L"Ruta JSON: ")) + Utf8ToWide(itP->second);
                            SetWindowTextW(ds->hwndLblInfo, pathLbl.c_str());
                        }
                    }
                }
            }
            break;
        }
        case WM_SIZE: {
            if (!ds) return 0;
            int w = LOWORD(lParam);
            int h = HIWORD(lParam);
            int pad = 14;
            int editW = (std::max)(200, w - pad * 2);
            MoveWindow(ds->hwndEditCmd, pad, 30, editW, 60, TRUE);
            if (ds->hwndLblInfo) {
                MoveWindow(ds->hwndLblInfo, pad, 96, editW, 20, TRUE);
            }
            int btnY = (std::max)(220, h - 42);
            int mainY = 122;
            int mainH = (std::max)(100, btnY - mainY - 8);

            if (ds->isJsonMode && ds->showTreeView && ds->hwndTree) {
                int treeW = (editW - 12) * 44 / 100;
                int jsonW = editW - treeW - 12;
                ShowWindow(ds->hwndTree, SW_SHOW);
                MoveWindow(ds->hwndTree, pad, mainY, treeW, mainH, TRUE);
                MoveWindow(ds->hwndEditInfo, pad + treeW + 12, mainY, jsonW, mainH, TRUE);
            } else {
                if (ds->hwndTree) ShowWindow(ds->hwndTree, SW_HIDE);
                MoveWindow(ds->hwndEditInfo, pad, mainY, editW, mainH, TRUE);
            }

            int curX = pad;
            if (ds->hwndBtnCopyCmd) {
                MoveWindow(ds->hwndBtnCopyCmd, curX, btnY, 170, 30, TRUE);
                curX += 176;
            }
            if (ds->hwndBtnCopyJson) {
                if (ds->isJsonMode) {
                    ShowWindow(ds->hwndBtnCopyJson, SW_SHOW);
                    MoveWindow(ds->hwndBtnCopyJson, curX, btnY, 120, 30, TRUE);
                    curX += 126;
                } else {
                    ShowWindow(ds->hwndBtnCopyJson, SW_HIDE);
                }
            }
            if (ds->hwndBtnToggleMode) {
                if (ds->isJsonMode) {
                    ShowWindow(ds->hwndBtnToggleMode, SW_SHOW);
                    MoveWindow(ds->hwndBtnToggleMode, curX, btnY, 175, 30, TRUE);
                    curX += 181;
                } else {
                    ShowWindow(ds->hwndBtnToggleMode, SW_HIDE);
                }
            }
            if (ds->hwndBtnOpenLog) {
                MoveWindow(ds->hwndBtnOpenLog, curX, btnY, 150, 30, TRUE);
                curX += 156;
            }
            if (ds->hwndBtnOpenStream) {
                if (!ds->snap.streamFilePath.empty()) {
                    ShowWindow(ds->hwndBtnOpenStream, SW_SHOW);
                    MoveWindow(ds->hwndBtnOpenStream, curX, btnY, 150, 30, TRUE);
                    curX += 156;
                } else {
                    ShowWindow(ds->hwndBtnOpenStream, SW_HIDE);
                }
            }
            if (ds->hwndBtnClose) {
                MoveWindow(ds->hwndBtnClose, (std::max)(curX + 10, w - pad - 100), btnY, 100, 30, TRUE);
            }
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == 2003 && ds) {
                CopyTextToClipboard(hwnd, Utf8ToWide(ds->snap.commandLine));
            } else if (id == 2004 && ds) {
                if (!ds->snap.logFilePath.empty()) {
                    std::wstring logW = Utf8ToWide(ds->snap.logFilePath);
                    ShellExecuteW(hwnd, L"open", L"notepad.exe", logW.c_str(), nullptr, SW_SHOWNORMAL);
                }
            } else if (id == 2005 && ds) {
                if (ds->isJsonMode) {
                    ds->showTreeView = !ds->showTreeView;
                    if (ds->showTreeView) {
                        SetWindowTextW(ds->hwndBtnToggleMode, Tr(L"生テキスト表示に切替", L"Switch to Raw Text", L"Cambiar a texto"));
                        HTREEITEM hSel = ds->hwndTree ? TreeView_GetSelection(ds->hwndTree) : nullptr;
                        if (hSel && ds->nodeJsonMap.count(hSel)) {
                            SetWindowTextW(ds->hwndEditInfo, NormalizeCrLfWide(ds->nodeJsonMap[hSel]).c_str());
                        } else {
                            SetWindowTextW(ds->hwndEditInfo, NormalizeCrLfWide(ds->rootJson.dump(2, ' ', false, nlohmann::json::error_handler_t::replace)).c_str());
                        }
                    } else {
                        SetWindowTextW(ds->hwndBtnToggleMode, Tr(L"JSONツリー表示に切替", L"Switch to JSON Tree", L"Cambiar a árbol JSON"));
                        SetWindowTextW(ds->hwndEditInfo, ds->fullOutput.c_str());
                    }
                    RECT rc;
                    GetClientRect(hwnd, &rc);
                    SendMessageW(hwnd, WM_SIZE, 0, MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top));
                }
            } else if (id == 2006 && ds) {
                HTREEITEM hSel = (ds->hwndTree && ds->showTreeView) ? TreeView_GetSelection(ds->hwndTree) : nullptr;
                if (hSel && ds->nodeJsonMap.count(hSel)) {
                    CopyTextToClipboard(hwnd, Utf8ToWide(ds->nodeJsonMap[hSel]));
                } else if (!ds->rootJson.is_null()) {
                    CopyTextToClipboard(hwnd, Utf8ToWide(ds->rootJson.dump(2, ' ', false, nlohmann::json::error_handler_t::replace)));
                }
            } else if (id == 2007 && ds) {
                if (!ds->snap.streamFilePath.empty()) {
                    std::wstring streamW = Utf8ToWide(ds->snap.streamFilePath);
                    ShellExecuteW(hwnd, L"open", L"notepad.exe", streamW.c_str(), nullptr, SW_SHOWNORMAL);
                }
            } else if (id == IDOK || id == IDCANCEL) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!dis || !ds) break;
            bool pressed = (dis->itemState & ODS_SELECTED) != 0;
            COLORREF bg = pressed ? g_theme.border : g_theme.panel;
            HBRUSH bgBrush = CreateSolidBrush(bg);
            FillRect(dis->hDC, &dis->rcItem, bgBrush);
            DeleteObject(bgBrush);
            HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
            HGDIOBJ oldPen = SelectObject(dis->hDC, borderPen);
            HGDIOBJ oldBrush = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
            Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
            SelectObject(dis->hDC, oldBrush);
            SelectObject(dis->hDC, oldPen);
            DeleteObject(borderPen);

            if (dis->itemState & ODS_FOCUS) {
                RECT rcFocus = dis->rcItem;
                InflateRect(&rcFocus, -2, -2);
                DrawFocusRect(dis->hDC, &rcFocus);
            }

            wchar_t text[256] = {};
            GetWindowTextW(dis->hwndItem, text, 256);
            RECT rc = dis->rcItem;
            InflateRect(&rc, -4, 0);
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, g_theme.text);
            HFONT oldFont = static_cast<HFONT>(SelectObject(dis->hDC, ds->hFontUi));
            DrawTextW(dis->hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dis->hDC, oldFont);
            return TRUE;
        }
        case WM_ERASEBKGND: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, g_dialogBgBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, g_theme.text);
            SetBkColor(hdc, g_theme.panel);
            return reinterpret_cast<LRESULT>(g_dialogBgBrush);
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            if (ds) {
                if (ds->hFontMono) DeleteObject(ds->hFontMono);
                delete ds;
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowTaskDetailsDialog(DashboardState* st, int procId) {
    if (!st || !st->pm || procId <= 0) return;
    ProcessSnapshot snap;
    if (!st->pm->GetSnapshot(procId, snap)) return;

    static bool regDone = false;
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!regDone) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = TaskDetailsWndProc;
        wc.hInstance = hInst;
        wc.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"ProcessCommanderTaskDetailsWnd";
        RegisterClassExW(&wc);
        regDone = true;
    }

    auto* ds = new TaskDetailsDlgState();
    ds->dash = st;
    ds->procId = procId;
    ds->snap = snap;
    ds->hFontUi = st->hFontUi;
    ds->hFontBold = st->hFontUiBold;

    std::string outUtf8;
    st->pm->GetOutput(procId, true, 200, outUtf8);
    ds->fullOutput = NormalizeCrLfWide(outUtf8);

    bool isStream = snap.jsonStream || !snap.agentType.empty() || snap.streamEventCount > 0;
    if (isStream) {
        nlohmann::json root = nlohmann::json::object();

        nlohmann::json proc = nlohmann::json::object();
        proc["id"] = snap.id;
        proc["name"] = snap.name;
        proc["agentType"] = snap.agentType;
        proc["state"] = ProcStateToString(snap.state);
        proc["exitCode"] = snap.exitCode;
        proc["commandLine"] = snap.commandLine;
        proc["workingDir"] = snap.workingDir;
        proc["rootPid"] = snap.rootPid;
        proc["treeProcessCount"] = snap.treeProcessCount;
        proc["memoryBytes"] = snap.treeMemoryBytes;
        proc["startTime"] = snap.startTimeStr;
        proc["endTime"] = snap.endTimeStr;
        proc["elapsedMs"] = snap.elapsedMs;
        proc["cpuTimeMs"] = snap.cpuTimeMs;
        proc["logFilePath"] = snap.logFilePath;
        proc["streamFilePath"] = snap.streamFilePath;
        proc["jsonStream"] = snap.jsonStream;
        if (snap.state == ProcState::Running && snap.timeoutSec > 0) {
            int64_t remainingMs = static_cast<int64_t>(snap.timeoutSec) * 1000 - static_cast<int64_t>(snap.elapsedMs);
            int64_t remSec = (remainingMs + 999) / 1000;
            if (remSec < 0) remSec = 0;
            proc["remainingSec"] = remSec;
            proc["remainingTime"] = WideToUtf8(FormatRemainingTime(remSec));
            proc["timeoutSec"] = snap.timeoutSec;
        }
        root["process"] = proc;

        std::vector<LlmStreamEvent> events;
        LlmAgentState agStateObj;
        if (st->pm) {
            st->pm->GetAgentStream(procId, 0, "", 0, events, agStateObj);
        }

        std::string modelForState = agStateObj.model;
        if (modelForState.empty()) {
            for (const auto& ev : events) {
                if (!ev.model.empty()) { modelForState = ev.model; break; }
            }
        }
        nlohmann::json agState = nlohmann::json::object();
        agState["agentType"] = snap.agentType.empty() ? agStateObj.agentType : snap.agentType;
        agState["model"] = modelForState;
        agState["status"] = agStateObj.status.empty() ? snap.agentStatus : agStateObj.status;
        agState["currentTool"] = agStateObj.currentTool.empty() ? snap.currentTool : agStateObj.currentTool;
        agState["currentToolAction"] = agStateObj.currentToolAction.empty() ? snap.currentToolAction : agStateObj.currentToolAction;
        agState["supervisorGoal"] = agStateObj.supervisorGoal.empty() ? snap.supervisorGoal : agStateObj.supervisorGoal;
        agState["supervisorNotes"] = agStateObj.supervisorNotes.empty() ? snap.supervisorNotes : agStateObj.supervisorNotes;
        agState["totalTokens"] = agStateObj.totalTokens;
        agState["costUsd"] = agStateObj.costUsd;
        agState["stepCount"] = agStateObj.stepCount;
        agState["lastThought"] = agStateObj.lastThought.empty() ? snap.lastThought : agStateObj.lastThought;
        agState["lastMessage"] = agStateObj.lastMessage.empty() ? snap.lastMessage : agStateObj.lastMessage;
        root["agentState"] = agState;

        nlohmann::json evArray = nlohmann::json::array();
        for (const auto& ev : events) {
            nlohmann::json evObj = nlohmann::json::object();
            evObj["seq"] = ev.seq;
            evObj["timestamp"] = ev.timestamp;
            evObj["type"] = ev.type;
            evObj["agentType"] = ev.agentType;
            if (!ev.model.empty()) evObj["model"] = ev.model;
            if (!ev.toolName.empty()) evObj["toolName"] = ev.toolName;
            if (!ev.toolAction.empty()) evObj["toolAction"] = ev.toolAction;
            if (!ev.toolInput.empty()) {
                auto parsedInput = nlohmann::json::parse(ev.toolInput, nullptr, false);
                if (!parsedInput.is_discarded()) {
                    evObj["toolInput"] = parsedInput;
                } else {
                    evObj["toolInput"] = ev.toolInput;
                }
            }
            if (!ev.content.empty()) {
                evObj["content"] = ev.content;
            }
            if (ev.totalTokens > 0) evObj["totalTokens"] = ev.totalTokens;
            if (ev.costUsd > 0.0) evObj["costUsd"] = ev.costUsd;
            if (!ev.rawJson.empty()) {
                auto parsedRaw = nlohmann::json::parse(ev.rawJson, nullptr, false);
                if (!parsedRaw.is_discarded()) {
                    evObj["rawEvent"] = parsedRaw;
                }
            }
            evArray.push_back(evObj);
        }
        root["streamEvents"] = evArray;

        ds->rootJson = std::move(root);
        ds->isJsonMode = true;
        ds->showTreeView = true;
    } else {
        std::string trimmed = outUtf8;
        size_t first = trimmed.find_first_not_of(" \t\r\n");
        if (first != std::string::npos && (trimmed[first] == '{' || trimmed[first] == '[')) {
            try {
                auto j = nlohmann::json::parse(trimmed, nullptr, false);
                if (!j.is_discarded() && (j.is_object() || j.is_array())) {
                    ds->rootJson = std::move(j);
                    ds->isJsonMode = true;
                    ds->showTreeView = true;
                }
            } catch (...) {}
        }
    }

    std::wstring title = std::wstring(Tr(L"タスク詳細", L"Task Details", L"Detalles de tarea")) + L" — #" + std::to_wstring(snap.id) + L" [" + Utf8ToWide(snap.name) + L"]";
    if (ds->isJsonMode) {
        title += L" [JSON Tree Editor]";
    }
    RECT rcParent = {};
    GetWindowRect(st->hwndMain, &rcParent);
    int dlgW = ds->isJsonMode ? 960 : 775;
    int dlgH = ds->isJsonMode ? 680 : 550;
    int x = rcParent.left + ((rcParent.right - rcParent.left) - dlgW) / 2;
    int y = rcParent.top + ((rcParent.bottom - rcParent.top) - dlgH) / 2;

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"ProcessCommanderTaskDetailsWnd",
        title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        x, y, dlgW, dlgH,
        st->hwndMain, nullptr, hInst, ds
    );
    if (hDlg) {
        ShowWindow(hDlg, SW_SHOW);
        SetForegroundWindow(hDlg);
    } else {
        delete ds;
    }
}

struct LlmStreamDlgState {
    DashboardState* dash = nullptr;
    int procId = 0;
    ProcessSnapshot snap;
    std::vector<LlmStreamEvent> events;
    LlmAgentState agentState;
    HFONT hFontUi = nullptr;
    HFONT hFontBold = nullptr;
    HFONT hFontMono = nullptr;
    HWND hwndHeader = nullptr;
    HWND hwndList = nullptr;
    HWND hwndEditPreview = nullptr;
    HWND hwndBtnRefresh = nullptr;
    HWND hwndBtnOpenStream = nullptr;
    HWND hwndBtnClose = nullptr;
    HWND hwndAgentIcon = nullptr;
    HWND hwndTooltip = nullptr;
    UINT_PTR timerId = 0;
    size_t lastEventCount = 0;
    RECT rcUsageGraph = {};
};

struct UsageBar {
    RECT rc = {};
    int eventIndex = -1;
    uint64_t inputTokens = 0;
    uint64_t outputTokens = 0;
    uint64_t thinkingTokens = 0;
    uint64_t cacheTokens = 0;
};

static std::vector<UsageBar> BuildUsageBars(const RECT& rc, const std::vector<LlmStreamEvent>& events) {
    std::vector<UsageBar> bars;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return bars;

    struct Value {
        int index;
        uint64_t in;
        uint64_t out;
        uint64_t think;
        uint64_t cache;
    };
    std::vector<Value> vals;
    uint64_t maxVal = 0;
    for (int i = 0; i < static_cast<int>(events.size()); ++i) {
        const auto& ev = events[i];
        uint64_t in = ev.inputTokens;
        uint64_t out = ev.outputTokens;
        uint64_t think = ev.thinkingTokens;
        uint64_t cache = ev.cacheReadTokens + ev.cacheWriteTokens;
        if (in == 0 && out == 0 && think == 0 && cache == 0 && ev.totalTokens > 0) in = ev.totalTokens;
        uint64_t sum = in + out + think + cache;
        if (sum == 0) continue;
        vals.push_back({i, in, out, think, cache});
        if (sum > maxVal) maxVal = sum;
    }
    if (vals.empty() || maxVal == 0) return bars;

    int left = rc.left + 4;
    int right = rc.right - 4;
    int top = rc.top + 18;
    int bottom = rc.bottom - 4;
    int plotW = right - left;
    int plotH = bottom - top;
    if (plotW <= 0 || plotH <= 0) return bars;

    int n = static_cast<int>(vals.size());
    int slot = plotW / n;
    if (slot < 1) slot = 1;
    for (int k = 0; k < n; ++k) {
        int x0 = left + k * slot;
        int x1 = (k == n - 1) ? right : left + (k + 1) * slot;
        if (x1 <= x0) x1 = x0 + 1;
        UsageBar b;
        b.rc = {x0, top, x1, bottom};
        b.eventIndex = vals[k].index;
        b.inputTokens = vals[k].in;
        b.outputTokens = vals[k].out;
        b.thinkingTokens = vals[k].think;
        b.cacheTokens = vals[k].cache;
        bars.push_back(b);
    }
    return bars;
}

static void DrawUsageGraph(HDC hdc, const RECT& rc, const std::vector<LlmStreamEvent>& events,
                           const LlmStreamEvent* selected, int selectedIndex) {
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;

    HBRUSH bg = CreateSolidBrush(g_theme.panel);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
    HPEN border = CreatePen(PS_SOLID, 1, g_theme.border);
    HGDIOBJ oldPen = SelectObject(hdc, border);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(border);

    auto bars = BuildUsageBars(rc, events);
    SetBkMode(hdc, TRANSPARENT);
    RECT titleRc = {rc.left + 6, rc.top + 2, rc.right - 6, rc.top + 16};
    SetTextColor(hdc, g_theme.muted);
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, GetStockObject(DEFAULT_GUI_FONT)));
    DrawTextW(hdc, L"Usage", -1, &titleRc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (bars.empty()) {
        RECT emptyRc = rc;
        InflateRect(&emptyRc, -6, -4);
        SetTextColor(hdc, g_theme.muted);
        DrawTextW(hdc, L"No usage data", -1, &emptyRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
        return;
    }

    if (selected && !LlmEventUsageText(*selected).empty()) {
        std::wstring usage = Utf8ToWide(LlmEventUsageText(*selected));
        RECT valRc = {rc.left + 50, rc.top + 2, rc.right - 6, rc.top + 16};
        SetTextColor(hdc, g_theme.text);
        DrawTextW(hdc, usage.c_str(), -1, &valRc, DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    int plotTop = rc.top + 18;
    int plotBottom = rc.bottom - 4;
    int plotH = plotBottom - plotTop;
    uint64_t maxVal = 1;
    for (const auto& b : bars) {
        uint64_t s = b.inputTokens + b.outputTokens + b.thinkingTokens + b.cacheTokens;
        if (s > maxVal) maxVal = s;
    }

    HPEN grid = CreatePen(PS_SOLID, 1, g_theme.border);
    HGDIOBJ prevPen = SelectObject(hdc, grid);
    MoveToEx(hdc, rc.left + 4, plotBottom, nullptr);
    LineTo(hdc, rc.right - 4, plotBottom);
    SelectObject(hdc, prevPen);
    DeleteObject(grid);

    for (const auto& b : bars) {
        int h = static_cast<int>((static_cast<double>(b.inputTokens + b.outputTokens + b.thinkingTokens + b.cacheTokens) / maxVal) * plotH);
        if (h < 1) h = 1;
        int totalH = h;
        auto drawSeg = [&](uint64_t v, COLORREF color) {
            if (v == 0 || totalH <= 0) return;
            int sh = static_cast<int>((static_cast<double>(v) / (b.inputTokens + b.outputTokens + b.thinkingTokens + b.cacheTokens)) * h);
            if (sh < 1) sh = 1;
            if (sh > totalH) sh = totalH;
            RECT seg = {b.rc.left, plotBottom - totalH, b.rc.right, plotBottom - totalH + sh};
            if (seg.right <= seg.left) seg.right = seg.left + 1;
            HBRUSH br = CreateSolidBrush(color);
            FillRect(hdc, &seg, br);
            DeleteObject(br);
            totalH -= sh;
        };
        drawSeg(b.cacheTokens, RGB(0x5C, 0x6C, 0x8C));
        drawSeg(b.thinkingTokens, RGB(0xB4, 0x7A, 0xFF));
        drawSeg(b.outputTokens, RGB(0x3D, 0xD5, 0x98));
        drawSeg(b.inputTokens, RGB(0x4F, 0x8C, 0xFF));

        if (b.eventIndex == selectedIndex) {
            HPEN selPen = CreatePen(PS_SOLID, 1, g_theme.accent);
            HGDIOBJ p = SelectObject(hdc, selPen);
            HGDIOBJ br = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            Rectangle(hdc, b.rc.left, plotTop, b.rc.right, plotBottom);
            SelectObject(hdc, br);
            SelectObject(hdc, p);
            DeleteObject(selPen);
        }
    }
    SelectObject(hdc, oldFont);
}


static void ReloadStreamDialogData(HWND hwnd, LlmStreamDlgState* ds) {
    if (!ds || !ds->dash || !ds->dash->pm) return;
    ds->dash->pm->GetSnapshot(ds->procId, ds->snap);
    ds->events.clear();
    ds->dash->pm->GetAgentStream(ds->procId, 300, "", 0, ds->events, ds->agentState);

    std::string modelStr = ds->agentState.model;
    if (modelStr.empty()) {
        for (const auto& ev : ds->events) {
            if (!ev.model.empty()) { modelStr = ev.model; break; }
        }
    }
    if (!modelStr.empty()) ds->agentState.model = modelStr;

    std::wstring agentLabel = Utf8ToWide(ds->snap.agentType.empty() ? (ds->agentState.agentType.empty() ? "generic" : ds->agentState.agentType) : ds->snap.agentType);
    std::wstringstream hdr;
    hdr << L"ID: #" << ds->snap.id << L"  |  "
        << Utf8ToWide(ds->snap.name) << L"  |  Agent: ["
        << agentLabel << L"] "
        << StateLabelWide(ds->snap.state, ds->snap.idleMs >= 10000)
        << L" (" << Utf8ToWide(ds->agentState.status) << L")";
    if (!modelStr.empty()) {
        hdr << L"  |  Model: " << Utf8ToWide(modelStr);
    }
    hdr << L"\r\n";
    if (!ds->agentState.currentTool.empty()) {
        hdr << L"Tool: " << Utf8ToWide(ds->agentState.currentTool);
        if (!ds->agentState.currentToolAction.empty()) {
            hdr << L" (" << Utf8ToWide(ds->agentState.currentToolAction) << L")";
        }
        hdr << L"  |  ";
    }
    hdr << L"Steps: " << ds->agentState.stepCount
        << L"  |  Events: " << ds->events.size();
    if (ds->agentState.totalTokens > 0) {
        hdr << L"  |  Tokens: " << ds->agentState.totalTokens;
    }
    if (ds->agentState.costUsd > 0.0) {
        hdr << L"  |  Cost: $" << ds->agentState.costUsd;
    }
    hdr << L"\r\n";
    if (!ds->snap.supervisorGoal.empty()) {
        hdr << Tr(L"目標: ", L"Goal: ", L"Objetivo: ") << Utf8ToWide(ds->snap.supervisorGoal) << L"\r\n";
    }
    if (!ds->snap.supervisorNotes.empty()) {
        hdr << Tr(L"メモ: ", L"Notes: ", L"Notas: ") << Utf8ToWide(ds->snap.supervisorNotes) << L"\r\n";
    }
    if (ds->hwndHeader) {
        SetWindowTextW(ds->hwndHeader, hdr.str().c_str());
    }

    if (ds->hwndList && ds->lastEventCount != ds->events.size()) {
        ds->lastEventCount = ds->events.size();
        SendMessageW(ds->hwndList, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(ds->hwndList);
        for (int i = 0; i < static_cast<int>(ds->events.size()); ++i) {
            const auto& ev = ds->events[i];
            std::wstring seqStr = std::to_wstring(ev.seq);
            std::wstring timeStr = Utf8ToWide(ev.timestamp);
            std::wstring typeStr = Utf8ToWide(ev.type);
            std::wstring toolStr = Utf8ToWide(ev.toolName);
            std::wstring actionStr = Utf8ToWide(ev.toolAction);
            std::string content = ev.content;
            if (content.empty() && !ev.toolInput.empty()) content = ev.toolInput;
            if (content.empty() && !ev.rawJson.empty()) content = ev.rawJson;
            std::wstring contentStr = Utf8ToWide(content);
            if (contentStr.size() > 100) contentStr = contentStr.substr(0, 97) + L"...";
            std::wstring usageStr = Utf8ToWide(LlmEventUsageText(ev));

            LVITEMW lvi = {};
            lvi.mask = LVIF_TEXT | LVIF_PARAM;
            lvi.iItem = i;
            lvi.iSubItem = 0;
            lvi.pszText = seqStr.data();
            lvi.lParam = i;
            ListView_InsertItem(ds->hwndList, &lvi);
            ListView_SetItemText(ds->hwndList, i, 1, timeStr.data());
            ListView_SetItemText(ds->hwndList, i, 2, typeStr.data());
            ListView_SetItemText(ds->hwndList, i, 3, toolStr.data());
            ListView_SetItemText(ds->hwndList, i, 4, actionStr.data());
            ListView_SetItemText(ds->hwndList, i, 5, contentStr.data());
            ListView_SetItemText(ds->hwndList, i, 6, usageStr.data());
        }
        SendMessageW(ds->hwndList, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(ds->hwndList, nullptr, FALSE);
        if (!ds->events.empty()) {
            int lastIdx = static_cast<int>(ds->events.size()) - 1;
            ListView_EnsureVisible(ds->hwndList, lastIdx, FALSE);
        }
    }
    if (ds->rcUsageGraph.right > ds->rcUsageGraph.left) {
        InvalidateRect(hwnd, &ds->rcUsageGraph, FALSE);
    }
}

static LRESULT CALLBACK LlmStreamWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ds = reinterpret_cast<LlmStreamDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ds = reinterpret_cast<LlmStreamDlgState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ds));

            ds->hFontMono = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                        FIXED_PITCH | FF_MODERN, L"Consolas");

            ds->hwndHeader = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                40, 12, 792, 75, hwnd, (HMENU)2100, nullptr, nullptr);
            ds->hwndAgentIcon = CreateWindowExW(0, L"STATIC", L"🤖",
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                12, 12, 24, 24, hwnd, (HMENU)2105, nullptr, nullptr);

            ds->hwndList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                12, 95, 820, 260, hwnd, (HMENU)2101, nullptr, nullptr);
            ListView_SetExtendedListViewStyle(ds->hwndList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
            ListView_SetBkColor(ds->hwndList, g_theme.panel);
            ListView_SetTextBkColor(ds->hwndList, g_theme.panel);
            ListView_SetTextColor(ds->hwndList, g_theme.listText);

            const wchar_t* colTitles[] = {
                Tr(L"#", L"#", L"#"),
                Tr(L"時刻", L"Time", L"Hora"),
                Tr(L"種別", L"Type", L"Tipo"),
                Tr(L"ツール", L"Tool", L"Herramienta"),
                Tr(L"アクション", L"Action", L"Acción"),
                Tr(L"内容 / サマリ", L"Content / Summary", L"Contenido"),
                Tr(L"Usage", L"Usage", L"Uso")
            };
            const int colWidths[] = {50, 80, 80, 110, 140, 235, 150};
            for (int i = 0; i < 7; ++i) {
                LVCOLUMNW lvc = {};
                lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                lvc.pszText = const_cast<wchar_t*>(colTitles[i]);
                lvc.cx = colWidths[i];
                lvc.iSubItem = i;
                ListView_InsertColumn(ds->hwndList, i, &lvc);
            }

            ds->hwndEditPreview = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                12, 365, 820, 180, hwnd, (HMENU)2102, nullptr, nullptr);

            ds->hwndBtnRefresh = CreateWindowExW(0, L"BUTTON", Tr(L"更新", L"Refresh", L"Actualizar"),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 12, 555, 130, 30, hwnd, (HMENU)2103, nullptr, nullptr);
            ds->hwndBtnOpenStream = CreateWindowExW(0, L"BUTTON", Tr(L"ストリームファイル (jsonl) を開く", L"Open Stream File (jsonl)", L"Abrir jsonl"),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 150, 555, 260, 30, hwnd, (HMENU)2104, nullptr, nullptr);
            ds->hwndBtnClose = CreateWindowExW(0, L"BUTTON", Tr(L"閉じる", L"Close", L"Cerrar"),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 702, 555, 110, 30, hwnd, (HMENU)IDOK, nullptr, nullptr);

            if (ds->hFontUi) {
                SendMessageW(ds->hwndHeader, WM_SETFONT, (WPARAM)ds->hFontUi, TRUE);
                if (ds->hwndAgentIcon) SendMessageW(ds->hwndAgentIcon, WM_SETFONT, (WPARAM)ds->hFontUi, TRUE);
                SendMessageW(ds->hwndList, WM_SETFONT, (WPARAM)ds->hFontUi, TRUE);
                SendMessageW(ds->hwndBtnRefresh, WM_SETFONT, (WPARAM)ds->hFontUi, TRUE);
                SendMessageW(ds->hwndBtnOpenStream, WM_SETFONT, (WPARAM)ds->hFontUi, TRUE);
                SendMessageW(ds->hwndBtnClose, WM_SETFONT, (WPARAM)ds->hFontUi, TRUE);
            }
            if (ds->hFontMono) {
                SendMessageW(ds->hwndEditPreview, WM_SETFONT, (WPARAM)ds->hFontMono, TRUE);
            }

            ApplyNativeThemeToWindow(hwnd);
            ReloadStreamDialogData(hwnd, ds);
            if (ds->hwndTooltip && ds->hwndAgentIcon) {
                std::string modelTooltip = ds->agentState.model;
                if (modelTooltip.empty()) {
                    for (const auto& ev : ds->events) {
                        if (!ev.model.empty()) { modelTooltip = ev.model; break; }
                    }
                }
                if (!modelTooltip.empty()) {
                    static wchar_t tipBuf2[512];
                    std::wstring tip = L"Model: " + Utf8ToWide(modelTooltip);
                    wcsncpy_s(tipBuf2, tip.c_str(), _TRUNCATE);
                    TOOLINFOW ti = {};
                    ti.cbSize = sizeof(ti);
                    ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
                    ti.hwnd = hwnd;
                    ti.uId = (UINT_PTR)ds->hwndAgentIcon;
                    ti.hinst = GetModuleHandleW(nullptr);
                    ti.lpszText = tipBuf2;
                    SendMessageW(ds->hwndTooltip, TTM_ADDTOOL, 0, (LPARAM)&ti);
                }
            }
            ds->timerId = SetTimer(hwnd, 1, 1000, nullptr);
            return 0;
        }
        case WM_TIMER: {
            if (ds) {
                ReloadStreamDialogData(hwnd, ds);
                if (ds->hwndTooltip && ds->hwndAgentIcon) {
                    std::string modelTooltip = ds->agentState.model;
                    if (modelTooltip.empty()) {
                        for (const auto& ev : ds->events) {
                            if (!ev.model.empty()) { modelTooltip = ev.model; break; }
                        }
                    }
                    if (!modelTooltip.empty()) {
                        static wchar_t tipBuf3[512];
                        std::wstring tip = L"Model: " + Utf8ToWide(modelTooltip);
                        wcsncpy_s(tipBuf3, tip.c_str(), _TRUNCATE);
                        TOOLINFOW ti = {};
                        ti.cbSize = sizeof(ti);
                        ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
                        ti.hwnd = hwnd;
                        ti.uId = (UINT_PTR)ds->hwndAgentIcon;
                        ti.hinst = GetModuleHandleW(nullptr);
                        ti.lpszText = tipBuf3;
                        SendMessageW(ds->hwndTooltip, TTM_UPDATETIPTEXT, 0, (LPARAM)&ti);
                    }
                }
            }
            return 0;
        }
        case WM_NOTIFY: {
            auto* pnmh = reinterpret_cast<NMHDR*>(lParam);
            if (ds && pnmh && pnmh->hwndFrom == ds->hwndList) {
                if (pnmh->code == LVN_ITEMCHANGED) {
                    auto* pnmlv = reinterpret_cast<NMLISTVIEW*>(lParam);
                    if ((pnmlv->uNewState & LVIS_SELECTED) && pnmlv->iItem >= 0 &&
                        pnmlv->iItem < static_cast<int>(ds->events.size())) {
                        const auto& ev = ds->events[pnmlv->iItem];
                        std::wstringstream detail;
                        detail << L"=== Event #" << ev.seq << L" [" << Utf8ToWide(ev.type) << L"] ===\r\n"
                               << L"Timestamp : " << Utf8ToWide(ev.timestamp) << L"\r\n"
                               << L"Agent     : " << Utf8ToWide(ev.agentType) << L"\r\n";
                        if (!ev.toolName.empty()) {
                            detail << L"Tool      : " << Utf8ToWide(ev.toolName) << L"\r\n";
                        }
                        if (!ev.toolAction.empty()) {
                            detail << L"Action    : " << Utf8ToWide(ev.toolAction) << L"\r\n";
                        }
                        if (!ev.toolInput.empty()) {
                            detail << L"Input     : " << Utf8ToWide(ev.toolInput) << L"\r\n";
                        }
                        if (!ev.content.empty()) {
                            detail << L"Content   :\r\n" << Utf8ToWide(ev.content) << L"\r\n";
                        }
                        if (ev.totalTokens > 0) {
                            detail << L"Tokens    : " << ev.totalTokens << L"\r\n";
                        }
                        std::string usageText = LlmEventUsageText(ev);
                        if (!usageText.empty()) {
                            detail << L"Usage     : " << Utf8ToWide(usageText) << L"\r\n";
                        }
                        detail << L"\r\n=== Raw JSON ===\r\n"
                               << Utf8ToWide(ev.rawJson) << L"\r\n";
                        SetWindowTextW(ds->hwndEditPreview, detail.str().c_str());
                    }
                }
            }
            break;
        }
        case WM_SIZE: {
            if (!ds) return 0;
            int w = LOWORD(lParam);
            int h = HIWORD(lParam);
            int pad = 12;
            int availW = (std::max)(200, w - pad * 2);
            int hdrH = 75;
            int contentX = pad + 28;
            int contentW = availW - 28;
            int graphW = (std::max)(180, contentW * 45 / 100);
            int textW = (std::max)(160, contentW - graphW - 8);
            MoveWindow(ds->hwndHeader, contentX, pad, textW, hdrH, TRUE);
            ds->rcUsageGraph = { contentX + textW + 8, pad, contentX + contentW, pad + hdrH };
            if (ds->hwndAgentIcon) MoveWindow(ds->hwndAgentIcon, pad, pad, 24, 24, TRUE);
            int btnY = (std::max)(250, h - 38);
            int middleH = btnY - (pad + hdrH + pad) - pad;
            int listH = (std::max)(80, middleH * 55 / 100);
            int editH = (std::max)(60, middleH - listH - 6);
            MoveWindow(ds->hwndList, pad, pad + hdrH + 6, availW, listH, TRUE);
            MoveWindow(ds->hwndEditPreview, pad, pad + hdrH + 6 + listH + 6, availW, editH, TRUE);
            MoveWindow(ds->hwndBtnRefresh, pad, btnY, 130, 30, TRUE);
            MoveWindow(ds->hwndBtnOpenStream, pad + 140, btnY, 260, 30, TRUE);
            MoveWindow(ds->hwndBtnClose, (std::max)(pad + 410, w - pad - 110), btnY, 110, 30, TRUE);
            InvalidateRect(hwnd, &ds->rcUsageGraph, FALSE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps = {};
            HDC hdc = BeginPaint(hwnd, &ps);
            if (ds) {
                int selIdx = ds->hwndList ? ListView_GetNextItem(ds->hwndList, -1, LVNI_SELECTED) : -1;
                const LlmStreamEvent* sel = (selIdx >= 0 && selIdx < static_cast<int>(ds->events.size()))
                    ? &ds->events[selIdx] : nullptr;
                DrawUsageGraph(hdc, ds->rcUsageGraph, ds->events, sel, selIdx);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (!ds) break;
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (PtInRect(&ds->rcUsageGraph, pt)) {
                auto bars = BuildUsageBars(ds->rcUsageGraph, ds->events);
                for (const auto& b : bars) {
                    if (!PtInRect(&b.rc, pt)) continue;
                    int idx = b.eventIndex;
                    if (idx >= 0 && idx < static_cast<int>(ds->events.size())) {
                        ListView_SetItemState(ds->hwndList, idx,
                                              LVIS_SELECTED | LVIS_FOCUSED,
                                              LVIS_SELECTED | LVIS_FOCUSED);
                        ListView_EnsureVisible(ds->hwndList, idx, FALSE);
                        SetFocus(ds->hwndList);
                        InvalidateRect(hwnd, &ds->rcUsageGraph, FALSE);
                    }
                    break;
                }
                return 0;
            }
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == 2103 && ds) {
                ReloadStreamDialogData(hwnd, ds);
            } else if (id == 2104 && ds) {
                if (!ds->snap.streamFilePath.empty()) {
                    std::wstring streamW = Utf8ToWide(ds->snap.streamFilePath);
                    ShellExecuteW(hwnd, L"open", L"notepad.exe", streamW.c_str(), nullptr, SW_SHOWNORMAL);
                }
            } else if (id == IDOK || id == IDCANCEL) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!dis || !ds) break;
            bool pressed = (dis->itemState & ODS_SELECTED) != 0;
            COLORREF bg = pressed ? g_theme.border : g_theme.panel;
            HBRUSH bgBrush = CreateSolidBrush(bg);
            FillRect(dis->hDC, &dis->rcItem, bgBrush);
            DeleteObject(bgBrush);
            HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
            HGDIOBJ oldPen = SelectObject(dis->hDC, borderPen);
            HGDIOBJ oldBrush = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
            Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
            SelectObject(dis->hDC, oldBrush);
            SelectObject(dis->hDC, oldPen);
            DeleteObject(borderPen);
            wchar_t text[256] = {};
            GetWindowTextW(dis->hwndItem, text, 256);
            RECT rc = dis->rcItem;
            InflateRect(&rc, -4, 0);
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, g_theme.text);
            HFONT oldFont = static_cast<HFONT>(SelectObject(dis->hDC, ds->hFontUi));
            DrawTextW(dis->hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dis->hDC, oldFont);
            return TRUE;
        }
        case WM_ERASEBKGND: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, g_dialogBgBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, g_theme.text);
            SetBkColor(hdc, g_theme.panel);
            return reinterpret_cast<LRESULT>(g_dialogBgBrush);
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            if (ds) {
                if (ds->timerId) KillTimer(hwnd, ds->timerId);
                if (ds->hFontMono) DeleteObject(ds->hFontMono);
                delete ds;
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowLlmStreamDialog(DashboardState* st, int procId) {
    if (!st || !st->pm || procId <= 0) return;
    ProcessSnapshot snap;
    if (!st->pm->GetSnapshot(procId, snap)) return;

    static bool regDone = false;
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!regDone) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = LlmStreamWndProc;
        wc.hInstance = hInst;
        wc.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"ProcessCommanderLlmStreamWnd";
        RegisterClassExW(&wc);
        regDone = true;
    }

    auto* ds = new LlmStreamDlgState();
    ds->dash = st;
    ds->procId = procId;
    ds->snap = snap;
    ds->hFontUi = st->hFontUi;
    ds->hFontBold = st->hFontUiBold;

    std::wstring title = std::wstring(Tr(L"LLM JSON ストリーム・イベント表示", L"LLM JSON Stream & Events", L"Flujo de eventos JSON de LLM"))
        + L" — #" + std::to_wstring(snap.id) + L" [" + Utf8ToWide(snap.name) + L"]";
    if (!snap.agentType.empty()) {
        title += L" (" + Utf8ToWide(snap.agentType) + L")";
    }

    RECT rcParent = {};
    GetWindowRect(st->hwndMain, &rcParent);
    int dlgW = 860;
    int dlgH = 620;
    int x = rcParent.left + ((rcParent.right - rcParent.left) - dlgW) / 2;
    int y = rcParent.top + ((rcParent.bottom - rcParent.top) - dlgH) / 2;

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"ProcessCommanderLlmStreamWnd",
        title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        x, y, dlgW, dlgH,
        st->hwndMain, nullptr, hInst, ds
    );
    if (hDlg) {
        ShowWindow(hDlg, SW_SHOW);
        SetForegroundWindow(hDlg);
    } else {
        delete ds;
    }
}

static void ShowTaskContextMenu(DashboardState* st, int procId, POINT ptScreen) {
    if (!st || !st->pm || procId <= 0) return;
    ProcessSnapshot snap;
    if (!st->pm->GetSnapshot(procId, snap)) return;

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, 3001, Tr(L"詳細を表示 (タスク詳細・コマンドライン)...", L"Task Details & Command Line...", L"Detalles de tarea y línea de comandos..."));
    AppendMenuW(hMenu, MF_STRING, 3007, Tr(L"LLM JSON ストリーム・イベント表示...", L"LLM JSON Stream Events...", L"Eventos de flujo JSON de LLM..."));
    AppendMenuW(hMenu, MF_STRING, 3002, Tr(L"コマンドラインをコピー", L"Copy Command Line", L"Copiar línea de comandos"));
    if (!snap.logFilePath.empty()) {
        AppendMenuW(hMenu, MF_STRING, 3003, Tr(L"ログファイルを開く", L"Open Log File", L"Abrir archivo de registro"));
    }
    if (!snap.streamFilePath.empty()) {
        AppendMenuW(hMenu, MF_STRING, 3008, Tr(L"ストリームファイル (jsonl) を開く", L"Open Stream File (jsonl)", L"Abrir archivo de flujo (jsonl)"));
    }
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    if (snap.state == ProcState::Running) {
        AppendMenuW(hMenu, MF_STRING, 3004, Tr(L"ツリー終了", L"Kill Tree", L"Terminar árbol"));
    } else {
        AppendMenuW(hMenu, MF_STRING, 3005, Tr(L"この履歴を削除", L"Remove This Entry", L"Eliminar esta entrada"));
        AppendMenuW(hMenu, MF_STRING, 3006, Tr(L"終了履歴をすべて消去", L"Clear All Ended History", L"Borrar todo el historial finalizado"));
    }

    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, ptScreen.x, ptScreen.y, 0, st->hwndMain, nullptr);
    DestroyMenu(hMenu);

    if (cmd == 3001) {
        ShowTaskDetailsDialog(st, procId);
    } else if (cmd == 3007) {
        ShowLlmStreamDialog(st, procId);
    } else if (cmd == 3008) {
        std::wstring streamW = Utf8ToWide(snap.streamFilePath);
        ShellExecuteW(st->hwndMain, L"open", L"notepad.exe", streamW.c_str(), nullptr, SW_SHOWNORMAL);
    } else if (cmd == 3002) {
        CopyTextToClipboard(st->hwndMain, Utf8ToWide(snap.commandLine));
    } else if (cmd == 3003) {
        std::wstring logW = Utf8ToWide(snap.logFilePath);
        ShellExecuteW(st->hwndMain, L"open", L"notepad.exe", logW.c_str(), nullptr, SW_SHOWNORMAL);
    } else if (cmd == 3004) {
        if (MessageBoxW(st->hwndMain,
                Tr(L"このプロセスツリーを強制終了しますか？",
                   L"Are you sure you want to kill this process tree?",
                   L"¿Seguro que desea terminar este árbol de procesos?"),
                Tr(L"ツリー終了の確認", L"Confirm Kill Tree", L"Confirmar cierre"),
                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
            std::string err;
            st->pm->KillProcess(procId, err);
            RefreshDashboardData(st);
        }
    } else if (cmd == 3005) {
        st->pm->RemoveProcess(procId);
        RefreshDashboardData(st);
    } else if (cmd == 3006) {
        st->pm->ClearFinished();
        RefreshDashboardData(st);
    }
}

static std::string GetDefaultBashCommand() {
    const wchar_t* gitBash = L"C:\\Program Files\\Git\\bin\\bash.exe";
    DWORD attr = GetFileAttributesW(gitBash);
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        return "\"C:\\Program Files\\Git\\bin\\bash.exe\" --login -i";
    }
    return "bash.exe";
}

static std::wstring GetConfigFilePathWide() {
    return GetLogsDirectoryWide() + L"\\config.json";
}

static void LoadShellConfig(DashboardState* st) {
    if (!st) return;
    st->defaultShell = ShellType::PowerShell;
    st->cmdCommand = "cmd.exe";
    st->powershellCommand = "powershell.exe";
    st->bashCommand = GetDefaultBashCommand();

    std::string content = ReadFileUtf8Wide(GetConfigFilePathWide());
    if (content.empty()) return;

    std::string defShell = JsonGetString(content, "default_shell", "powershell");
    if (defShell == "cmd") st->defaultShell = ShellType::Cmd;
    else if (defShell == "bash") st->defaultShell = ShellType::Bash;
    else st->defaultShell = ShellType::PowerShell;

    st->cmdCommand = JsonGetString(content, "cmd_command", st->cmdCommand);
    st->powershellCommand = JsonGetString(content, "powershell_command", st->powershellCommand);
    st->bashCommand = JsonGetString(content, "bash_command", st->bashCommand);
    st->hideEndedProcesses = JsonGetBool(content, "hide_ended", false);
    st->hideProcessList = JsonGetBool(content, "hide_process_list", false);
    st->hideTerminalGrid = JsonGetBool(content, "hide_terminal_grid", true);
    st->completionSoundEnabled = JsonGetBool(content, "completion_sound_enabled", true);
    st->completionSoundPath = JsonGetString(content, "completion_sound_path", "");
    int listH = static_cast<int>(JsonGetInt(content, "process_list_height", st->processListHeight));
    if (listH >= kMinListH && listH <= 4000) st->processListHeight = listH;
    int modeVal = static_cast<int>(JsonGetInt(content, "list_display_mode", static_cast<int64_t>(ListDisplayMode::TwoRows)));
    if (modeVal >= 0 && modeVal <= 2) {
        st->listDisplayMode = static_cast<ListDisplayMode>(modeVal);
    }
    int ratioPct = static_cast<int>(JsonGetInt(content, "proc_row_ratio", 50));
    if (ratioPct >= 15 && ratioPct <= 85) {
        st->procRowRatio = ratioPct / 100.0f;
    }
    std::string langCode = JsonGetString(content, "language", "");
    if (!langCode.empty()) g_uiLanguage = ParseLanguage(langCode);
    std::string themeCode = JsonGetString(content, "theme", "");
    if (!themeCode.empty()) g_uiTheme = ParseTheme(themeCode);
    ApplyTheme();
}

static void SaveShellConfig(const DashboardState* st) {
    if (!st) return;
    std::string defShell = "powershell";
    if (st->defaultShell == ShellType::Cmd) defShell = "cmd";
    else if (st->defaultShell == ShellType::Bash) defShell = "bash";

    std::ostringstream ss;
    ss << "{\n"
       << "  \"default_shell\": \"" << JsonEscape(defShell) << "\",\n"
       << "  \"cmd_command\": \"" << JsonEscape(st->cmdCommand) << "\",\n"
       << "  \"powershell_command\": \"" << JsonEscape(st->powershellCommand) << "\",\n"
       << "  \"bash_command\": \"" << JsonEscape(st->bashCommand) << "\",\n"
       << "  \"hide_ended\": " << (st->hideEndedProcesses ? "true" : "false") << ",\n"
       << "  \"hide_process_list\": " << (st->hideProcessList ? "true" : "false") << ",\n"
       << "  \"hide_terminal_grid\": " << (st->hideTerminalGrid ? "true" : "false") << ",\n"
       << "  \"completion_sound_enabled\": " << (st->completionSoundEnabled ? "true" : "false") << ",\n"
       << "  \"completion_sound_path\": \"" << JsonEscape(st->completionSoundPath) << "\",\n"
       << "  \"process_list_height\": " << st->processListHeight << ",\n"
       << "  \"list_display_mode\": " << static_cast<int>(st->listDisplayMode) << ",\n"
       << "  \"proc_row_ratio\": " << static_cast<int>(st->procRowRatio * 100.0f + 0.5f) << ",\n"
       << "  \"language\": \"" << LanguageCode(g_uiLanguage) << "\",\n"
       << "  \"theme\": \"" << ThemeCode(g_uiTheme) << "\"\n"
       << "}\n";

    WriteFileAtomicWide(GetConfigFilePathWide(), ss.str());
}

static void UpdateLaunchButtonLabel(DashboardState* st) {
    if (!st || !st->hwndBtnStart) return;
    std::wstring label = Tr(L"+ 起動 (", L"+ Launch (", L"+ Iniciar (");
    if (st->defaultShell == ShellType::Cmd) label += L"cmd";
    else if (st->defaultShell == ShellType::Bash) label += L"bash";
    else label += L"powershell";
    label += L")";
    SetWindowTextW(st->hwndBtnStart, label.c_str());
}

static void ApplyListTheme(DashboardState* st) {
    if (!st || !st->hwndList) return;
    ListView_SetBkColor(st->hwndList, g_theme.panel);
    ListView_SetTextBkColor(st->hwndList, g_theme.panel);
    ListView_SetTextColor(st->hwndList, g_theme.listText);
    InvalidateRect(st->hwndList, nullptr, TRUE);
    HWND hHeader = ListView_GetHeader(st->hwndList);
    if (hHeader) InvalidateRect(hHeader, nullptr, TRUE);
    if (st->hwndTaskList) {
        ListView_SetBkColor(st->hwndTaskList, g_theme.panel);
        ListView_SetTextBkColor(st->hwndTaskList, g_theme.panel);
        ListView_SetTextColor(st->hwndTaskList, g_theme.listText);
        InvalidateRect(st->hwndTaskList, nullptr, TRUE);
        HWND hTaskHeader = ListView_GetHeader(st->hwndTaskList);
        if (hTaskHeader) InvalidateRect(hTaskHeader, nullptr, TRUE);
    }
    if (st->hwndTab) InvalidateRect(st->hwndTab, nullptr, TRUE);
}

static void UpdateClock(DashboardState* st) {
    if (!st || !st->hwndClock) return;
    SYSTEMTIME t = {};
    GetLocalTime(&t);
    wchar_t buf[16] = {};
    swprintf_s(buf, L"%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
    SetWindowTextW(st->hwndClock, buf);
    InvalidateRect(st->hwndClock, nullptr, TRUE);
}

static void UpdateToolbarLabels(DashboardState* st) {
    if (!st) return;
    UpdateLaunchButtonLabel(st);
    if (st->hwndBtnSettings)
        SetWindowTextW(st->hwndBtnSettings, Tr(L"設定", L"Settings", L"Ajustes"));
    if (st->hwndBtnKillAll)
        SetWindowTextW(st->hwndBtnKillAll, Tr(L"全終了", L"Kill All", L"Terminar todo"));
    if (st->hwndBtnGridView)
        SetWindowTextW(st->hwndBtnGridView, L"▦");
    if (st->hwndBtnToggleList)
        SetWindowTextW(st->hwndBtnToggleList, L"☰");
    if (st->hwndBtnToggleTerm)
        SetWindowTextW(st->hwndBtnToggleTerm, L">_");
    if (st->hwndBtnHideTray)
        SetWindowTextW(st->hwndBtnHideTray, L"⤓");

    if (st->hwndToolbarTip) {
        if (st->hwndBtnGridView)
            UpdateControlTooltip(st->hwndToolbarTip, st->hwndMain, st->hwndBtnGridView,
                Tr(L"グリッド表示", L"Grid View", L"Vista de cuadrícula"));
        if (st->hwndBtnHideTray)
            UpdateControlTooltip(st->hwndToolbarTip, st->hwndMain, st->hwndBtnHideTray,
                Tr(L"トレイに格納", L"Tray", L"Bandeja"));
    }
    UpdatePanelToggleLabels(st);
    if (st->hwndBtnViewTasks) {
        const wchar_t* btnLabel = Tr(L"表示: 2行 (上下)", L"View: 2 Rows", L"Vista: 2 Filas");
        if (st->listDisplayMode == ListDisplayMode::ProcessOnly) {
            btnLabel = Tr(L"表示: プロセス", L"View: Process Only", L"Vista: Solo Proc");
        } else if (st->listDisplayMode == ListDisplayMode::TaskOnly) {
            btnLabel = Tr(L"表示: タスク", L"View: Task Only", L"Vista: Solo Tareas");
        }
        SetWindowTextW(st->hwndBtnViewTasks, btnLabel);
    }
}

static void ApplyLanguageToUi(DashboardState* st) {
    if (!st) return;
    SetWindowTextW(st->hwndMain,
        Tr(L"ProcessCommander — マルチターミナル ConPTY ダッシュボード (Direct2D TerminalView)",
           L"ProcessCommander — Multi-Terminal ConPTY Dashboard (Direct2D TerminalView)",
           L"ProcessCommander — Panel de terminales ConPTY (Direct2D TerminalView)"));
    UpdateToolbarLabels(st);
    UpdateListHeaderSortIndicator(st);
    UpdateTaskListHeaders(st);
    RefreshDashboardData(st);
    InvalidateRect(st->hwndMain, nullptr, FALSE);
}

static void LaunchShellByType(DashboardState* st, ShellType type) {
    if (!st || !st->pm) return;
    StartProcessRequest req;
    req.mode = ExecMode::ConPTY;
    if (type == ShellType::Cmd) {
        req.name = "cmd";
        req.commandLine = st->cmdCommand.empty() ? "cmd.exe" : st->cmdCommand;
    } else if (type == ShellType::Bash) {
        req.name = "bash";
        req.commandLine = st->bashCommand.empty() ? GetDefaultBashCommand() : st->bashCommand;
    } else {
        req.name = "powershell";
        req.commandLine = st->powershellCommand.empty() ? "powershell.exe" : st->powershellCommand;
    }

    std::string err;
    int id = st->pm->StartProcess(req, err);
    if (id < 0) {
        std::wstring werr = Utf8ToWide(err);
        MessageBoxW(st->hwndMain, werr.c_str(), L"Failed to Start Terminal", MB_ICONERROR);
    } else {
        st->selectedProcId = id;
        RefreshDashboardData(st);
        TerminalView* tv = st->pm->GetTerminalView(id);
        if (tv && tv->Hwnd()) {
            SetFocus(tv->Hwnd());
        }
    }
}

static void LaunchFromToolbar(DashboardState* st) {
    if (!st) return;
    LaunchShellByType(st, st->defaultShell);
}

struct ShellConfigDlgState {
    DashboardState* dash = nullptr;
    HWND hwndRadCmd = nullptr;
    HWND hwndRadPs = nullptr;
    HWND hwndRadBash = nullptr;
    HWND hwndEditCmd = nullptr;
    HWND hwndEditPs = nullptr;
    HWND hwndEditBash = nullptr;
    ShellType selectedShell = ShellType::PowerShell;
};

static void ApplyShellConfigFromDialog(HWND hwnd, ShellConfigDlgState* cs) {
    if (!cs || !cs->dash) return;
    cs->dash->defaultShell = cs->selectedShell;

    wchar_t buf[1024] = {};
    GetWindowTextW(cs->hwndEditCmd, buf, 1023);
    cs->dash->cmdCommand = WideToUtf8(buf);
    GetWindowTextW(cs->hwndEditPs, buf, 1023);
    cs->dash->powershellCommand = WideToUtf8(buf);
    GetWindowTextW(cs->hwndEditBash, buf, 1023);
    cs->dash->bashCommand = WideToUtf8(buf);

    SaveShellConfig(cs->dash);
    UpdateLaunchButtonLabel(cs->dash);
}

static LRESULT CALLBACK ShellConfigWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* cs = reinterpret_cast<ShellConfigDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
            cs = reinterpret_cast<ShellConfigDlgState*>(createStruct->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs));
            DashboardState* st = cs->dash;

            HWND hTitle = CreateWindowExW(0, L"STATIC",
                Tr(L"Launch Terminal のデフォルトシェルと起動コマンド:",
                   L"Default shell and startup command for Launch Terminal:",
                   L"Shell predeterminado y comando de inicio para Launch Terminal:"),
                WS_CHILD | WS_VISIBLE, 16, 14, 540, 20, hwnd, nullptr, nullptr, nullptr);

            cs->hwndRadCmd = CreateWindowExW(0, L"BUTTON", L"cmd (Command Prompt)",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                16, 44, 190, 24, hwnd, (HMENU)5001, nullptr, nullptr);
            cs->hwndRadPs = CreateWindowExW(0, L"BUTTON", L"powershell (PowerShell)",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                16, 84, 190, 24, hwnd, (HMENU)5002, nullptr, nullptr);
            cs->hwndRadBash = CreateWindowExW(0, L"BUTTON", L"bash (Git Bash / WSL)",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                16, 124, 190, 24, hwnd, (HMENU)5003, nullptr, nullptr);

            cs->hwndEditCmd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", Utf8ToWide(st->cmdCommand).c_str(),
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 212, 44, 260, 24, hwnd, (HMENU)5011, nullptr, nullptr);
            cs->hwndEditPs = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", Utf8ToWide(st->powershellCommand).c_str(),
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 212, 84, 260, 24, hwnd, (HMENU)5012, nullptr, nullptr);
            cs->hwndEditBash = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", Utf8ToWide(st->bashCommand).c_str(),
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 212, 124, 260, 24, hwnd, (HMENU)5013, nullptr, nullptr);

            HWND hBtnRunCmd = CreateWindowExW(0, L"BUTTON", Tr(L"cmd を起動", L"Launch cmd", L"Iniciar cmd"),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 480, 44, 140, 24, hwnd, (HMENU)5021, nullptr, nullptr);
            HWND hBtnRunPs = CreateWindowExW(0, L"BUTTON", Tr(L"powershell を起動", L"Launch powershell", L"Iniciar powershell"),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 480, 84, 140, 24, hwnd, (HMENU)5022, nullptr, nullptr);
            HWND hBtnRunBash = CreateWindowExW(0, L"BUTTON", Tr(L"bash を起動", L"Launch bash", L"Iniciar bash"),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 480, 124, 140, 24, hwnd, (HMENU)5023, nullptr, nullptr);

            cs->selectedShell = st->defaultShell;
            SendMessageW(cs->hwndRadCmd, BM_SETCHECK, st->defaultShell == ShellType::Cmd ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(cs->hwndRadPs, BM_SETCHECK, st->defaultShell == ShellType::PowerShell ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(cs->hwndRadBash, BM_SETCHECK, st->defaultShell == ShellType::Bash ? BST_CHECKED : BST_UNCHECKED, 0);

            HWND hBtnSave = CreateWindowExW(0, L"BUTTON", Tr(L"保存して閉じる", L"Save and Close", L"Guardar y cerrar"),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 330, 170, 140, 30, hwnd, (HMENU)IDOK, nullptr, nullptr);
            HWND hBtnCancel = CreateWindowExW(0, L"BUTTON", Tr(L"キャンセル", L"Cancel", L"Cancelar"),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 480, 170, 140, 30, hwnd, (HMENU)IDCANCEL, nullptr, nullptr);

            SendMessageW(hTitle, WM_SETFONT, (WPARAM)st->hFontUiBold, TRUE);
            HWND ctrls[] = {
                cs->hwndRadCmd, cs->hwndRadPs, cs->hwndRadBash,
                cs->hwndEditCmd, cs->hwndEditPs, cs->hwndEditBash,
                hBtnRunCmd, hBtnRunPs, hBtnRunBash, hBtnSave, hBtnCancel
            };
            for (HWND h : ctrls) {
                SendMessageW(h, WM_SETFONT, (WPARAM)st->hFontUi, TRUE);
            }
            ApplyNativeThemeToWindow(hwnd);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDOK && cs) {
                ApplyShellConfigFromDialog(hwnd, cs);
                DestroyWindow(hwnd);
            } else if (id == IDCANCEL) {
                DestroyWindow(hwnd);
            } else if ((id == 5001 || id == 5002 || id == 5003) && cs) {
                cs->selectedShell = (id == 5001) ? ShellType::Cmd :
                                    (id == 5003) ? ShellType::Bash : ShellType::PowerShell;
                SendMessageW(cs->hwndRadCmd, BM_SETCHECK, id == 5001 ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(cs->hwndRadPs, BM_SETCHECK, id == 5002 ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(cs->hwndRadBash, BM_SETCHECK, id == 5003 ? BST_CHECKED : BST_UNCHECKED, 0);
                InvalidateRect(hwnd, nullptr, TRUE);
            } else if ((id == 5021 || id == 5022 || id == 5023) && cs && cs->dash) {
                if (id == 5021) {
                    cs->selectedShell = ShellType::Cmd;
                    SendMessageW(cs->hwndRadCmd, BM_SETCHECK, BST_CHECKED, 0);
                    SendMessageW(cs->hwndRadPs, BM_SETCHECK, BST_UNCHECKED, 0);
                    SendMessageW(cs->hwndRadBash, BM_SETCHECK, BST_UNCHECKED, 0);
                } else if (id == 5022) {
                    cs->selectedShell = ShellType::PowerShell;
                    SendMessageW(cs->hwndRadCmd, BM_SETCHECK, BST_UNCHECKED, 0);
                    SendMessageW(cs->hwndRadPs, BM_SETCHECK, BST_CHECKED, 0);
                    SendMessageW(cs->hwndRadBash, BM_SETCHECK, BST_UNCHECKED, 0);
                } else if (id == 5023) {
                    cs->selectedShell = ShellType::Bash;
                    SendMessageW(cs->hwndRadCmd, BM_SETCHECK, BST_UNCHECKED, 0);
                    SendMessageW(cs->hwndRadPs, BM_SETCHECK, BST_UNCHECKED, 0);
                    SendMessageW(cs->hwndRadBash, BM_SETCHECK, BST_CHECKED, 0);
                }
                ApplyShellConfigFromDialog(hwnd, cs);
                DashboardState* dash = cs->dash;
                ShellType target = dash->defaultShell;
                DestroyWindow(hwnd);
                LaunchShellByType(dash, target);
            }
            return 0;
        }
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!dis || !cs) break;
            int id = static_cast<int>(dis->CtlID);
            bool radio = (id == 5001 || id == 5002 || id == 5003);
            bool checked = (id == 5001 && cs->selectedShell == ShellType::Cmd) ||
                           (id == 5002 && cs->selectedShell == ShellType::PowerShell) ||
                           (id == 5003 && cs->selectedShell == ShellType::Bash);
            bool pressed = (dis->itemState & ODS_SELECTED) != 0;
            COLORREF bg = pressed ? g_theme.border : g_theme.panel;
            HBRUSH bgBrush = CreateSolidBrush(bg);
            FillRect(dis->hDC, &dis->rcItem, bgBrush);
            DeleteObject(bgBrush);
            HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
            HGDIOBJ oldPen = SelectObject(dis->hDC, borderPen);
            HGDIOBJ oldBrush = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
            Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
            SelectObject(dis->hDC, oldBrush);
            SelectObject(dis->hDC, oldPen);
            DeleteObject(borderPen);

            if (dis->itemState & ODS_FOCUS) {
                RECT rcFocus = dis->rcItem;
                InflateRect(&rcFocus, -2, -2);
                DrawFocusRect(dis->hDC, &rcFocus);
            }

            RECT textRc = dis->rcItem;
            if (radio) {
                int cx = dis->rcItem.left + 14;
                int cy = (dis->rcItem.top + dis->rcItem.bottom) / 2;
                HPEN circlePen = CreatePen(PS_SOLID, 1, g_theme.muted);
                HGDIOBJ op = SelectObject(dis->hDC, circlePen);
                HGDIOBJ ob = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
                Ellipse(dis->hDC, cx - 6, cy - 6, cx + 6, cy + 6);
                SelectObject(dis->hDC, ob);
                SelectObject(dis->hDC, op);
                DeleteObject(circlePen);
                if (checked) {
                    HBRUSH dot = CreateSolidBrush(g_theme.accent);
                    HGDIOBJ ob2 = SelectObject(dis->hDC, dot);
                    Ellipse(dis->hDC, cx - 3, cy - 3, cx + 3, cy + 3);
                    SelectObject(dis->hDC, ob2);
                    DeleteObject(dot);
                }
                textRc.left += 28;
            }
            InflateRect(&textRc, -4, 0);
            wchar_t text[256] = {};
            GetWindowTextW(dis->hwndItem, text, 256);
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, g_theme.text);
            HFONT oldFont = static_cast<HFONT>(SelectObject(dis->hDC, cs->dash->hFontUi));
            UINT fmt = (radio ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
            DrawTextW(dis->hDC, text, -1, &textRc, fmt);
            SelectObject(dis->hDC, oldFont);
            return TRUE;
        }
        case WM_ERASEBKGND: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, g_dialogBgBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, g_theme.text);
            SetBkColor(hdc, g_theme.panel);
            return reinterpret_cast<LRESULT>(g_dialogBgBrush);
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            delete cs;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowShellConfigDialog(DashboardState* st) {
    if (!st || !st->hwndMain) return;
    static bool regDone = false;
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!regDone) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = ShellConfigWndProc;
        wc.hInstance = hInst;
        wc.hIcon = LoadAppIcon();
        wc.hIconSm = LoadAppSmallIcon();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"ProcessCommanderShellConfigWnd";
        RegisterClassExW(&wc);
        regDone = true;
    }

    auto* cs = new ShellConfigDlgState();
    cs->dash = st;

    RECT rcParent = {};
    GetWindowRect(st->hwndMain, &rcParent);
    int dlgW = 660;
    int dlgH = 250;
    int x = rcParent.left + ((rcParent.right - rcParent.left) - dlgW) / 2;
    int y = rcParent.top + ((rcParent.bottom - rcParent.top) - dlgH) / 2;

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"ProcessCommanderShellConfigWnd",
        Tr(L"シェル設定 (cmd / powershell / bash)", L"Shell Config (cmd / powershell / bash)", L"Configuración de shell (cmd / powershell / bash)"),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, dlgW, dlgH,
        st->hwndMain, nullptr, hInst, cs
    );
    if (hDlg) {
        ShowWindow(hDlg, SW_SHOW);
        SetForegroundWindow(hDlg);
    } else {
        delete cs;
    }
}

static void InitTrayIcon(DashboardState* st) {
    if (!st || !st->hwndMain || st->trayAdded) return;
    ZeroMemory(&st->nid, sizeof(st->nid));
    st->nid.cbSize = sizeof(NOTIFYICONDATAW);
    st->nid.hWnd = st->hwndMain;
    st->nid.uID = 1;
    st->nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    st->nid.uCallbackMessage = WM_PC_TRAYICON;
    st->nid.hIcon = LoadAppSmallIcon();
    wcscpy_s(st->nid.szTip, L"ProcessCommander — Multi-Terminal ConPTY Dashboard");
    st->trayAdded = (Shell_NotifyIconW(NIM_ADD, &st->nid) != FALSE);
}

static void RemoveTrayIcon(DashboardState* st) {
    if (!st || !st->trayAdded) return;
    Shell_NotifyIconW(NIM_DELETE, &st->nid);
    st->trayAdded = false;
}

static void HideToTray(DashboardState* st) {
    if (!st || !st->hwndMain) return;
    InitTrayIcon(st);
    if (st->gridView && st->gridView->IsVisible()) {
        st->gridView->Hide();
    }
    ShowWindow(st->hwndMain, SW_HIDE);
}

static void RestoreFromTray(DashboardState* st) {
    if (!st || !st->hwndMain) return;
    ShowWindow(st->hwndMain, SW_SHOW);
    if (IsIconic(st->hwndMain)) {
        ShowWindow(st->hwndMain, SW_RESTORE);
    }
    SetForegroundWindow(st->hwndMain);
    RefreshDashboardData(st);
    if (st->selectedProcId > 0 && st->pm) {
        TerminalView* tv = st->pm->GetTerminalView(st->selectedProcId);
        if (tv && tv->Hwnd()) {
            SetFocus(tv->Hwnd());
        }
    }
}

static void ShowTrayContextMenu(DashboardState* st) {
    if (!st || !st->hwndMain) return;
    POINT pt = {};
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING | MF_DEFAULT, IDM_TRAY_RESTORE, Tr(L"ダッシュボードを復元", L"Restore Dashboard", L"Restaurar panel"));
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_GRID_VIEW, Tr(L"グリッド表示を開く", L"Open Grid View", L"Abrir vista de cuadrícula"));
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_USAGE, Tr(L"トークン使用量グラフ", L"Token Usage Graph", L"Gráfico de uso de tokens"));
    if (IsWindowVisible(st->hwndMain)) {
        AppendMenuW(hMenu, MF_STRING, IDM_TRAY_HIDE, Tr(L"タスクトレイに格納", L"Hide to Tray", L"Ocultar en bandeja"));
    }
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, Tr(L"ProcessCommander を終了", L"Exit ProcessCommander", L"Salir de ProcessCommander"));

    SetForegroundWindow(st->hwndMain);
    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, st->hwndMain, nullptr);
    PostMessageW(st->hwndMain, WM_NULL, 0, 0);
    DestroyMenu(hMenu);

    if (cmd == IDM_TRAY_RESTORE) {
        RestoreFromTray(st);
    } else if (cmd == IDM_TRAY_GRID_VIEW) {
        RestoreFromTray(st);
        if (st->gridView) {
            LayoutTerminalViews(st);
            st->gridView->Show();
        }
    } else if (cmd == IDM_TRAY_USAGE) {
        RestoreFromTray(st);
        st->showUsagePanel = true;
        RECT rcClient = {};
        GetClientRect(st->hwndMain, &rcClient);
        LayoutControls(st, rcClient.right, rcClient.bottom);
        InvalidateRect(st->hwndMain, nullptr, FALSE);
    } else if (cmd == IDM_TRAY_HIDE) {
        HideToTray(st);
    } else if (cmd == IDM_TRAY_EXIT) {
        st->allowExit = true;
        DestroyWindow(st->hwndMain);
    }
}

static void ResetProcessIdsWithConfirm(DashboardState* st) {
    if (!st || !st->pm || !st->hwndMain) return;
    std::wstring title = Tr(L"プロセスIDをリセット", L"Reset Process IDs", L"Restablecer IDs");
    std::wstring msg = Tr(
        L"プロセスIDをリセットしますか？ 終了済みプロセスは #1 から振り直されます。\n"
        L"実行中のプロセスはすべて停止している必要があります。",
        L"Reset process IDs? Finished processes will be renumbered from #1.\n"
        L"All running processes must be stopped first.",
        L"¿Restablecer los IDs? Los procesos finalizados se renumerarán desde #1.\n"
        L"Primero debe detener todos los procesos en ejecución.");
    if (MessageBoxW(st->hwndMain, msg.c_str(), title.c_str(), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    std::string err;
    int count = st->pm->ResetProcessIds(err);
    if (count < 0) {
        MessageBoxW(st->hwndMain, Utf8ToWide(err).c_str(), title.c_str(), MB_ICONWARNING);
    } else {
        st->selectedProcId = -1;
        RefreshDashboardData(st);
    }
}

struct SettingsDlgState {
    DashboardState* dash = nullptr;
    HWND hwndTitle = nullptr;
    HWND hwndLangLabel = nullptr;
    HWND hwndThemeLabel = nullptr;
    HWND btnJa = nullptr;
    HWND btnEn = nullptr;
    HWND btnEs = nullptr;
    HWND btnSystem = nullptr;
    HWND btnDark = nullptr;
    HWND btnLight = nullptr;
    HWND btnPastel = nullptr;
    HWND btnHc = nullptr;
    HWND btnShell = nullptr;
    HWND btnReset = nullptr;
    HWND btnClear = nullptr;
    HWND btnClose = nullptr;
    HWND hwndSoundLabel = nullptr;
    HWND chkSound = nullptr;
    HWND editSound = nullptr;
    HWND btnBrowseSound = nullptr;
    HWND btnTestSound = nullptr;
    HWND hwndSoundHint = nullptr;
    HWND chkHideEnded = nullptr;
};

static void CommitSoundSettings(SettingsDlgState* ds) {
    if (!ds || !ds->dash) return;
    wchar_t path[MAX_PATH * 4] = {};
    GetWindowTextW(ds->editSound, path, MAX_PATH * 4);
    ds->dash->completionSoundPath = WideToUtf8(path);
    ds->dash->completionSoundEnabled =
        (SendMessageW(ds->chkSound, BM_GETCHECK, 0, 0) == BST_CHECKED);
    SaveShellConfig(ds->dash);
}

static void UpdateSettingsDialogTexts(HWND hwnd) {
    auto* ds = reinterpret_cast<SettingsDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!ds) return;
    SetWindowTextW(hwnd, Tr(L"設定", L"Settings", L"Ajustes"));
    SetWindowTextW(ds->hwndTitle, Tr(L"表示と言語の設定", L"Appearance and language", L"Apariencia e idioma"));
    SetWindowTextW(ds->hwndLangLabel, Tr(L"言語", L"Language", L"Idioma"));
    SetWindowTextW(ds->hwndThemeLabel, Tr(L"テーマ (色)", L"Theme (colors)", L"Tema (colores)"));
    SetWindowTextW(ds->btnSystem, L"Windows");
    SetWindowTextW(ds->btnDark, Tr(L"ダーク", L"Dark", L"Oscuro"));
    SetWindowTextW(ds->btnLight, Tr(L"ライト", L"Light", L"Claro"));
    SetWindowTextW(ds->btnPastel, Tr(L"パステル", L"Pastel", L"Pastel"));
    SetWindowTextW(ds->btnHc, Tr(L"ハイコントラスト", L"High Contrast", L"Alto contraste"));
    SetWindowTextW(ds->btnShell, Tr(L"シェル構成...", L"Shell Config...", L"Configurar shell..."));
    SetWindowTextW(ds->btnReset, Tr(L"プロセスIDをリセット", L"Reset Process IDs", L"Restablecer IDs"));
    SetWindowTextW(ds->btnClear, Tr(L"終了履歴を消去", L"Clear Ended", L"Limpiar finalizados"));
    SetWindowTextW(ds->chkHideEnded, Tr(L"終了履歴を非表示", L"Hide ended processes", L"Ocultar finalizados"));
    SetWindowTextW(ds->btnClose, Tr(L"閉じる", L"Close", L"Cerrar"));
    SetWindowTextW(ds->hwndSoundLabel, Tr(L"終了時の通知音", L"Completion sound", L"Sonido al finalizar"));
    SetWindowTextW(ds->chkSound, Tr(L"プロセス終了時に音を鳴らす", L"Play a sound when a process finishes", L"Reproducir un sonido al finalizar un proceso"));
    SetWindowTextW(ds->btnBrowseSound, Tr(L"参照...", L"Browse...", L"Examinar..."));
    SetWindowTextW(ds->btnTestSound, Tr(L"試聴", L"Test", L"Probar"));
    SetWindowTextW(ds->hwndSoundHint, Tr(L"(空欄 = Windows の既定の通知音)", L"(Empty = Windows default notification sound)", L"(Vacío = sonido de notificación predeterminado de Windows)"));
    CheckRadioButton(hwnd, IDC_SET_LANG_JA, IDC_SET_LANG_ES,
        g_uiLanguage == UiLanguage::Japanese ? IDC_SET_LANG_JA :
        g_uiLanguage == UiLanguage::Spanish ? IDC_SET_LANG_ES : IDC_SET_LANG_EN);
    CheckRadioButton(hwnd, IDC_SET_THEME_SYSTEM, IDC_SET_THEME_PASTEL,
        g_uiTheme == UiTheme::Dark ? IDC_SET_THEME_DARK :
        g_uiTheme == UiTheme::Light ? IDC_SET_THEME_LIGHT :
        g_uiTheme == UiTheme::Pastel ? IDC_SET_THEME_PASTEL :
        g_uiTheme == UiTheme::HighContrast ? IDC_SET_THEME_HC : IDC_SET_THEME_SYSTEM);
}

static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ds = reinterpret_cast<SettingsDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ds = reinterpret_cast<SettingsDlgState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ds));
            HINSTANCE hInst = GetModuleHandleW(nullptr);

            ds->hwndTitle = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                18, 14, 440, 22, hwnd, nullptr, hInst, nullptr);
            SendMessageW(ds->hwndTitle, WM_SETFONT, (WPARAM)ds->dash->hFontUiBold, TRUE);

            ds->hwndLangLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                18, 48, 200, 20, hwnd, nullptr, hInst, nullptr);
            SendMessageW(ds->hwndLangLabel, WM_SETFONT, (WPARAM)ds->dash->hFontUiBold, TRUE);

            ds->btnJa = CreateWindowExW(0, L"BUTTON", L"日本語", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                18, 72, 120, 30, hwnd, (HMENU)IDC_SET_LANG_JA, hInst, nullptr);
            ds->btnEn = CreateWindowExW(0, L"BUTTON", L"English", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                146, 72, 120, 30, hwnd, (HMENU)IDC_SET_LANG_EN, hInst, nullptr);
            ds->btnEs = CreateWindowExW(0, L"BUTTON", L"Español", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                274, 72, 120, 30, hwnd, (HMENU)IDC_SET_LANG_ES, hInst, nullptr);

            ds->hwndThemeLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                18, 118, 200, 20, hwnd, nullptr, hInst, nullptr);
            SendMessageW(ds->hwndThemeLabel, WM_SETFONT, (WPARAM)ds->dash->hFontUiBold, TRUE);

            ds->btnSystem = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                18, 142, 130, 30, hwnd, (HMENU)IDC_SET_THEME_SYSTEM, hInst, nullptr);
            ds->btnDark = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                156, 142, 90, 30, hwnd, (HMENU)IDC_SET_THEME_DARK, hInst, nullptr);
            ds->btnLight = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                254, 142, 90, 30, hwnd, (HMENU)IDC_SET_THEME_LIGHT, hInst, nullptr);
            ds->btnPastel = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                352, 142, 100, 30, hwnd, (HMENU)IDC_SET_THEME_PASTEL, hInst, nullptr);
            ds->btnHc = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                460, 142, 168, 30, hwnd, (HMENU)IDC_SET_THEME_HC, hInst, nullptr);

            ds->btnShell = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                18, 200, 180, 32, hwnd, (HMENU)IDC_SET_SHELL_CONFIG, hInst, nullptr);
            ds->btnReset = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                210, 200, 180, 32, hwnd, (HMENU)IDC_SET_RESET_IDS, hInst, nullptr);
            ds->btnClear = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                402, 200, 180, 32, hwnd, (HMENU)IDC_SET_CLEAR_ENDED, hInst, nullptr);
            ds->btnClose = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                560, 360, 110, 32, hwnd, (HMENU)IDCANCEL, hInst, nullptr);

            ds->hwndSoundLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                18, 248, 300, 20, hwnd, nullptr, hInst, nullptr);
            SendMessageW(ds->hwndSoundLabel, WM_SETFONT, (WPARAM)ds->dash->hFontUiBold, TRUE);

            ds->chkSound = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                18, 272, 340, 24, hwnd, (HMENU)IDC_SET_SOUND_ENABLED, hInst, nullptr);
            ds->editSound = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                18, 302, 470, 26, hwnd, (HMENU)IDC_SET_SOUND_PATH, hInst, nullptr);
            ds->btnBrowseSound = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                496, 302, 90, 26, hwnd, (HMENU)IDC_SET_SOUND_BROWSE, hInst, nullptr);
            ds->btnTestSound = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                594, 302, 80, 26, hwnd, (HMENU)IDC_SET_SOUND_TEST, hInst, nullptr);
            ds->hwndSoundHint = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                18, 332, 470, 18, hwnd, nullptr, hInst, nullptr);
            SendMessageW(ds->hwndSoundHint, WM_SETFONT, (WPARAM)ds->dash->hFontUi, TRUE);

            SendMessageW(ds->chkSound, BM_SETCHECK,
                ds->dash->completionSoundEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
            SetWindowTextW(ds->editSound, Utf8ToWide(ds->dash->completionSoundPath).c_str());

            ds->chkHideEnded = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                18, 364, 380, 24, hwnd, (HMENU)IDC_SET_HIDE_ENDED, hInst, nullptr);
            SendMessageW(ds->chkHideEnded, BM_SETCHECK,
                ds->dash->hideEndedProcesses ? BST_CHECKED : BST_UNCHECKED, 0);

            HWND ctrls[] = { ds->btnJa, ds->btnEn, ds->btnEs, ds->btnSystem, ds->btnDark, ds->btnLight, ds->btnPastel, ds->btnHc,
                             ds->btnShell, ds->btnReset, ds->btnClear, ds->btnClose, ds->chkSound, ds->editSound,
                             ds->btnBrowseSound, ds->btnTestSound, ds->chkHideEnded };
            for (HWND h : ctrls) SendMessageW(h, WM_SETFONT, (WPARAM)ds->dash->hFontUi, TRUE);

            UpdateSettingsDialogTexts(hwnd);
            ApplyNativeThemeToWindow(hwnd);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDCANCEL || id == IDOK) {
                if (ds) CommitSoundSettings(ds);
                DestroyWindow(hwnd);
                return 0;
            }
            if (!ds) return 0;
            if (id == IDC_SET_HIDE_ENDED) {
                ds->dash->hideEndedProcesses =
                    (SendMessageW(ds->chkHideEnded, BM_GETCHECK, 0, 0) == BST_CHECKED);
                SaveShellConfig(ds->dash);
                RefreshDashboardData(ds->dash);
                return 0;
            }
            if (id == IDC_SET_SOUND_ENABLED) {
                CommitSoundSettings(ds);
                return 0;
            }
            if (id == IDC_SET_SOUND_BROWSE) {
                CommitSoundSettings(ds);
                wchar_t file[MAX_PATH] = {};
                OPENFILENAMEW ofn = {};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFilter = L"WAV (*.wav)\0*.wav\0All Files (*.*)\0*.*\0";
                ofn.lpstrFile = file;
                ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                if (GetOpenFileNameW(&ofn)) {
                    SetWindowTextW(ds->editSound, file);
                    CommitSoundSettings(ds);
                }
                return 0;
            }
            if (id == IDC_SET_SOUND_TEST) {
                CommitSoundSettings(ds);
                PlayCompletionSound(*ds->dash);
                return 0;
            }
            if (id == IDC_SET_LANG_JA || id == IDC_SET_LANG_EN || id == IDC_SET_LANG_ES) {
                g_uiLanguage = (id == IDC_SET_LANG_JA) ? UiLanguage::Japanese :
                               (id == IDC_SET_LANG_ES) ? UiLanguage::Spanish : UiLanguage::English;
                SaveShellConfig(ds->dash);
                ApplyLanguageToUi(ds->dash);
                UpdateSettingsDialogTexts(hwnd);
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
            if (id == IDC_SET_THEME_SYSTEM || id == IDC_SET_THEME_DARK ||
                id == IDC_SET_THEME_LIGHT || id == IDC_SET_THEME_PASTEL ||
                id == IDC_SET_THEME_HC) {
                g_uiTheme = (id == IDC_SET_THEME_LIGHT) ? UiTheme::Light :
                            (id == IDC_SET_THEME_PASTEL) ? UiTheme::Pastel :
                            (id == IDC_SET_THEME_HC) ? UiTheme::HighContrast :
                            (id == IDC_SET_THEME_DARK) ? UiTheme::Dark : UiTheme::System;
                ApplyTheme();
                SaveShellConfig(ds->dash);
                ApplyListTheme(ds->dash);
                ApplyNativeThemeToWindow(ds->dash->hwndMain);
                ApplyNativeThemeToWindow(hwnd);
                HWND shellDlg = FindWindowW(L"ProcessCommanderShellConfigWnd", nullptr);
                if (shellDlg) ApplyNativeThemeToWindow(shellDlg);
                HWND taskDlg = FindWindowW(L"ProcessCommanderTaskDetailsWnd", nullptr);
                if (taskDlg) ApplyNativeThemeToWindow(taskDlg);
                InvalidateRect(ds->dash->hwndMain, nullptr, TRUE);
                UpdateSettingsDialogTexts(hwnd);
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
            if (id == IDC_SET_SHELL_CONFIG) {
                ShowShellConfigDialog(ds->dash);
                return 0;
            }
            if (id == IDC_SET_RESET_IDS) {
                ResetProcessIdsWithConfirm(ds->dash);
                return 0;
            }
            if (id == IDC_SET_CLEAR_ENDED) {
                if (ds->dash && ds->dash->pm) {
                    ds->dash->pm->ClearFinished();
                    RefreshDashboardData(ds->dash);
                }
                return 0;
            }
            return 0;
        }
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!dis || !ds) break;
            int id = static_cast<int>(dis->CtlID);
            bool radio = (id >= IDC_SET_LANG_JA && id <= IDC_SET_LANG_ES) ||
                         (id >= IDC_SET_THEME_SYSTEM && id <= IDC_SET_THEME_PASTEL);
            bool checked = false;
            if (id == IDC_SET_LANG_JA) checked = (g_uiLanguage == UiLanguage::Japanese);
            else if (id == IDC_SET_LANG_EN) checked = (g_uiLanguage == UiLanguage::English);
            else if (id == IDC_SET_LANG_ES) checked = (g_uiLanguage == UiLanguage::Spanish);
            else if (id == IDC_SET_THEME_SYSTEM) checked = (g_uiTheme == UiTheme::System);
            else if (id == IDC_SET_THEME_DARK) checked = (g_uiTheme == UiTheme::Dark);
            else if (id == IDC_SET_THEME_LIGHT) checked = (g_uiTheme == UiTheme::Light);
            else if (id == IDC_SET_THEME_PASTEL) checked = (g_uiTheme == UiTheme::Pastel);
            else if (id == IDC_SET_THEME_HC) checked = (g_uiTheme == UiTheme::HighContrast);

            bool pressed = (dis->itemState & ODS_SELECTED) != 0;
            COLORREF bg = pressed ? g_theme.border : g_theme.panel;
            HBRUSH bgBrush = CreateSolidBrush(bg);
            FillRect(dis->hDC, &dis->rcItem, bgBrush);
            DeleteObject(bgBrush);
            HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
            HGDIOBJ oldPen = SelectObject(dis->hDC, borderPen);
            HGDIOBJ oldBrush = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
            Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
            SelectObject(dis->hDC, oldBrush);
            SelectObject(dis->hDC, oldPen);
            DeleteObject(borderPen);

            if (dis->itemState & ODS_FOCUS) {
                RECT rcFocus = dis->rcItem;
                InflateRect(&rcFocus, -2, -2);
                DrawFocusRect(dis->hDC, &rcFocus);
            }

            RECT textRc = dis->rcItem;
            if (radio) {
                int cx = dis->rcItem.left + 16;
                int cy = (dis->rcItem.top + dis->rcItem.bottom) / 2;
                HPEN circlePen = CreatePen(PS_SOLID, 1, g_theme.muted);
                HGDIOBJ op = SelectObject(dis->hDC, circlePen);
                HGDIOBJ ob = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
                Ellipse(dis->hDC, cx - 7, cy - 7, cx + 7, cy + 7);
                SelectObject(dis->hDC, ob);
                SelectObject(dis->hDC, op);
                DeleteObject(circlePen);
                if (checked) {
                    HBRUSH dot = CreateSolidBrush(g_theme.accent);
                    HGDIOBJ ob2 = SelectObject(dis->hDC, dot);
                    Ellipse(dis->hDC, cx - 4, cy - 4, cx + 4, cy + 4);
                    SelectObject(dis->hDC, ob2);
                    DeleteObject(dot);
                }
                textRc.left += 30;
            }
            InflateRect(&textRc, -4, 0);
            wchar_t text[256] = {};
            GetWindowTextW(dis->hwndItem, text, 256);
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, g_theme.text);
            HFONT oldFont = static_cast<HFONT>(SelectObject(dis->hDC, ds->dash->hFontUi));
            UINT fmt = (radio ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
            DrawTextW(dis->hDC, text, -1, &textRc, fmt);
            SelectObject(dis->hDC, oldFont);
            return TRUE;
        }

        case WM_ERASEBKGND: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, g_dialogBgBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, g_theme.text);
            SetBkColor(hdc, g_theme.panel);
            return reinterpret_cast<LRESULT>(g_dialogBgBrush);
        }
        case WM_CLOSE:
            if (ds) CommitSoundSettings(ds);
            DestroyWindow(hwnd);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowSettingsDialog(DashboardState* st) {
    if (!st || !st->hwndMain) return;
    static bool regDone = false;
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!regDone) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = SettingsWndProc;
        wc.hInstance = hInst;
        wc.hIcon = LoadAppIcon();
        wc.hIconSm = LoadAppSmallIcon();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"ProcessCommanderSettingsWnd";
        RegisterClassExW(&wc);
        regDone = true;
    }

    HWND existing = FindWindowW(L"ProcessCommanderSettingsWnd", nullptr);
    if (existing) {
        ShowWindow(existing, SW_SHOW);
        SetForegroundWindow(existing);
        return;
    }

    auto* ds = new SettingsDlgState();
    ds->dash = st;
    RECT rcParent = {};
    GetWindowRect(st->hwndMain, &rcParent);
    int dlgW = 700;
    int dlgH = 450;
    int x = rcParent.left + ((rcParent.right - rcParent.left) - dlgW) / 2;
    int y = rcParent.top + ((rcParent.bottom - rcParent.top) - dlgH) / 2;

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"ProcessCommanderSettingsWnd",
        Tr(L"設定", L"Settings", L"Ajustes"),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, dlgW, dlgH,
        st->hwndMain, nullptr, hInst, ds
    );
    if (hDlg) {
        ShowWindow(hDlg, SW_SHOW);
        SetForegroundWindow(hDlg);
    } else {
        delete ds;
    }
}

// ---------------------------------------------------------------------------
// Token usage panel (docked below the KPI cards)
// ---------------------------------------------------------------------------
static const int kUsageRangeCount = 5;
static const int kUsageRangeSec[kUsageRangeCount] = {1800, 3600, 14400, 43200, 86400};
static const wchar_t* const kUsageRangeLabels[kUsageRangeCount] = {L"30m", L"1h", L"4h", L"12h", L"24h"};
static const COLORREF kUsagePalette[] = {
    RGB(0x4F, 0x8C, 0xFF), RGB(0x3D, 0xD5, 0x98), RGB(0xFF, 0xB4, 0x54),
    RGB(0xFF, 0x6B, 0x6B), RGB(0xB4, 0x7A, 0xFF), RGB(0x2E, 0xC4, 0xE0),
    RGB(0xF7, 0x8F, 0xB4), RGB(0x9C, 0xD6, 0x5B)
};
static const int kUsagePaletteCount = static_cast<int>(sizeof(kUsagePalette) / sizeof(kUsagePalette[0]));

static std::wstring FormatCompactTokens(uint64_t v) {
    wchar_t buf[32] = {};
    if (v >= 1000000ULL) swprintf_s(buf, L"%.1fM", v / 1000000.0);
    else if (v >= 1000ULL) swprintf_s(buf, L"%.1fk", v / 1000.0);
    else swprintf_s(buf, L"%llu", static_cast<unsigned long long>(v));
    return buf;
}

static void GetUsagePillRects(int width, RECT out[kUsageRangeCount], RECT& closeRc) {
    int x = 136;
    for (int i = 0; i < kUsageRangeCount; ++i) {
        out[i] = {x, 7, x + 40, 24};
        x += 44;
    }
    closeRc = {width - 28, 5, width - 8, 25};
}

static void DrawUsagePanel(HWND hwnd, HDC hdc, DashboardState* st) {
    RECT rc = {};
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    HBRUSH bg = CreateSolidBrush(g_theme.panel);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
    HPEN border = CreatePen(PS_SOLID, 1, g_theme.border);
    HGDIOBJ oldPen = SelectObject(hdc, border);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, 0, 0, w, h);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(border);

    SetBkMode(hdc, TRANSPARENT);
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, st->hFontUiBold ? st->hFontUiBold : GetStockObject(DEFAULT_GUI_FONT)));
    HBRUSH accent = CreateSolidBrush(g_theme.accent);
    RECT accentRc = {10, 8, 13, 22};
    FillRect(hdc, &accentRc, accent);
    DeleteObject(accent);
    SetTextColor(hdc, g_theme.text);
    RECT titleRc = {18, 6, 130, 24};
    DrawTextW(hdc, Tr(L"トークン使用量", L"TOKEN USAGE", L"USO DE TOKENS"), -1, &titleRc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(hdc, st->hFontUi ? st->hFontUi : GetStockObject(DEFAULT_GUI_FONT));
    int rangeIdx = st->usageRangeIndex;
    if (rangeIdx < 0) rangeIdx = 0;
    if (rangeIdx >= kUsageRangeCount) rangeIdx = kUsageRangeCount - 1;

    RECT pills[kUsageRangeCount] = {};
    RECT closeRc = {};
    GetUsagePillRects(w, pills, closeRc);
    for (int i = 0; i < kUsageRangeCount; ++i) {
        bool active = (i == rangeIdx);
        HBRUSH pb = CreateSolidBrush(active ? g_theme.accent : g_theme.panel);
        HPEN pp = CreatePen(PS_SOLID, 1, active ? g_theme.accent : g_theme.border);
        HGDIOBJ ob = SelectObject(hdc, pb);
        HGDIOBJ op = SelectObject(hdc, pp);
        RoundRect(hdc, pills[i].left, pills[i].top, pills[i].right, pills[i].bottom, 10, 10);
        SelectObject(hdc, op);
        SelectObject(hdc, ob);
        DeleteObject(pp);
        DeleteObject(pb);
        SetTextColor(hdc, active ? g_theme.windowBg : g_theme.muted);
        DrawTextW(hdc, kUsageRangeLabels[i], -1, &pills[i],
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SetTextColor(hdc, g_theme.muted);
    DrawTextW(hdc, L"\x2715", -1, &closeRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    int64_t now = static_cast<int64_t>(time(nullptr));
    int64_t since = now - kUsageRangeSec[rangeIdx];
    std::vector<LlmUsagePoint> points = GetLlmUsageHistory(since);

    struct KeyStat {
        std::string name;
        uint64_t total = 0;
        COLORREF color = RGB(128, 128, 128);
    };
    std::vector<KeyStat> keys;
    std::unordered_map<std::string, int> keyIndex;
    uint64_t rangeTotal = 0;
    double rangeCost = 0.0;
    for (const auto& p : points) {
        std::string name = p.agentType.empty() ? "unknown" : p.agentType;
        if (!p.model.empty()) name += " / " + p.model;
        auto it = keyIndex.find(name);
        int idx;
        if (it == keyIndex.end()) {
            idx = static_cast<int>(keys.size());
            keyIndex[name] = idx;
            KeyStat ks;
            ks.name = name;
            ks.color = kUsagePalette[idx % kUsagePaletteCount];
            keys.push_back(ks);
        } else {
            idx = it->second;
        }
        keys[idx].total += p.totalTokens;
        rangeTotal += p.totalTokens;
        rangeCost += p.costUsd;
    }

    SelectObject(hdc, st->hFontUiBold ? st->hFontUiBold : GetStockObject(DEFAULT_GUI_FONT));
    std::wstring summary = FormatCompactTokens(rangeTotal) + L" tok";
    if (rangeCost > 0.0) {
        wchar_t cbuf[32] = {};
        swprintf_s(cbuf, L"  \x00B7  $%.4f", rangeCost);
        summary += cbuf;
    }
    RECT sumRc = {closeRc.left - 270, 6, closeRc.left - 6, 24};
    SetTextColor(hdc, g_theme.accent);
    DrawTextW(hdc, summary.c_str(), -1, &sumRc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    int gx0 = 12;
    int splitX = 12 + (w - 24) * 62 / 100;
    int gx1 = splitX - 6;
    int mx0 = splitX + 6;
    int mx1 = w - 12;
    int gy0 = 30;
    int gy1 = h - 26;
    int graphW = gx1 - gx0;
    int graphH = gy1 - gy0;
    if (graphW < 40 || graphH < 20) {
        SelectObject(hdc, oldFont);
        return;
    }

    HPEN grid = CreatePen(PS_DOT, 1, g_theme.border);
    for (int g = 1; g <= 3; ++g) {
        int y = gy1 - graphH * g / 4;
        HGDIOBJ op = SelectObject(hdc, grid);
        MoveToEx(hdc, gx0, y, nullptr);
        LineTo(hdc, gx1, y);
        SelectObject(hdc, op);
    }
    DeleteObject(grid);

    if (points.empty() || keys.empty()) {
        SetTextColor(hdc, g_theme.muted);
        RECT emptyRc = {gx0, gy0, gx1, gy1};
        DrawTextW(hdc, Tr(L"まだ使用量データがありません", L"No token usage recorded yet", L"Aún no hay datos de uso"),
                  -1, &emptyRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
        return;
    }

    int nBuckets = graphW / 7;
    if (nBuckets < 12) nBuckets = 12;
    if (nBuckets > 96) nBuckets = 96;
    double bucketSec = static_cast<double>(kUsageRangeSec[rangeIdx]) / nBuckets;
    std::vector<std::vector<uint64_t>> vals(nBuckets, std::vector<uint64_t>(keys.size(), 0));
    std::vector<std::vector<uint64_t>> valIn(nBuckets, std::vector<uint64_t>(keys.size(), 0));
    std::vector<std::vector<uint64_t>> valOut(nBuckets, std::vector<uint64_t>(keys.size(), 0));
    std::vector<std::vector<uint64_t>> valThink(nBuckets, std::vector<uint64_t>(keys.size(), 0));
    std::vector<std::vector<uint64_t>> valCache(nBuckets, std::vector<uint64_t>(keys.size(), 0));
    std::vector<uint64_t> bucketTotal(nBuckets, 0);
    for (const auto& p : points) {
        int bi = static_cast<int>((p.epochSec - since) / bucketSec);
        if (bi < 0) bi = 0;
        if (bi >= nBuckets) bi = nBuckets - 1;
        std::string name = p.agentType.empty() ? "unknown" : p.agentType;
        if (!p.model.empty()) name += " / " + p.model;
        auto it = keyIndex.find(name);
        if (it == keyIndex.end()) continue;
        uint64_t pt = p.totalTokens;
        if (pt == 0) pt = p.inputTokens + p.outputTokens + p.thinkingTokens + p.cacheReadTokens + p.cacheWriteTokens;
        vals[bi][it->second] += pt;
        valIn[bi][it->second] += p.inputTokens;
        valOut[bi][it->second] += p.outputTokens;
        valThink[bi][it->second] += p.thinkingTokens;
        valCache[bi][it->second] += p.cacheReadTokens + p.cacheWriteTokens;
        bucketTotal[bi] += pt;
    }
    uint64_t maxBucket = 1;
    for (uint64_t v : bucketTotal) if (v > maxBucket) maxBucket = v;

    int slot = graphW / nBuckets;
    if (slot < 1) slot = 1;
    for (int bi = 0; bi < nBuckets; ++bi) {
        if (bucketTotal[bi] == 0) continue;
        int x0 = gx0 + bi * slot;
        int x1 = x0 + slot - 1;
        if (x1 <= x0) x1 = x0 + 1;
        int barH = static_cast<int>((static_cast<double>(bucketTotal[bi]) / maxBucket) * graphH);
        if (barH < 1) barH = 1;
        int used = 0;
        for (size_t k = 0; k < keys.size(); ++k) {
            uint64_t v = vals[bi][k];
            if (v == 0) continue;
            int sh = static_cast<int>((static_cast<double>(v) / bucketTotal[bi]) * barH);
            if (sh < 1) sh = 1;
            int remain = barH - used;
            if (sh > remain) sh = remain;
            if (sh <= 0) break;
            // Draw sub-segments by token kind (input / output / thinking / cache).
            struct Seg { uint64_t v; COLORREF c; };
            Seg segs[4] = {
                {valIn[bi][k], RGB(0x4F, 0x8C, 0xFF)},
                {valOut[bi][k], RGB(0x3D, 0xD5, 0x98)},
                {valThink[bi][k], RGB(0xB4, 0x7A, 0xFF)},
                {valCache[bi][k], RGB(0x5C, 0x6C, 0x8C)}
            };
            uint64_t segSum = valIn[bi][k] + valOut[bi][k] + valThink[bi][k] + valCache[bi][k];
            if (segSum == 0) segSum = v;
            int subUsed = 0;
            for (int s = 0; s < 4; ++s) {
                if (segs[s].v == 0) continue;
                int ssh = static_cast<int>((static_cast<double>(segs[s].v) / segSum) * sh);
                if (ssh < 1) ssh = 1;
                int subRemain = sh - subUsed;
                if (ssh > subRemain) ssh = subRemain;
                if (ssh <= 0) break;
                RECT segRc = {x0, gy1 - used - subUsed - ssh, x1, gy1 - used - subUsed};
                HBRUSH sb = CreateSolidBrush(segs[s].c);
                FillRect(hdc, &segRc, sb);
                DeleteObject(sb);
                subUsed += ssh;
                if (subUsed >= sh) break;
            }
            used += sh;
            if (used >= barH) break;
        }
    }
    // Per-provider/model horizontal bar chart on the right.
    {
        RECT mtitle = {mx0, gy0 - 14, mx1, gy0};
        SetTextColor(hdc, g_theme.muted);
        SelectObject(hdc, st->hFontUiBold ? st->hFontUiBold : GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(hdc, Tr(L"モデル別使用量", L"USAGE BY MODEL", L"USO POR MODELO"), -1, &mtitle,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        uint64_t maxModel = 1;
        for (const auto& k : keys) if (k.total > maxModel) maxModel = k.total;
        int n = static_cast<int>(keys.size());
        int availH = gy1 - gy0;
        int rowH = (n > 0) ? (availH / n) : availH;
        if (rowH < 11) rowH = 11;
        if (rowH > 18) rowH = 18;
        int labelW = (std::min)(150, mx1 - mx0 - 30);
        int barX0 = mx0 + labelW + 4;
        int y = gy0 + 2;
        SelectObject(hdc, st->hFontUi ? st->hFontUi : GetStockObject(DEFAULT_GUI_FONT));
        for (int i = 0; i < n && y + rowH <= gy1 + 4; ++i) {
            std::wstring lbl = Utf8ToWide(keys[i].name);
            RECT labRc = {mx0, y, mx0 + labelW, y + rowH};
            SetTextColor(hdc, g_theme.text);
            DrawTextW(hdc, lbl.c_str(), -1, &labRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            int barW = (int)((double)keys[i].total / maxModel * (mx1 - barX0));
            if (barW < 2) barW = 2;
            if (barX0 + barW > mx1) barW = mx1 - barX0;
            RECT barRc = {barX0, y + 2, barX0 + barW, y + rowH - 2};
            HBRUSH mb = CreateSolidBrush(keys[i].color);
            FillRect(hdc, &barRc, mb);
            DeleteObject(mb);

            std::wstring val = FormatCompactTokens(keys[i].total);
            RECT valRc = {barX0 + barW + 4, y, mx1, y + rowH};
            SetTextColor(hdc, g_theme.muted);
            DrawTextW(hdc, val.c_str(), -1, &valRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            y += rowH + 1;
        }
    }

    SetTextColor(hdc, g_theme.muted);
    RECT axisL = {gx0, gy1 + 2, gx0 + 80, gy1 + 16};
    RECT axisR = {gx1 - 80, gy1 + 2, gx1, gy1 + 16};
    wchar_t tbuf[32] = {};
    struct tm tmv = {};
    time_t stv = static_cast<time_t>(since);
    localtime_s(&tmv, &stv);
    swprintf_s(tbuf, L"%02d:%02d", tmv.tm_hour, tmv.tm_min);
    DrawTextW(hdc, tbuf, -1, &axisL, DT_LEFT | DT_SINGLELINE);
    DrawTextW(hdc, Tr(L"現在", L"now", L"ahora"), -1, &axisR, DT_RIGHT | DT_SINGLELINE);

    int lx = gx0;
    int ly = gy1 + 2;
    HFONT oldLegendFont = static_cast<HFONT>(SelectObject(hdc, st->hFontUi ? st->hFontUi : GetStockObject(DEFAULT_GUI_FONT)));
    for (size_t k = 0; k < keys.size(); ++k) {
        std::wstring label = Utf8ToWide(keys[k].name) + L" " + FormatCompactTokens(keys[k].total);
        SIZE sz = {};
        GetTextExtentPoint32W(hdc, label.c_str(), static_cast<int>(label.size()), &sz);
        if (lx + sz.cx + 16 > gx1) {
            if (k + 1 < keys.size()) {
                std::wstring more = L"+" + std::to_wstring(keys.size() - k);
                DrawTextW(hdc, more.c_str(), -1, &axisL, DT_LEFT | DT_SINGLELINE);
            }
            break;
        }
        HBRUSH dot = CreateSolidBrush(keys[k].color);
        RECT dotRc = {lx, ly + 5, lx + 7, ly + 12};
        FillRect(hdc, &dotRc, dot);
        DeleteObject(dot);
        RECT labRc = {lx + 10, ly, gx1, ly + 16};
        SetTextColor(hdc, g_theme.muted);
        DrawTextW(hdc, label.c_str(), -1, &labRc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        lx += sz.cx + 18;
    }
    SelectObject(hdc, oldLegendFont);
    SelectObject(hdc, oldFont);
}

static LRESULT CALLBACK UsagePanelWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<DashboardState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            SetTimer(hwnd, 1, 2000, nullptr);
            return 0;
        }
        case WM_TIMER:
            if (wParam == 1) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps = {};
            HDC hdc = BeginPaint(hwnd, &ps);
            if (st) DrawUsagePanel(hwnd, hdc, st);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (!st) break;
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            RECT pills[kUsageRangeCount] = {};
            RECT closeRc = {};
            GetUsagePillRects(rc.right, pills, closeRc);
            if (PtInRect(&closeRc, pt)) {
                st->showUsagePanel = false;
                RECT mainRc = {};
                GetClientRect(st->hwndMain, &mainRc);
                LayoutControls(st, mainRc.right - mainRc.left, mainRc.bottom - mainRc.top);
                InvalidateRect(st->hwndMain, nullptr, TRUE);
                return 0;
            }
            for (int i = 0; i < kUsageRangeCount; ++i) {
                if (PtInRect(&pills[i], pt)) {
                    st->usageRangeIndex = i;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            return 0;
        }
        case WM_NCDESTROY:
            KillTimer(hwnd, 1);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK DashboardWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DashboardState* st = GetState(hwnd);

    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            st = reinterpret_cast<DashboardState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
            st->hwndMain = hwnd;
            g_mainHwnd = hwnd;

            st->hFontUi = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            st->hFontUiBold = CreateFontW(-13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            st->hFontKpiVal = CreateFontW(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

            st->hwndBtnStart = CreateWindowExW(0, L"BUTTON", L"+ Launch (powershell)",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 160, 24, hwnd, (HMENU)IDC_BTN_START, nullptr, nullptr);
            st->hwndBtnSettings = CreateWindowExW(0, L"BUTTON", Tr(L"設定", L"Settings", L"Ajustes"),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 90, 24, hwnd, (HMENU)IDC_BTN_SETTINGS, nullptr, nullptr);
            st->hwndBtnKillAll = CreateWindowExW(0, L"BUTTON", L"Kill All",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 74, 24, hwnd, (HMENU)IDC_BTN_KILL_ALL, nullptr, nullptr);
            st->hwndBtnGridView = CreateWindowExW(0, L"BUTTON", L"▦",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 28, 24, hwnd, (HMENU)IDC_BTN_GRID_VIEW, nullptr, nullptr);
            st->hwndBtnToggleList = CreateWindowExW(0, L"BUTTON", L"☰",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 28, 24, hwnd, (HMENU)IDC_BTN_TOGGLE_LIST, nullptr, nullptr);
            st->hwndBtnToggleTerm = CreateWindowExW(0, L"BUTTON", L">_",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 30, 24, hwnd, (HMENU)IDC_BTN_TOGGLE_TERM, nullptr, nullptr);
            st->hwndBtnHideTray = CreateWindowExW(0, L"BUTTON", L"⤓",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 28, 24, hwnd, (HMENU)IDC_BTN_HIDE_TRAY, nullptr, nullptr);
            st->hwndBtnViewTasks = CreateWindowExW(0, L"BUTTON", L"Task Queue",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 150, 24, hwnd, (HMENU)IDC_BTN_VIEW_TASKS, nullptr, nullptr);
            st->hwndClock = CreateWindowExW(0, L"BUTTON", L"",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 80, 24, hwnd, (HMENU)IDC_CLOCK, nullptr, nullptr);
            st->hwndBtnUsage = CreateWindowExW(0, L"BUTTON", L"📊",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 28, 24, hwnd, (HMENU)IDC_BTN_USAGE, nullptr, nullptr);

            static bool usageClassRegistered = false;
            if (!usageClassRegistered) {
                WNDCLASSEXW wcUsage = {};
                wcUsage.cbSize = sizeof(wcUsage);
                wcUsage.style = CS_HREDRAW | CS_VREDRAW;
                wcUsage.lpfnWndProc = UsagePanelWndProc;
                wcUsage.hInstance = GetModuleHandleW(nullptr);
                wcUsage.hCursor = LoadCursorW(nullptr, IDC_ARROW);
                wcUsage.lpszClassName = L"ProcessCommanderUsagePanelWnd";
                RegisterClassExW(&wcUsage);
                usageClassRegistered = true;
            }
            st->hwndUsagePanel = CreateWindowExW(0, L"ProcessCommanderUsagePanelWnd", L"",
                WS_CHILD, 0, 0, 10, 10, hwnd, nullptr, GetModuleHandleW(nullptr), st);

            st->hwndToolbarTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (st->hwndToolbarTip) {
                SetWindowPos(st->hwndToolbarTip, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                SendMessageW(st->hwndToolbarTip, TTM_SETMAXTIPWIDTH, 0, 320);
                auto addTool = [&](HWND hCtrl) {
                    if (!hCtrl) return;
                    TOOLINFOW ti = {};
                    ti.cbSize = sizeof(ti);
                    ti.hwnd = hwnd;
                    ti.uId = reinterpret_cast<UINT_PTR>(hCtrl);
                    ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
                    ti.lpszText = const_cast<wchar_t*>(L"");
                    SendMessageW(st->hwndToolbarTip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
                };
                addTool(st->hwndBtnGridView);
                addTool(st->hwndBtnToggleList);
                addTool(st->hwndBtnToggleTerm);
                addTool(st->hwndBtnHideTray);
            }

            LoadShellConfig(st);
            UpdateToolbarLabels(st);
            SetWindowTextW(hwnd,
                Tr(L"ProcessCommander — マルチターミナル ConPTY ダッシュボード (Direct2D TerminalView)",
                   L"ProcessCommander — Multi-Terminal ConPTY Dashboard (Direct2D TerminalView)",
                   L"ProcessCommander — Panel de terminales ConPTY (Direct2D TerminalView)"));

            st->hwndList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                0, 0, 600, 160, hwnd, (HMENU)IDC_LIST_PROCS, nullptr, nullptr);
            ListView_SetExtendedListViewStyle(st->hwndList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
            ApplyListTheme(st);

            const int colWidths[13] = {56, 115, 125, 145, 165, 110, 95, 80, 80, 80, 250, 250, 120};
            for (int i = 0; i < 13; ++i) {
                LVCOLUMNW lvc = {};
                lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                lvc.pszText = const_cast<wchar_t*>(ColumnTitle(i));
                lvc.cx = colWidths[i];
                lvc.iSubItem = i;
                ListView_InsertColumn(st->hwndList, i, &lvc);
            }
            UpdateListHeaderSortIndicator(st);

            HWND hHeader = ListView_GetHeader(st->hwndList);
            if (hHeader) {
                for (int i = 0; i < 13; ++i) {
                    HDITEMW hdi = {};
                    hdi.mask = HDI_FORMAT;
                    if (Header_GetItem(hHeader, i, &hdi)) {
                        hdi.fmt |= HDF_OWNERDRAW;
                        Header_SetItem(hHeader, i, &hdi);
                    }
                }
                SetWindowSubclass(hHeader, HeaderSubclassProc, 1, 0);
            }
            SetWindowSubclass(st->hwndList, ListViewSubclassProc, 1, 0);

            st->hwndTaskList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                0, 0, 600, 160, hwnd, (HMENU)IDC_LIST_TASKS, nullptr, nullptr);
            ListView_SetExtendedListViewStyle(st->hwndTaskList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
            ListView_SetBkColor(st->hwndTaskList, g_theme.panel);
            ListView_SetTextBkColor(st->hwndTaskList, g_theme.panel);
            ListView_SetTextColor(st->hwndTaskList, g_theme.listText);
            {
                const int taskWidths[9] = {90, 350, 100, 100, 165, 90, 100, 190, 110};
                for (int i = 0; i < 9; ++i) {
                    LVCOLUMNW lvc = {};
                    lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                    lvc.pszText = const_cast<wchar_t*>(TaskColumnTitle(i));
                    lvc.cx = taskWidths[i];
                    lvc.iSubItem = i;
                    ListView_InsertColumn(st->hwndTaskList, i, &lvc);
                }
            }
            HWND hTaskHeader = ListView_GetHeader(st->hwndTaskList);
            if (hTaskHeader) {
                for (int i = 0; i < 9; ++i) {
                    HDITEMW hdi = {};
                    hdi.mask = HDI_FORMAT;
                    if (Header_GetItem(hTaskHeader, i, &hdi)) {
                        hdi.fmt |= HDF_OWNERDRAW;
                        Header_SetItem(hTaskHeader, i, &hdi);
                    }
                }
                SetWindowSubclass(hTaskHeader, HeaderSubclassProc, 1, 0);
            }
            SetWindowSubclass(st->hwndTaskList, ListViewSubclassProc, 1, 0);

            st->hwndTab = CreateWindowExW(0, WC_TABCONTROLW, L"",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_TABS | TCS_SINGLELINE | TCS_OWNERDRAWFIXED,
                0, 0, 600, 26, hwnd, (HMENU)IDC_TAB_TERMINALS, nullptr, nullptr);
            g_tabHwnd = st->hwndTab;
            SetWindowSubclass(st->hwndTab, TabSubclassProc, 1, 0);

            HWND uiCtrls[] = {
                st->hwndBtnStart, st->hwndBtnSettings, st->hwndBtnKillAll,
                st->hwndBtnGridView,
                st->hwndBtnToggleList, st->hwndBtnToggleTerm, st->hwndBtnHideTray, st->hwndClock,
                st->hwndBtnViewTasks, st->hwndList, st->hwndTaskList, st->hwndTab
            };
            for (HWND h : uiCtrls) {
                if (h) SendMessageW(h, WM_SETFONT, (WPARAM)st->hFontUi, TRUE);
            }

            st->gridView = std::make_unique<TerminalGridView>();
            st->gridView->Create(hwnd);

            st->hwndKpiTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (st->hwndKpiTip) {
                SetWindowPos(st->hwndKpiTip, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                SendMessageW(st->hwndKpiTip, TTM_SETMAXTIPWIDTH, 0, 420);
                for (int i = 0; i < kNumKpiCards; ++i) {
                    TOOLINFOW ti = {};
                    ti.cbSize = sizeof(ti);
                    ti.uFlags = TTF_SUBCLASS;
                    ti.hwnd = hwnd;
                    ti.uId = static_cast<UINT_PTR>(i + 1);
                    ti.rect = {0, 0, 0, 0};
                    ti.lpszText = const_cast<wchar_t*>(L"");
                    SendMessageW(st->hwndKpiTip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
                }
            }

            InitTrayIcon(st);
            UpdateClock(st);
            ApplyNativeThemeToWindow(hwnd);

            SetTimer(hwnd, TIMER_REFRESH_ID, 250, nullptr);
            SetTimer(hwnd, TIMER_KPI_ANIM_ID, 60, nullptr);
            RefreshDashboardData(st);
            return 0;
        }

        case WM_PC_CREATE_TERMVIEW: {
            auto* params = reinterpret_cast<CreateTermViewParams*>(lParam);
            if (!params || !st) return 0;

            auto* tv = new TerminalView();
            HWND hTv = tv->Create(hwnd);
            if (!hTv) {
                params->outError = L"TerminalView::Create failed";
                delete tv;
                return 0;
            }
            int paneW = (std::max)(200L, st->rcTerminalArea.right - st->rcTerminalArea.left);
            int paneH = (std::max)(120L, st->rcTerminalArea.bottom - st->rcTerminalArea.top - 28);
            tv->MoveAndResize(st->rcTerminalArea.left, st->rcTerminalArea.top + 26, paneW, paneH);
            tv->SetWorkingDirectory(params->cwd);
            tv->SetOutputObserver(params->onOutput);

            if (!tv->StartSession(params->resolvedCmd, CP_UTF8)) {
                params->outError = tv->LastError();
                DestroyWindow(hTv);
                delete tv;
                return 0;
            }
            params->outView = tv;
            return 1;
        }

        case WM_PC_DESTROY_TERMVIEW: {
            auto* tv = reinterpret_cast<TerminalView*>(wParam);
            if (tv) {
                if (tv->Hwnd() && IsWindow(tv->Hwnd())) {
                    DestroyWindow(tv->Hwnd());
                }
                delete tv;
            }
            return 0;
        }

        case WM_DEFERRED_FOCUS: {
            int procId = static_cast<int>(lParam);
            if (st && procId > 0) {
                st->selectedProcId = procId;
                RefreshDashboardData(st);
            }
            HWND hTarget = reinterpret_cast<HWND>(wParam);
            if (hTarget && IsWindow(hTarget)) {
                SetFocus(hTarget);
            }
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 1000;
            mmi->ptMinTrackSize.y = 220;
            return 0;
        }

        case WM_SIZE: {
            if (st) {
                LayoutControls(st, LOWORD(lParam), HIWORD(lParam));
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_TIMER: {
            if (wParam == TIMER_REFRESH_ID && st) {
                UpdateClock(st);
                RefreshDashboardData(st);
            } else if (wParam == TIMER_KPI_ANIM_ID && st) {
                if (st->activeRunningCount > 0) {
                    g_kpiBarPhase += 0.05f;
                    if (g_kpiBarPhase >= 1.0f) g_kpiBarPhase -= 1.0f;
                    InvalidateRect(hwnd, &st->kpiCardRects[1], FALSE);
                    InvalidateRect(hwnd, &st->kpiCardRects[8], FALSE);
                }
            }
            return 0;
        }

        case WM_PC_IPC_EVENT: {
            if (st) {
                int newId = static_cast<int>(wParam);
                if (newId > 0) {
                    st->selectedProcId = newId;
                }
                RefreshDashboardData(st);
            }
            return 0;
        }

        case WM_SETCURSOR: {
            if (st && LOWORD(lParam) == HTCLIENT) {
                POINT pt = {};
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                if ((st->rcRowsSplitter.bottom > st->rcRowsSplitter.top && PtInRect(&st->rcRowsSplitter, pt)) ||
                    (st->rcSplitter.bottom > st->rcSplitter.top && PtInRect(&st->rcSplitter, pt))) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                    return TRUE;
                }
            }
            break;
        }

        case WM_LBUTTONDOWN: {
            if (st) {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                if (st->rcRowsSplitter.bottom > st->rcRowsSplitter.top && PtInRect(&st->rcRowsSplitter, pt)) {
                    st->rowsSplitterDragging = true;
                    SetCapture(hwnd);
                    InvalidateRect(hwnd, &st->rcRowsSplitter, FALSE);
                    return 0;
                }
                if (st->rcSplitter.bottom > st->rcSplitter.top && PtInRect(&st->rcSplitter, pt)) {
                    st->splitterDragging = true;
                    SetCapture(hwnd);
                    InvalidateRect(hwnd, &st->rcSplitter, FALSE);
                    return 0;
                }
                for (const auto& tb : st->termBanners) {
                    if (PtInRect(&tb.rcBorder, pt)) {
                        st->selectedProcId = tb.procId;
                        RefreshDashboardData(st);
                        TerminalView* tv = st->pm->GetTerminalView(tb.procId);
                        if (tv && tv->Hwnd()) {
                            SetFocus(tv->Hwnd());
                        }
                        break;
                    }
                }
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (st && st->rowsSplitterDragging) {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                if (st->lastTotalListHeight >= kMinRowListH * 2) {
                    int relY = pt.y - st->processListTop;
                    float ratio = static_cast<float>(relY) / static_cast<float>(st->lastTotalListHeight);
                    ratio = (std::clamp)(ratio, static_cast<float>(kMinRowListH) / static_cast<float>(st->lastTotalListHeight),
                                         1.0f - static_cast<float>(kMinRowListH) / static_cast<float>(st->lastTotalListHeight));
                    st->procRowRatio = ratio;
                    RECT rcClient = {};
                    GetClientRect(hwnd, &rcClient);
                    LayoutControls(st, rcClient.right, rcClient.bottom);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            if (st && st->splitterDragging) {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                st->processListHeight = pt.y - st->processListTop;
                if (st->processListHeight < kMinListH) st->processListHeight = kMinListH;
                RECT rcClient = {};
                GetClientRect(hwnd, &rcClient);
                LayoutControls(st, rcClient.right, rcClient.bottom);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        }

        case WM_LBUTTONUP: {
            if (st && st->rowsSplitterDragging) {
                st->rowsSplitterDragging = false;
                if (GetCapture() == hwnd) ReleaseCapture();
                SaveShellConfig(st);
                InvalidateRect(hwnd, &st->rcRowsSplitter, FALSE);
                return 0;
            }
            if (st && st->splitterDragging) {
                st->splitterDragging = false;
                if (GetCapture() == hwnd) ReleaseCapture();
                SaveShellConfig(st);
                InvalidateRect(hwnd, &st->rcSplitter, FALSE);
                return 0;
            }
            break;
        }

        case WM_CAPTURECHANGED: {
            if (st) {
                st->splitterDragging = false;
                st->rowsSplitterDragging = false;
            }
            break;
        }

        case WM_RBUTTONDOWN: {
            if (st) {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                for (const auto& tb : st->termBanners) {
                    if (PtInRect(&tb.rcBorder, pt)) {
                        st->selectedProcId = tb.procId;
                        RefreshDashboardData(st);
                        POINT ptScreen = pt;
                        ClientToScreen(hwnd, &ptScreen);
                        ShowTaskContextMenu(st, tb.procId, ptScreen);
                        break;
                    }
                }
            }
            return 0;
        }

        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!dis) break;
            if (dis->CtlType == ODT_HEADER) {
                DrawHeaderItem(dis);
                return TRUE;
            }
            if (dis->CtlType == ODT_TAB) {
                wchar_t text[256] = {};
                TCITEMW tie = {};
                tie.mask = TCIF_TEXT;
                tie.pszText = text;
                tie.cchTextMax = 256;
                TabCtrl_GetItem(dis->hwndItem, dis->itemID, &tie);
                bool selected = (dis->itemState & ODS_SELECTED) != 0 ||
                                TabCtrl_GetCurSel(dis->hwndItem) == static_cast<int>(dis->itemID);
                COLORREF bg = selected ? g_theme.panel : g_theme.windowBg;
                HBRUSH bgBrush = CreateSolidBrush(bg);
                FillRect(dis->hDC, &dis->rcItem, bgBrush);
                DeleteObject(bgBrush);
                HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
                HGDIOBJ oldPen = SelectObject(dis->hDC, borderPen);
                HGDIOBJ oldBrush = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
                Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
                SelectObject(dis->hDC, oldBrush);
                SelectObject(dis->hDC, oldPen);
                DeleteObject(borderPen);
                if (selected) {
                    RECT bar = dis->rcItem;
                    bar.top = bar.bottom - 2;
                    HBRUSH accentBrush = CreateSolidBrush(g_theme.accent);
                    FillRect(dis->hDC, &bar, accentBrush);
                    DeleteObject(accentBrush);
                }
                SetBkMode(dis->hDC, TRANSPARENT);
                SetTextColor(dis->hDC, selected ? g_theme.accent : g_theme.text);
                HFONT oldFont = static_cast<HFONT>(SelectObject(dis->hDC,
                    st ? st->hFontUi : GetStockObject(DEFAULT_GUI_FONT)));
                RECT rcText = dis->rcItem;
                InflateRect(&rcText, -6, 0);
                DrawTextW(dis->hDC, text, -1, &rcText,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                SelectObject(dis->hDC, oldFont);
                return TRUE;
            }
            if (dis->CtlType != ODT_BUTTON) break;
            bool pressed = (dis->itemState & ODS_SELECTED) != 0;
            COLORREF bg = pressed ? g_theme.border : g_theme.panel;
            HBRUSH bgBrush = CreateSolidBrush(bg);
            FillRect(dis->hDC, &dis->rcItem, bgBrush);
            DeleteObject(bgBrush);
            HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
            HGDIOBJ oldPen = SelectObject(dis->hDC, borderPen);
            HGDIOBJ oldBrush = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
            Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
            SelectObject(dis->hDC, oldBrush);
            SelectObject(dis->hDC, oldPen);
            DeleteObject(borderPen);

            if (dis->itemState & ODS_FOCUS) {
                RECT rcFocus = dis->rcItem;
                InflateRect(&rcFocus, -2, -2);
                DrawFocusRect(dis->hDC, &rcFocus);
            }

            wchar_t text[256] = {};
            GetWindowTextW(dis->hwndItem, text, 256);
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, (dis->itemState & ODS_DISABLED) ? g_theme.muted : g_theme.text);
            HFONT oldFont = static_cast<HFONT>(SelectObject(dis->hDC, st ? st->hFontUi : GetStockObject(DEFAULT_GUI_FONT)));
            RECT rc = dis->rcItem;
            InflateRect(&rc, -4, 0);
            DrawTextW(dis->hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dis->hDC, oldFont);
            return TRUE;
        }

        case WM_COMMAND: {
            if (!st) return 0;
            int id = LOWORD(wParam);
            if (id == IDC_BTN_START) {
                LaunchFromToolbar(st);
            } else if (id == IDC_BTN_SETTINGS) {
                ShowSettingsDialog(st);
            } else if (id == IDC_BTN_KILL_SEL) {
                if (st->selectedProcId > 0) {
                    if (MessageBoxW(hwnd,
                            Tr(L"このプロセスツリーを強制終了しますか？", L"Are you sure you want to kill this process tree?", L"¿Seguro que desea terminar este árbol de procesos?"),
                            Tr(L"終了の確認", L"Confirm Kill", L"Confirmar cierre"), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                        std::string err;
                        st->pm->KillProcess(st->selectedProcId, err);
                        RefreshDashboardData(st);
                    }
                }
            } else if (id == IDC_BTN_KILL_ALL) {
                if (MessageBoxW(hwnd,
                        Tr(L"実行中のすべてのプロセスを強制終了しますか？", L"Are you sure you want to kill ALL running processes?", L"¿Seguro que desea terminar TODOS los procesos en ejecución?"),
                        Tr(L"全終了の確認", L"Confirm Kill All", L"Confirmar cierre total"), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                    st->pm->KillAllRunning();
                    RefreshDashboardData(st);
                }
            } else if (id == IDC_BTN_CLEAR) {
                if (MessageBoxW(hwnd,
                        Tr(L"終了済みプロセスの履歴を消去しますか？", L"Are you sure you want to clear finished process history?", L"¿Seguro que desea borrar el historial de procesos finalizados?"),
                        Tr(L"履歴消去の確認", L"Confirm Clear", L"Confirmar borrado"), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                    st->pm->ClearFinished();
                    RefreshDashboardData(st);
                }
            } else if (id == IDC_BTN_VIEW_TASKS) {
                if (st->listDisplayMode == ListDisplayMode::TwoRows) {
                    st->listDisplayMode = ListDisplayMode::ProcessOnly;
                } else if (st->listDisplayMode == ListDisplayMode::ProcessOnly) {
                    st->listDisplayMode = ListDisplayMode::TaskOnly;
                } else {
                    st->listDisplayMode = ListDisplayMode::TwoRows;
                }
                if (st->hideProcessList) {
                    st->hideProcessList = false;
                    UpdatePanelToggleLabels(st);
                }
                UpdateToolbarLabels(st);
                SaveShellConfig(st);
                RECT rcClient = {};
                GetClientRect(hwnd, &rcClient);
                LayoutControls(st, rcClient.right, rcClient.bottom);
                RefreshDashboardData(st);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (id == IDC_BTN_GRID_VIEW) {
                if (st->gridView) {
                    LayoutTerminalViews(st);
                    if (st->gridView->IsVisible()) st->gridView->Hide();
                    else st->gridView->Show();
                }
            } else if (id == IDC_BTN_TOGGLE_LIST) {
                st->hideProcessList = !st->hideProcessList;
                UpdatePanelToggleLabels(st);
                SaveShellConfig(st);
                RECT rcClient = {};
                GetClientRect(hwnd, &rcClient);
                LayoutControls(st, rcClient.right, rcClient.bottom);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (id == IDC_BTN_TOGGLE_TERM) {
                st->hideTerminalGrid = !st->hideTerminalGrid;
                UpdatePanelToggleLabels(st);
                SaveShellConfig(st);
                RECT rcClient = {};
                GetClientRect(hwnd, &rcClient);
                LayoutControls(st, rcClient.right, rcClient.bottom);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (id == IDC_BTN_USAGE) {
                st->showUsagePanel = !st->showUsagePanel;
                RECT rcClient = {};
                GetClientRect(hwnd, &rcClient);
                LayoutControls(st, rcClient.right, rcClient.bottom);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (id == IDC_BTN_HIDE_TRAY) {
                HideToTray(st);
            }
            return 0;
        }

        case WM_NOTIFY: {
            if (!st) return 0;
            auto* nmhdr = reinterpret_cast<NMHDR*>(lParam);
            if (nmhdr->idFrom == IDC_TAB_TERMINALS) {
                if (nmhdr->code == TCN_SELCHANGE && !g_suppressTabChange) {
                    int sel = TabCtrl_GetCurSel(st->hwndTab);
                    if (sel >= 0 && sel < static_cast<int>(g_appTabs.size())) {
                        int procId = static_cast<int>(reinterpret_cast<intptr_t>(g_appTabs[sel].data));
                        HWND hTv = g_appTabs[sel].hwnd;
                        if (procId > 0) {
                            st->selectedProcId = procId;
                            RefreshDashboardData(st);
                            if (hTv && IsWindow(hTv)) {
                                SetFocus(hTv);
                            }
                        }
                    }
                } else if (nmhdr->code == NM_RCLICK) {
                    POINT ptScreen = {};
                    GetCursorPos(&ptScreen);
                    TCHITTESTINFO hti = {};
                    hti.pt = ptScreen;
                    ScreenToClient(st->hwndTab, &hti.pt);
                    int idx = TabCtrl_HitTest(st->hwndTab, &hti);
                    if (idx >= 0 && idx < static_cast<int>(g_appTabs.size())) {
                        int procId = static_cast<int>(reinterpret_cast<intptr_t>(g_appTabs[idx].data));
                        if (procId > 0) {
                            st->selectedProcId = procId;
                            RefreshDashboardData(st);
                            ShowTaskContextMenu(st, procId, ptScreen);
                        }
                    }
                }
            } else if (nmhdr->idFrom == IDC_LIST_TASKS) {
                if (nmhdr->code == NM_RCLICK) {
                    auto* nmia = reinterpret_cast<NMITEMACTIVATE*>(lParam);
                    int idx = nmia->iItem;
                    if (idx >= 0 && idx < static_cast<int>(st->taskJobIds.size())) {
                        POINT ptScreen = {};
                        GetCursorPos(&ptScreen);
                        ShowTaskQueueContextMenu(st, st->taskJobIds[idx], ptScreen);
                    }
                } else if (nmhdr->code == NM_DBLCLK) {
                    auto* nmia = reinterpret_cast<NMITEMACTIVATE*>(lParam);
                    int idx = nmia->iItem;
                    if (idx >= 0 && idx < static_cast<int>(st->taskJobIds.size())) {
                        ShowTaskQueueJobDetails(st, st->taskJobIds[idx]);
                    }
                } else if (nmhdr->code == NM_CUSTOMDRAW) {
                    auto* lvcd = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
                    if (lvcd->nmcd.dwDrawStage == CDDS_PREPAINT) {
                        return CDRF_NOTIFYITEMDRAW;
                    }
                    if (lvcd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                        int idx = static_cast<int>(lvcd->nmcd.dwItemSpec);
                        std::string kind;
                        if (idx >= 0 && idx < static_cast<int>(st->taskJobStates.size())) {
                            kind = st->taskJobStates[idx];
                        }
                        lvcd->clrText = g_theme.listText;
                        if (kind == "running") {
                            lvcd->clrTextBk = MutedRowColor(g_theme.rowSelected);
                        } else if (kind == "timeout") {
                            lvcd->clrTextBk = MutedRowColor(g_theme.rowIdle);
                        } else if (kind == "failed") {
                            lvcd->clrTextBk = MutedRowColor(g_theme.rowFailed);
                        } else {
                            lvcd->clrTextBk = g_theme.panel;
                        }
                        return CDRF_NEWFONT;
                    }
                }
            } else if (nmhdr->idFrom == IDC_LIST_PROCS) {
                if (nmhdr->code == LVN_COLUMNCLICK) {
                    auto* nmlv = reinterpret_cast<NMLISTVIEW*>(lParam);
                    int clickedCol = nmlv->iSubItem;
                    if (clickedCol >= 0 && clickedCol < 13) {
                        if (st->sortColumn == clickedCol) {
                            st->sortAscending = !st->sortAscending;
                        } else {
                            st->sortColumn = clickedCol;
                            st->sortAscending = false;
                        }
                        UpdateListHeaderSortIndicator(st);
                        RefreshDashboardData(st);
                    }
                } else if (nmhdr->code == LVN_ITEMCHANGED) {
                    auto* nmlv = reinterpret_cast<NMLISTVIEW*>(lParam);
                    if ((nmlv->uChanged & LVIF_STATE) && (nmlv->uNewState & LVIS_SELECTED)) {
                        int procId = static_cast<int>(nmlv->lParam);
                        if (procId > 0 && procId != st->selectedProcId) {
                            st->selectedProcId = procId;
                            RefreshDashboardData(st);
                            TerminalView* tv = st->pm->GetTerminalView(procId);
                            if (tv && tv->Hwnd()) {
                                SetFocus(tv->Hwnd());
                            }
                        }
                    }
                } else if (nmhdr->code == NM_RCLICK) {
                    auto* nmia = reinterpret_cast<NMITEMACTIVATE*>(lParam);
                    int procId = st->selectedProcId;
                    if (nmia->iItem >= 0 && nmia->iItem < static_cast<int>(st->cachedSnapshots.size())) {
                        procId = st->cachedSnapshots[nmia->iItem].id;
                        st->selectedProcId = procId;
                        RefreshDashboardData(st);
                    }
                    if (procId > 0) {
                        POINT ptScreen = {};
                        GetCursorPos(&ptScreen);
                        ShowTaskContextMenu(st, procId, ptScreen);
                    }
                } else if (nmhdr->code == NM_DBLCLK) {
                    auto* nmia = reinterpret_cast<NMITEMACTIVATE*>(lParam);
                    int procId = st->selectedProcId;
                    if (nmia->iItem >= 0 && nmia->iItem < static_cast<int>(st->cachedSnapshots.size())) {
                        procId = st->cachedSnapshots[nmia->iItem].id;
                        st->selectedProcId = procId;
                    }
                    if (procId > 0) {
                        ShowTaskDetailsDialog(st, procId);
                    }
                } else if (nmhdr->code == NM_CUSTOMDRAW) {
                    auto* lvcd = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
                    if (lvcd->nmcd.dwDrawStage == CDDS_PREPAINT) {
                        return CDRF_NOTIFYITEMDRAW;
                    }
                    if (lvcd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                        int itemIdx = static_cast<int>(lvcd->nmcd.dwItemSpec);
                        if (itemIdx >= 0 && itemIdx < static_cast<int>(st->cachedSnapshots.size())) {
                            const auto& snap = st->cachedSnapshots[itemIdx];
                            bool selected = (lvcd->nmcd.uItemState & CDIS_SELECTED) != 0;
                            lvcd->clrTextBk = selected ? g_theme.rowSelected : GetStateRowBgColor(snap);
                            lvcd->clrText = g_theme.listText;
                        }
                        return CDRF_NOTIFYSUBITEMDRAW | CDRF_NEWFONT;
                    }
                    if (lvcd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
                        int itemIdx = static_cast<int>(lvcd->nmcd.dwItemSpec);
                        if (itemIdx >= 0 && itemIdx < static_cast<int>(st->cachedSnapshots.size())) {
                            const auto& snap = st->cachedSnapshots[itemIdx];
                            lvcd->clrText = (lvcd->iSubItem == 4 && snap.state == ProcState::Running)
                                                ? g_theme.muted : g_theme.listText;
                        }
                        return CDRF_NEWFONT;
                    }
                }
            }
            return 0;
        }

        case WM_PC_TRAYICON: {
            if (!st) return 0;
            UINT ev = LOWORD(lParam);
            if (ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK) {
                if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
                    SetForegroundWindow(hwnd);
                } else {
                    RestoreFromTray(st);
                }
            } else if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
                ShowTrayContextMenu(st);
            }
            return 0;
        }

        case WM_SYSCOMMAND: {
            if ((wParam & 0xFFF0) == SC_MINIMIZE && st) {
                HideToTray(st);
                return 0;
            }
            break;
        }

        case WM_SETTINGCHANGE:
        case 0x0320: { // WM_DWMCOLORIZATIONCOLORCHANGED
            if (st && g_uiTheme == UiTheme::System) {
                ApplyTheme();
                ApplyListTheme(st);
                ApplyNativeThemeToWindow(st->hwndMain);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        }

        case WM_CLOSE: {
            if (st && !st->allowExit) {
                HideToTray(st);
                return 0;
            }
            if (st) SaveShellConfig(st);
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_TERMINAL_PROGRESS: {
            if (st) {
                st->termProgressState = (lParam >= 0) ? 1 : 0;
                st->termProgressValue = (lParam >= 0) ? static_cast<int>(lParam / 100) : 0;
                InvalidateRect(hwnd, &st->rcStatusBar, FALSE);
            }
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PRINTCLIENT: {
            if (st && wParam) {
                DrawDashboardCanvas(st, reinterpret_cast<HDC>(wParam));
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            if (st) {
                DrawDashboardCanvas(st, hdc);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY: {
            KillTimer(hwnd, TIMER_REFRESH_ID);
            KillTimer(hwnd, TIMER_KPI_ANIM_ID);
            if (st) {
                RemoveTrayIcon(st);
                st->gridView.reset();
                if (st->hFontUi) DeleteObject(st->hFontUi);
                if (st->hFontUiBold) DeleteObject(st->hFontUiBold);
                if (st->hFontKpiVal) DeleteObject(st->hFontKpiVal);
            }
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int RunGuiDashboard(ProcessManager& pm, IpcServer& ipc, TaskQueueManager& tqm) {
    SetProcessDPIAware();

    INITCOMMONCONTROLSEX icex = {};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icex);

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    TerminalView::RegisterWindowClass(hInst);
    TerminalGridView::RegisterWindowClass(hInst);

    g_uiLanguage = DetectDefaultLanguage();
    ApplyTheme();

    DashboardState state;
    state.pm = &pm;
    state.ipc = &ipc;
    state.tqm = &tqm;
    state.guiThreadId = GetCurrentThreadId();
    state.dashboardStartTimeStr = GetCurrentTimestampStr();
    state.dashboardStartTickMs = GetNowTickMs();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DashboardWndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadAppIcon();
    wc.hIconSm = LoadAppSmallIcon();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"ProcessCommanderDashboardWnd";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"ProcessCommander — Multi-Terminal ConPTY Dashboard (Direct2D TerminalView)",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1460, 940,
        nullptr, nullptr, hInst, &state
    );
    if (!hwnd) return 1;

    pm.SetTerminalCallbacks(
        [hwnd](const std::wstring& resolvedCmd,
               const std::wstring& cwd,
               std::function<void(const char*, size_t)> onOutput,
               std::wstring& outError) -> TerminalView* {
            CreateTermViewParams params;
            params.resolvedCmd = resolvedCmd;
            params.cwd = cwd;
            params.onOutput = std::move(onOutput);
            SendMessageW(hwnd, WM_PC_CREATE_TERMVIEW, 0, reinterpret_cast<LPARAM>(&params));
            outError = params.outError;
            return params.outView;
        },
        [hwnd](TerminalView* tv) {
            if (tv && IsWindow(hwnd)) {
                PostMessageW(hwnd, WM_PC_DESTROY_TERMVIEW, reinterpret_cast<WPARAM>(tv), 0);
            }
        }
    );

    ipc.SetNotifyHwnd(hwnd);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    ApplyNativeThemeToWindow(hwnd);

    ACCEL accel = {};
    accel.fVirt = FVIRTKEY | FCONTROL;
    accel.key = 'T';
    accel.cmd = IDC_BTN_VIEW_TASKS;
    HACCEL hAccel = CreateAcceleratorTableW(&accel, 1);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (hAccel && TranslateAcceleratorW(hwnd, hAccel, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (hAccel) DestroyAcceleratorTable(hAccel);

    pm.SetTerminalCallbacks(nullptr, nullptr);
    ipc.SetNotifyHwnd(nullptr);
    return static_cast<int>(msg.wParam);
}

} // namespace pc
