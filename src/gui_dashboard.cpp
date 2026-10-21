#include "gui_dashboard.h"
#include "common.h"
#include "TerminalView.h"
#include "TerminalGridView.h"
#include <commctrl.h>
#include <windowsx.h>
#include <shellapi.h>
#include <sstream>
#include <iomanip>
#include <algorithm>

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

static bool UseEnglishUi() {
    static const bool english = [] {
        wchar_t buf[16] = {};
        DWORD n = GetEnvironmentVariableW(L"PC_UI_LANG", buf, 16);
        if (n > 0) return _wcsnicmp(buf, L"en", 2) == 0;
        return PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_JAPANESE;
    }();
    return english;
}

static const wchar_t* Tr(const wchar_t* ja, const wchar_t* en) {
    return UseEnglishUi() ? en : ja;
}

// Compact dashboard header / splitter layout constants
constexpr int kHeaderKpiTop      = 6;
constexpr int kHeaderKpiBottom   = 64;
constexpr int kToolbarTop        = 68;
constexpr int kToolbarH          = 24;
constexpr int kListTop           = 96;
constexpr int kSplitterH         = 6;
constexpr int kTabH              = 26;
constexpr int kStatusBarH        = 26;
constexpr int kMinListH          = 60;
constexpr int kMinTerminalH      = 200;

enum ControlId {
    IDC_BTN_START = 1005,
    IDC_BTN_KILL_SEL = 1006,
    IDC_BTN_KILL_ALL = 1007,
    IDC_BTN_CLEAR = 1008,
    IDC_LIST_PROCS = 1009,
    IDC_BTN_GRID_VIEW = 1015,
    IDC_TAB_TERMINALS = 1016,
    IDC_BTN_HIDE_TRAY = 1017,
    IDC_BTN_CONFIG = 1018,
    IDC_BTN_HIDE_ENDED = 1019,
    IDC_BTN_TOGGLE_LIST = 1020,
    IDC_BTN_TOGGLE_TERM = 1021,
    IDC_BTN_RESET_IDS = 1022,
    IDM_TRAY_RESTORE = 4001,
    IDM_TRAY_GRID_VIEW = 4002,
    IDM_TRAY_HIDE = 4003,
    IDM_TRAY_EXIT = 4004
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

struct DashboardState {
    ProcessManager* pm = nullptr;
    IpcServer* ipc = nullptr;
    DWORD guiThreadId = 0;

    HWND hwndMain = nullptr;
    HWND hwndBtnStart = nullptr;
    HWND hwndBtnConfig = nullptr;
    HWND hwndBtnKillSel = nullptr;
    HWND hwndBtnKillAll = nullptr;
    HWND hwndBtnClear = nullptr;
    HWND hwndBtnHideEnded = nullptr;
    HWND hwndBtnGridView = nullptr;
    HWND hwndBtnHideTray = nullptr;
    HWND hwndBtnToggleList = nullptr;
    HWND hwndBtnToggleTerm = nullptr;
    HWND hwndBtnResetIds = nullptr;
    HWND hwndList = nullptr;
    HWND hwndTab = nullptr;

    bool hideProcessList = false;
    bool hideTerminalGrid = false;

    ShellType defaultShell = ShellType::PowerShell;
    std::string cmdCommand = "cmd.exe";
    std::string powershellCommand = "powershell.exe";
    std::string bashCommand = "bash.exe";

    bool hideEndedProcesses = false;
    int sortColumn = 0;        // Default: Column 0 (ID)
    bool sortAscending = false; // Default: Descending (newest processes at the top)

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
    int processListHeight = 130;
    int processListTop = kListTop;
    bool splitterDragging = false;
    std::vector<TermSlotBanner> termBanners;
};

static DashboardState* GetState(HWND hwnd) {
    return reinterpret_cast<DashboardState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

static COLORREF GetStateBadgeColor(const ProcessSnapshot& s) {
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
            return (s.idleMs >= 10000) ? RGB(254, 243, 199) : RGB(220, 252, 231);
        case ProcState::Completed:
            return RGB(239, 246, 255);
        case ProcState::Failed:
        case ProcState::TimedOut:
        case ProcState::IdleTimedOut:
        case ProcState::Killed:
            return RGB(254, 226, 226);
    }
    return RGB(255, 255, 255);
}

static void DrawKpiCard(HDC hdc, const RECT& rc, const wchar_t* title, const std::wstring& value,
                        const std::wstring& subtext, COLORREF accentColor, HFONT hFontUi, HFONT hFontKpiVal) {
    HBRUSH bgBrush = CreateSolidBrush(RGB(30, 41, 59));
    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(51, 65, 85));
    HGDIOBJ oldBrush = SelectObject(hdc, bgBrush);
    HGDIOBJ oldPen = SelectObject(hdc, borderPen);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 8, 8);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(bgBrush);
    DeleteObject(borderPen);

    RECT rcStripe = {rc.left + 2, rc.top + 1, rc.right - 2, rc.top + 4};
    HBRUSH stripeBrush = CreateSolidBrush(accentColor);
    FillRect(hdc, &rcStripe, stripeBrush);
    DeleteObject(stripeBrush);

    SetBkMode(hdc, TRANSPARENT);

    RECT rcTitle = {rc.left + 10, rc.top + 4, rc.right - 6, rc.top + 19};
    SetTextColor(hdc, RGB(148, 163, 184));
    HGDIOBJ oldFont = SelectObject(hdc, hFontUi);
    DrawTextW(hdc, title, -1, &rcTitle, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT rcVal = {rc.left + 10, rc.top + 18, rc.right - 6, rc.top + 43};
    SetTextColor(hdc, RGB(248, 250, 252));
    SelectObject(hdc, hFontKpiVal);
    DrawTextW(hdc, value.c_str(), -1, &rcVal, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT rcSub = {rc.left + 10, rc.top + 41, rc.right - 6, rc.bottom - 2};
    SetTextColor(hdc, accentColor);
    SelectObject(hdc, hFontUi);
    DrawTextW(hdc, subtext.c_str(), -1, &rcSub, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(hdc, oldFont);
}

// Position the active tab's TerminalView Direct2D child window inside rcTerminalArea
// and synchronize the WC_TABCONTROLW tab bar (st->hwndTab / g_tabHwnd).
static void LayoutTerminalViews(DashboardState* st) {
    if (!st) return;

    g_appTabs.clear();
    std::vector<ProcessSnapshot> termProcs;
    int activeTabIdx = 0;

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
            }
            termProcs.push_back(s);
        }
    }

    if (!g_appTabs.empty()) {
        g_activeAppTab = activeTabIdx;
        st->selectedProcId = static_cast<int>(reinterpret_cast<intptr_t>(g_appTabs[g_activeAppTab].data));
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

    HBRUSH bgBrush = CreateSolidBrush(RGB(15, 23, 42));
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

    // 1. Top KPI Cards
    int kpiMargin = 10;
    int kpiGap = 8;
    int numCards = 6;
    int availW = width - kpiMargin * 2 - kpiGap * (numCards - 1);
    int cardW = (std::max)(110, availW / numCards);
    int cardTop = kHeaderKpiTop;
    int cardBottom = kHeaderKpiBottom;

    for (int i = 0; i < numCards; ++i) {
        RECT rcCard;
        rcCard.left = kpiMargin + i * (cardW + kpiGap);
        rcCard.right = (i == numCards - 1) ? (width - kpiMargin) : (rcCard.left + cardW);
        rcCard.top = cardTop;
        rcCard.bottom = cardBottom;

        if (i == 0) {
            DrawKpiCard(hdc, rcCard, L"TOTAL TERMINALS", std::to_wstring(totalCount),
                        L"Direct2D TerminalView + ConPTY", RGB(148, 163, 184), st->hFontUi, st->hFontKpiVal);
        } else if (i == 1) {
            DrawKpiCard(hdc, rcCard, L"ACTIVE RUNNING", std::to_wstring(activeRunning),
                        L"Receiving live PTY stream", RGB(34, 197, 94), st->hFontUi, st->hFontKpiVal);
        } else if (i == 2) {
            DrawKpiCard(hdc, rcCard, L"WAITING / IDLE", std::to_wstring(idleWaiting),
                        L"Quiet >= 10s (Waiting input)", RGB(245, 158, 11), st->hFontUi, st->hFontKpiVal);
        } else if (i == 3) {
            DrawKpiCard(hdc, rcCard, L"COMPLETED (OK)", std::to_wstring(completedOk),
                        L"Exit code 0", RGB(59, 130, 246), st->hFontUi, st->hFontKpiVal);
        } else if (i == 4) {
            DrawKpiCard(hdc, rcCard, L"FAILED / KILLED", std::to_wstring(failedOrKilled),
                        L"Error / Timeout / Kill", RGB(239, 68, 68), st->hFontUi, st->hFontKpiVal);
        } else if (i == 5) {
            std::wstring memStr = Utf8ToWide(FormatBytes(totalMemBytes));
            std::wstring sub = std::to_wstring(totalTreeProcs) + L" processes in Job Objects";
            DrawKpiCard(hdc, rcCard, L"TOTAL JOB MEMORY", memStr,
                        sub, RGB(168, 85, 247), st->hFontUi, st->hFontKpiVal);
        }
    }

    // Draggable splitter between the process list and the terminal area
    if (st->rcSplitter.bottom > st->rcSplitter.top) {
        HBRUSH spBrush = CreateSolidBrush(st->splitterDragging ? RGB(51, 65, 85) : RGB(30, 41, 59));
        FillRect(hdc, &st->rcSplitter, spBrush);
        DeleteObject(spBrush);

        int gripY = (st->rcSplitter.top + st->rcSplitter.bottom) / 2;
        int gripCx = (st->rcSplitter.left + st->rcSplitter.right) / 2;
        HPEN gripPen = CreatePen(PS_SOLID, 1, st->splitterDragging ? RGB(56, 189, 248) : RGB(100, 116, 139));
        HGDIOBJ oldGripPen = SelectObject(hdc, gripPen);
        for (int dx = -14; dx <= 14; dx += 14) {
            MoveToEx(hdc, gripCx + dx - 5, gripY, nullptr);
            LineTo(hdc, gripCx + dx + 5, gripY);
        }
        SelectObject(hdc, oldGripPen);
        DeleteObject(gripPen);
    }

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(203, 213, 225));
    HGDIOBJ oldFont = SelectObject(hdc, st->hFontUiBold);

    // 2. Draw Headers & Borders around the active TerminalView tab
    if (st->termBanners.empty()) {
        if (st->rcTerminalArea.bottom > st->rcTerminalArea.top) {
            RECT rcEmpty = st->rcTerminalArea;
            HBRUSH emptyBrush = CreateSolidBrush(RGB(17, 24, 39));
            HPEN emptyPen = CreatePen(PS_SOLID, 1, RGB(51, 65, 85));
            SelectObject(hdc, emptyBrush);
            SelectObject(hdc, emptyPen);
            RoundRect(hdc, rcEmpty.left, rcEmpty.top, rcEmpty.right, rcEmpty.bottom, 6, 6);
            DeleteObject(emptyBrush);
            DeleteObject(emptyPen);

            SetTextColor(hdc, RGB(148, 163, 184));
            SelectObject(hdc, st->hFontUiBold);
            DrawTextW(hdc, L"No active TerminalView sessions. Click [+ New Shell] above or start via Claude CLI / MCP.",
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
            COLORREF borderCol = isSelected ? RGB(56, 189, 248) : RGB(51, 65, 85);

            // Outer frame around TerminalView
            HPEN framePen = CreatePen(PS_SOLID, isSelected ? 2 : 1, borderCol);
            HBRUSH frameBrush = CreateSolidBrush(RGB(30, 41, 59));
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
            std::wstring stateStr = ProcStateToDisplayWide(snap->state, snap->idleMs >= 10000);
            DrawTextW(hdc, stateStr.c_str(), -1, &rcBadge, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            // Title text on left of banner (includes Start Time & Uptime HH:MM:SS)
            std::wstringstream ss;
            ss << L"#" << snap->id << L" [" << Utf8ToWide(snap->name) << L"]  PID: " << snap->rootPid
               << L" (" << snap->treeProcessCount << L" procs, " << Utf8ToWide(FormatBytes(snap->treeMemoryBytes))
               << L")  Start: " << Utf8ToWide(snap->startTimeStr)
               << L"  Uptime: " << Utf8ToWide(FormatUptimeHMS(snap->elapsedMs))
               << L"  —  " << Utf8ToWide(snap->commandLine);
            RECT rcTitle = {tb.rcBanner.left + 8, tb.rcBanner.top + 2, rcBadge.left - 8, tb.rcBanner.bottom - 2};
            SetTextColor(hdc, isSelected ? RGB(56, 189, 248) : RGB(226, 232, 240));
            DrawTextW(hdc, ss.str().c_str(), -1, &rcTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            // Terminal viewport backing (used when PrintWindow captures parent GDI surface)
            RECT rcTv = {tb.rcBorder.left + 2, tb.rcBanner.bottom, tb.rcBorder.right - 2, tb.rcBorder.bottom - 2};
            HBRUSH tvBrush = CreateSolidBrush(RGB(12, 16, 23));
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
                        SetTextColor(hdc, RGB(226, 232, 240));
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
    HBRUSH sbBrush = CreateSolidBrush(RGB(30, 41, 59));
    FillRect(hdc, &rcSb, sbBrush);
    DeleteObject(sbBrush);

    HPEN sbTopPen = CreatePen(PS_SOLID, 1, RGB(51, 65, 85));
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
        sbLeft << L"Selected: #" << selSnap->id << L" [" << Utf8ToWide(selSnap->name) << L"]  |  "
               << ProcStateToDisplayWide(selSnap->state, selSnap->idleMs >= 10000)
               << L"  |  " << Tr(L"起動時刻(Start): ", L"Start: ") << Utf8ToWide(selSnap->startTimeStr)
               << L"  |  " << Tr(L"稼働時間(Uptime): ", L"Uptime: ") << Utf8ToWide(FormatUptimeHMS(selSnap->elapsedMs));
        if (!selSnap->logFilePath.empty()) {
            sbLeft << L"  |  Log: " << Utf8ToWide(selSnap->logFilePath);
        }
        if (st->pm) {
            TerminalView* tv = st->pm->GetTerminalView(selSnap->id);
            if (tv && !tv->Title().empty()) {
                sbLeft << L"  |  Title: " << tv->Title();
            }
        }
        if (st->termProgressState > 0) {
            sbLeft << L"  |  Progress: " << st->termProgressValue << L"%";
        }
    } else {
        sbLeft << L"Ready — Logs saved to: " << GetLogsDirectoryWide();
    }

    uint64_t dashElapsedMs = GetNowTickMs() - st->dashboardStartTickMs;
    std::wstringstream sbRight;
    sbRight << Tr(L"Dashboard 起動時刻: ", L"Dashboard Started: ") << Utf8ToWide(st->dashboardStartTimeStr)
            << L"  |  " << Tr(L"稼働時間: ", L"Uptime: ") << Utf8ToWide(FormatUptimeHMS(dashElapsedMs))
            << L"  |  IPC: \\\\.\\pipe\\ProcessCommanderIPC";

    SelectObject(hdc, st->hFontUi);
    RECT rcSbRight = {rcSb.right - 520, rcSb.top + 2, rcSb.right - 10, rcSb.bottom - 2};
    SetTextColor(hdc, RGB(148, 163, 184));
    DrawTextW(hdc, sbRight.str().c_str(), -1, &rcSbRight, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT rcSbLeft = {rcSb.left + 10, rcSb.top + 2, rcSbRight.left - 12, rcSb.bottom - 2};
    SetTextColor(hdc, RGB(226, 232, 240));
    DrawTextW(hdc, sbLeft.str().c_str(), -1, &rcSbLeft, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(hdc, oldFont);
    BitBlt(hdcWindow, 0, 0, width, height, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, oldBm);
    DeleteObject(hbm);
    DeleteDC(hdc);
}

static const wchar_t* ColumnTitle(int index) {
    static const wchar_t* const ja[11] = {
        L"ID",
        L"Agent / Name",
        L"Status",
        L"Start Time (起動時刻)",
        L"Uptime (稼働時間)",
        L"PID (Tree)",
        L"Memory",
        L"CPU Time",
        L"Idle",
        L"Latest Output Preview",
        L"Command Line"
    };
    static const wchar_t* const en[11] = {
        L"ID",
        L"Agent / Name",
        L"Status",
        L"Start Time",
        L"Uptime",
        L"PID (Tree)",
        L"Memory",
        L"CPU Time",
        L"Idle",
        L"Latest Output Preview",
        L"Command Line"
    };
    return UseEnglishUi() ? en[index] : ja[index];
}

static void UpdateListHeaderSortIndicator(DashboardState* st) {
    if (!st || !st->hwndList) return;
    HWND hHeader = ListView_GetHeader(st->hwndList);
    for (int i = 0; i < 11; ++i) {
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

static void UpdateHideEndedButtonLabel(DashboardState* st) {
    if (!st || !st->hwndBtnHideEnded) return;
    SetWindowTextW(st->hwndBtnHideEnded,
        st->hideEndedProcesses ? Tr(L"終了履歴を表示 (Show Ended)", L"Show Ended")
                               : Tr(L"終了履歴を非表示 (Hide Ended)", L"Hide Ended"));
}

static void UpdatePanelToggleLabels(DashboardState* st) {
    if (!st) return;
    if (st->hwndBtnToggleList) {
        SetWindowTextW(st->hwndBtnToggleList, st->hideProcessList ? L"Show List" : L"Hide List");
    }
    if (st->hwndBtnToggleTerm) {
        SetWindowTextW(st->hwndBtnToggleTerm, st->hideTerminalGrid ? L"Show Terminal" : L"Hide Terminal");
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
            case 4: // Uptime
                cmp = (a.elapsedMs < b.elapsedMs) ? -1 : (a.elapsedMs > b.elapsedMs ? 1 : 0);
                break;
            case 5: // PID
                cmp = (a.rootPid < b.rootPid) ? -1 : (a.rootPid > b.rootPid ? 1 : 0);
                break;
            case 6: // Memory
                cmp = (a.treeMemoryBytes < b.treeMemoryBytes) ? -1 : (a.treeMemoryBytes > b.treeMemoryBytes ? 1 : 0);
                break;
            case 7: // CPU Time
                cmp = (a.cpuTimeMs < b.cpuTimeMs) ? -1 : (a.cpuTimeMs > b.cpuTimeMs ? 1 : 0);
                break;
            case 8: // Idle
                cmp = (a.idleMs < b.idleMs) ? -1 : (a.idleMs > b.idleMs ? 1 : 0);
                break;
            case 9: // Latest Output Preview
                cmp = _stricmp(a.lastLinePreview.c_str(), b.lastLinePreview.c_str());
                break;
            case 10: // Command Line
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

static void RefreshDashboardData(DashboardState* st) {
    if (!st || !st->pm) return;
    st->allSnapshots = st->pm->GetAllSnapshots();
    st->cachedSnapshots.clear();
    for (const auto& s : st->allSnapshots) {
        if (st->hideEndedProcesses && s.state != ProcState::Running) {
            continue;
        }
        st->cachedSnapshots.push_back(s);
    }

    SortSnapshots(st->cachedSnapshots, st->sortColumn, st->sortAscending);

    HWND hFocus = GetFocus();
    if (hFocus) {
        for (const auto& s : st->cachedSnapshots) {
            if (s.terminalHwnd && s.terminalHwnd == hFocus) {
                st->selectedProcId = s.id;
                break;
            }
        }
    }

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
        std::wstring statusStr = ProcStateToDisplayWide(s.state, s.idleMs >= 10000);
        if (s.state != ProcState::Running && s.exitCode != STILL_ACTIVE) {
            statusStr += L" (" + std::to_wstring(s.exitCode) + L")";
        }
        std::wstring startStr = Utf8ToWide(s.startTimeStr);
        std::wstring uptimeStr = Utf8ToWide(FormatUptimeHMS(s.elapsedMs));
        if (s.timeoutSec > 0) {
            uptimeStr += L" / " + std::to_wstring(s.timeoutSec) + L"s";
        }
        std::wstring pidStr = std::to_wstring(s.rootPid) + L" (" + std::to_wstring(s.treeProcessCount) + L")";
        std::wstring memStr = Utf8ToWide(FormatBytes(s.treeMemoryBytes));
        std::wstring cpuStr = Utf8ToWide(FormatUptimeHMS(s.cpuTimeMs));
        std::wstring idleStr = (s.state == ProcState::Running) ? Utf8ToWide(FormatDurationSec(s.idleMs)) : L"-";
        std::wstring previewStr = Utf8ToWide(s.lastLinePreview);
        std::wstring cmdStr = Utf8ToWide(s.commandLine);

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
        ListView_SetItemText(st->hwndList, i, 4, uptimeStr.data());
        ListView_SetItemText(st->hwndList, i, 5, pidStr.data());
        ListView_SetItemText(st->hwndList, i, 6, memStr.data());
        ListView_SetItemText(st->hwndList, i, 7, cpuStr.data());
        ListView_SetItemText(st->hwndList, i, 8, idleStr.data());
        ListView_SetItemText(st->hwndList, i, 9, previewStr.data());
        ListView_SetItemText(st->hwndList, i, 10, cmdStr.data());
    }

    SendMessageW(st->hwndList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(st->hwndList, nullptr, FALSE);
    InvalidateRect(st->hwndMain, nullptr, FALSE);
}

static void LayoutControls(DashboardState* st, int width, int height) {
    if (!st) return;
    int margin = 10;

    st->rcKpiArea = {margin, kHeaderKpiTop, width - margin, kHeaderKpiBottom};

    // Toolbar buttons flow into multiple rows when the window is too narrow.
    struct ToolbarButton { HWND hwnd; int w; };
    const ToolbarButton buttons[] = {
        {st->hwndBtnStart, 165},
        {st->hwndBtnConfig, 74},
        {st->hwndBtnKillSel, 82},
        {st->hwndBtnKillAll, 74},
        {st->hwndBtnClear, 195},
        {st->hwndBtnHideEnded, 215},
        {st->hwndBtnGridView, 94},
        {st->hwndBtnToggleList, 90},
        {st->hwndBtnToggleTerm, 112},
        {st->hwndBtnResetIds, 80},
        {st->hwndBtnHideTray, 150}
    };
    const int gap = 7;
    int xCur = margin;
    int yBar = kToolbarTop;
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
        st->rcSplitter = {};
    } else {
        ShowWindow(st->hwndList, SW_SHOW);
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
        MoveWindow(st->hwndList, margin, topList, width - margin * 2, listH, TRUE);
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
    ProcessSnapshot snap;
    std::wstring fullOutput;
    HFONT hFontUi = nullptr;
    HFONT hFontBold = nullptr;
    HWND hwndEditCmd = nullptr;
    HWND hwndEditInfo = nullptr;
    HWND hwndBtnCopyCmd = nullptr;
    HWND hwndBtnOpenLog = nullptr;
    HWND hwndBtnClose = nullptr;
};

static LRESULT CALLBACK TaskDetailsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ds = reinterpret_cast<TaskDetailsDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ds = reinterpret_cast<TaskDetailsDlgState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ds));

            HWND hLblCmd = CreateWindowExW(0, L"STATIC", Tr(L"Command Line (コマンドライン):", L"Command Line:"),
                WS_CHILD | WS_VISIBLE, 14, 12, 400, 20, hwnd, nullptr, nullptr, nullptr);

            std::wstring cmdW = Utf8ToWide(ds->snap.commandLine);
            ds->hwndEditCmd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", cmdW.c_str(),
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                14, 34, 730, 82, hwnd, (HMENU)2001, nullptr, nullptr);

            HWND hLblInfo = CreateWindowExW(0, L"STATIC", Tr(L"Task Details & Output Preview (タスク詳細・出力):", L"Task Details & Output Preview:"),
                WS_CHILD | WS_VISIBLE, 14, 124, 400, 20, hwnd, nullptr, nullptr, nullptr);

            std::wstringstream ss;
            ss << L"ID              : #" << ds->snap.id << L"\r\n"
               << L"Name            : " << Utf8ToWide(ds->snap.name) << L"\r\n"
               << L"Status          : " << ProcStateToDisplayWide(ds->snap.state, ds->snap.idleMs >= 10000);
            if (ds->snap.state != ProcState::Running && ds->snap.exitCode != STILL_ACTIVE) {
                ss << L" (Exit Code: " << ds->snap.exitCode << L")";
            }
            ss << L"\r\n"
               << L"Command Line    : " << Utf8ToWide(ds->snap.commandLine) << L"\r\n"
               << L"Working Dir     : " << (ds->snap.workingDir.empty() ? L"(default)" : Utf8ToWide(ds->snap.workingDir)) << L"\r\n"
               << L"Root PID        : " << ds->snap.rootPid << L" (" << ds->snap.treeProcessCount << L" processes in Job Object)\r\n"
               << L"Memory Usage    : " << Utf8ToWide(FormatBytes(ds->snap.treeMemoryBytes)) << L"\r\n"
               << L"Start Time      : " << Utf8ToWide(ds->snap.startTimeStr) << L"\r\n"
               << L"End Time        : " << (ds->snap.endTimeStr.empty() ? L"(running)" : Utf8ToWide(ds->snap.endTimeStr)) << L"\r\n"
               << Tr(L"Uptime (稼働時間): ", L"Uptime: ") << Utf8ToWide(FormatUptimeHMS(ds->snap.elapsedMs))
               << L" (" << Utf8ToWide(FormatDurationSec(ds->snap.elapsedMs)) << L")\r\n"
               << L"CPU Time        : " << Utf8ToWide(FormatUptimeHMS(ds->snap.cpuTimeMs)) << L"\r\n"
               << L"Log File        : " << Utf8ToWide(ds->snap.logFilePath) << L"\r\n"
               << L"Done File       : " << Utf8ToWide(ds->snap.doneFilePath) << L"\r\n"
               << L"--------------------------------------------------------------------------------\r\n"
               << ds->fullOutput;

            std::wstring infoW = ss.str();
            ds->hwndEditInfo = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", infoW.c_str(),
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
                14, 146, 730, 310, hwnd, (HMENU)2002, nullptr, nullptr);

            ds->hwndBtnCopyCmd = CreateWindowExW(0, L"BUTTON", Tr(L"コマンドラインをコピー (Copy Command Line)", L"Copy Command Line"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 14, 468, 250, 30, hwnd, (HMENU)2003, nullptr, nullptr);
            ds->hwndBtnOpenLog = CreateWindowExW(0, L"BUTTON", Tr(L"ログファイルを開く (Open Log File)", L"Open Log File"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 274, 468, 220, 30, hwnd, (HMENU)2004, nullptr, nullptr);
            ds->hwndBtnClose = CreateWindowExW(0, L"BUTTON", Tr(L"閉じる (Close)", L"Close"),
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 634, 468, 110, 30, hwnd, (HMENU)IDOK, nullptr, nullptr);

            SendMessageW(hLblCmd, WM_SETFONT, (WPARAM)ds->hFontBold, TRUE);
            SendMessageW(hLblInfo, WM_SETFONT, (WPARAM)ds->hFontBold, TRUE);
            HWND ctrls[] = {ds->hwndEditCmd, ds->hwndEditInfo, ds->hwndBtnCopyCmd, ds->hwndBtnOpenLog, ds->hwndBtnClose};
            for (HWND h : ctrls) {
                SendMessageW(h, WM_SETFONT, (WPARAM)ds->hFontUi, TRUE);
            }
            return 0;
        }
        case WM_SIZE: {
            if (!ds) return 0;
            int w = LOWORD(lParam);
            int h = HIWORD(lParam);
            int pad = 14;
            int editW = (std::max)(200, w - pad * 2);
            MoveWindow(ds->hwndEditCmd, pad, 34, editW, 82, TRUE);
            int btnY = (std::max)(220, h - 42);
            MoveWindow(ds->hwndEditInfo, pad, 146, editW, (std::max)(80, btnY - 156), TRUE);
            MoveWindow(ds->hwndBtnCopyCmd, pad, btnY, 250, 30, TRUE);
            MoveWindow(ds->hwndBtnOpenLog, pad + 260, btnY, 220, 30, TRUE);
            MoveWindow(ds->hwndBtnClose, (std::max)(pad + 490, w - pad - 110), btnY, 110, 30, TRUE);
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
            } else if (id == IDOK || id == IDCANCEL) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            delete ds;
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
    ds->snap = snap;
    ds->hFontUi = st->hFontUi;
    ds->hFontBold = st->hFontUiBold;
    std::string outUtf8;
    st->pm->GetOutput(procId, true, 120, outUtf8);
    ds->fullOutput = NormalizeCrLfWide(outUtf8);

    std::wstring title = std::wstring(Tr(L"Task Details (タスク詳細)", L"Task Details")) + L" — #" + std::to_wstring(snap.id) + L" [" + Utf8ToWide(snap.name) + L"]";
    RECT rcParent = {};
    GetWindowRect(st->hwndMain, &rcParent);
    int dlgW = 775;
    int dlgH = 550;
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

static void ShowTaskContextMenu(DashboardState* st, int procId, POINT ptScreen) {
    if (!st || !st->pm || procId <= 0) return;
    ProcessSnapshot snap;
    if (!st->pm->GetSnapshot(procId, snap)) return;

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, 3001, Tr(L"詳細を表示 (Task Details & Command Line)...", L"Task Details & Command Line..."));
    AppendMenuW(hMenu, MF_STRING, 3002, Tr(L"コマンドラインをコピー (Copy Command Line)", L"Copy Command Line"));
    if (!snap.logFilePath.empty()) {
        AppendMenuW(hMenu, MF_STRING, 3003, Tr(L"ログファイルを開く (Open Log File)", L"Open Log File"));
    }
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    if (snap.state == ProcState::Running) {
        AppendMenuW(hMenu, MF_STRING, 3004, Tr(L"プロセスツリーを強制終了 (Kill Process Tree)", L"Kill Process Tree"));
    } else {
        AppendMenuW(hMenu, MF_STRING, 3005, Tr(L"この履歴を削除 (Remove This Entry)", L"Remove This Entry"));
        AppendMenuW(hMenu, MF_STRING, 3006, Tr(L"終了履歴をすべて消去 (Clear All Ended History)", L"Clear All Ended History"));
    }

    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, ptScreen.x, ptScreen.y, 0, st->hwndMain, nullptr);
    DestroyMenu(hMenu);

    if (cmd == 3001) {
        ShowTaskDetailsDialog(st, procId);
    } else if (cmd == 3002) {
        CopyTextToClipboard(st->hwndMain, Utf8ToWide(snap.commandLine));
    } else if (cmd == 3003) {
        std::wstring logW = Utf8ToWide(snap.logFilePath);
        ShellExecuteW(st->hwndMain, L"open", L"notepad.exe", logW.c_str(), nullptr, SW_SHOWNORMAL);
    } else if (cmd == 3004) {
        std::string err;
        st->pm->KillProcess(procId, err);
        RefreshDashboardData(st);
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
    st->hideTerminalGrid = JsonGetBool(content, "hide_terminal_grid", false);
    int listH = static_cast<int>(JsonGetInt(content, "process_list_height", st->processListHeight));
    if (listH >= kMinListH && listH <= 4000) st->processListHeight = listH;
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
       << "  \"process_list_height\": " << st->processListHeight << "\n"
       << "}\n";

    WriteFileAtomicWide(GetConfigFilePathWide(), ss.str());
}

static void UpdateLaunchButtonLabel(DashboardState* st) {
    if (!st || !st->hwndBtnStart) return;
    const wchar_t* label = L"+ Launch (powershell)";
    if (st->defaultShell == ShellType::Cmd) label = L"+ Launch (cmd)";
    else if (st->defaultShell == ShellType::Bash) label = L"+ Launch (bash)";
    SetWindowTextW(st->hwndBtnStart, label);
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
};

static void ApplyShellConfigFromDialog(HWND hwnd, ShellConfigDlgState* cs) {
    if (!cs || !cs->dash) return;
    if (SendMessageW(cs->hwndRadCmd, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        cs->dash->defaultShell = ShellType::Cmd;
    } else if (SendMessageW(cs->hwndRadBash, BM_GETCHECK, 0, 0) == BST_CHECKED) {
        cs->dash->defaultShell = ShellType::Bash;
    } else {
        cs->dash->defaultShell = ShellType::PowerShell;
    }

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
                Tr(L"Launch Terminal のデフォルトシェル選択と起動コマンド設定:",
                   L"Default shell and startup command for Launch Terminal:"),
                WS_CHILD | WS_VISIBLE, 16, 14, 540, 20, hwnd, nullptr, nullptr, nullptr);

            cs->hwndRadCmd = CreateWindowExW(0, L"BUTTON", L"cmd (Command Prompt)",
                WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
                16, 44, 190, 24, hwnd, (HMENU)5001, nullptr, nullptr);
            cs->hwndRadPs = CreateWindowExW(0, L"BUTTON", L"powershell (PowerShell)",
                WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                16, 84, 190, 24, hwnd, (HMENU)5002, nullptr, nullptr);
            cs->hwndRadBash = CreateWindowExW(0, L"BUTTON", L"bash (Git Bash / WSL)",
                WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
                16, 124, 190, 24, hwnd, (HMENU)5003, nullptr, nullptr);

            cs->hwndEditCmd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", Utf8ToWide(st->cmdCommand).c_str(),
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 212, 44, 260, 24, hwnd, (HMENU)5011, nullptr, nullptr);
            cs->hwndEditPs = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", Utf8ToWide(st->powershellCommand).c_str(),
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 212, 84, 260, 24, hwnd, (HMENU)5012, nullptr, nullptr);
            cs->hwndEditBash = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", Utf8ToWide(st->bashCommand).c_str(),
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 212, 124, 260, 24, hwnd, (HMENU)5013, nullptr, nullptr);

            HWND hBtnRunCmd = CreateWindowExW(0, L"BUTTON", Tr(L"cmd を起動", L"Launch cmd"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 480, 44, 140, 24, hwnd, (HMENU)5021, nullptr, nullptr);
            HWND hBtnRunPs = CreateWindowExW(0, L"BUTTON", Tr(L"powershell 起動", L"Launch powershell"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 480, 84, 140, 24, hwnd, (HMENU)5022, nullptr, nullptr);
            HWND hBtnRunBash = CreateWindowExW(0, L"BUTTON", Tr(L"bash を起動", L"Launch bash"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 480, 124, 140, 24, hwnd, (HMENU)5023, nullptr, nullptr);

            if (st->defaultShell == ShellType::Cmd) {
                SendMessageW(cs->hwndRadCmd, BM_SETCHECK, BST_CHECKED, 0);
            } else if (st->defaultShell == ShellType::Bash) {
                SendMessageW(cs->hwndRadBash, BM_SETCHECK, BST_CHECKED, 0);
            } else {
                SendMessageW(cs->hwndRadPs, BM_SETCHECK, BST_CHECKED, 0);
            }

            HWND hBtnSave = CreateWindowExW(0, L"BUTTON", Tr(L"保存して閉じる (Save)", L"Save and Close"),
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 330, 170, 140, 30, hwnd, (HMENU)IDOK, nullptr, nullptr);
            HWND hBtnCancel = CreateWindowExW(0, L"BUTTON", Tr(L"キャンセル", L"Cancel"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 480, 170, 140, 30, hwnd, (HMENU)IDCANCEL, nullptr, nullptr);

            SendMessageW(hTitle, WM_SETFONT, (WPARAM)st->hFontUiBold, TRUE);
            HWND ctrls[] = {
                cs->hwndRadCmd, cs->hwndRadPs, cs->hwndRadBash,
                cs->hwndEditCmd, cs->hwndEditPs, cs->hwndEditBash,
                hBtnRunCmd, hBtnRunPs, hBtnRunBash, hBtnSave, hBtnCancel
            };
            for (HWND h : ctrls) {
                SendMessageW(h, WM_SETFONT, (WPARAM)st->hFontUi, TRUE);
            }
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDOK && cs) {
                ApplyShellConfigFromDialog(hwnd, cs);
                DestroyWindow(hwnd);
            } else if (id == IDCANCEL) {
                DestroyWindow(hwnd);
            } else if ((id == 5021 || id == 5022 || id == 5023) && cs && cs->dash) {
                if (id == 5021) {
                    SendMessageW(cs->hwndRadCmd, BM_SETCHECK, BST_CHECKED, 0);
                    SendMessageW(cs->hwndRadPs, BM_SETCHECK, BST_UNCHECKED, 0);
                    SendMessageW(cs->hwndRadBash, BM_SETCHECK, BST_UNCHECKED, 0);
                } else if (id == 5022) {
                    SendMessageW(cs->hwndRadCmd, BM_SETCHECK, BST_UNCHECKED, 0);
                    SendMessageW(cs->hwndRadPs, BM_SETCHECK, BST_CHECKED, 0);
                    SendMessageW(cs->hwndRadBash, BM_SETCHECK, BST_UNCHECKED, 0);
                } else if (id == 5023) {
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
        Tr(L"Shell Config (cmd / powershell / bash 起動設定)", L"Shell Config (cmd / powershell / bash)"),
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
    AppendMenuW(hMenu, MF_STRING | MF_DEFAULT, IDM_TRAY_RESTORE, Tr(L"ダッシュボードを拡大・表示 (Restore Dashboard)", L"Restore Dashboard"));
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_GRID_VIEW, Tr(L"グリッド表示を開く (Open Grid View)", L"Open Grid View"));
    if (IsWindowVisible(st->hwndMain)) {
        AppendMenuW(hMenu, MF_STRING, IDM_TRAY_HIDE, Tr(L"タスクトレイに格納 (Hide to Tray)", L"Hide to Tray"));
    }
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, Tr(L"ProcessCommander を完全終了 (Exit)", L"Exit ProcessCommander"));

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
    } else if (cmd == IDM_TRAY_HIDE) {
        HideToTray(st);
    } else if (cmd == IDM_TRAY_EXIT) {
        st->allowExit = true;
        DestroyWindow(st->hwndMain);
    }
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
            st->hFontKpiVal = CreateFontW(-19, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

            st->hwndBtnStart = CreateWindowExW(0, L"BUTTON", L"+ Launch (powershell)",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 160, 24, hwnd, (HMENU)IDC_BTN_START, nullptr, nullptr);
            st->hwndBtnConfig = CreateWindowExW(0, L"BUTTON", L"Config",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 74, 24, hwnd, (HMENU)IDC_BTN_CONFIG, nullptr, nullptr);
            st->hwndBtnKillSel = CreateWindowExW(0, L"BUTTON", L"Kill Tree",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 82, 24, hwnd, (HMENU)IDC_BTN_KILL_SEL, nullptr, nullptr);
            st->hwndBtnKillAll = CreateWindowExW(0, L"BUTTON", L"Kill All",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 74, 24, hwnd, (HMENU)IDC_BTN_KILL_ALL, nullptr, nullptr);
            st->hwndBtnClear = CreateWindowExW(0, L"BUTTON", Tr(L"終了履歴を消去 (Clear Ended)", L"Clear Ended"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 165, 24, hwnd, (HMENU)IDC_BTN_CLEAR, nullptr, nullptr);
            st->hwndBtnHideEnded = CreateWindowExW(0, L"BUTTON", Tr(L"終了履歴を非表示 (Hide Ended)", L"Hide Ended"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 185, 24, hwnd, (HMENU)IDC_BTN_HIDE_ENDED, nullptr, nullptr);
            st->hwndBtnGridView = CreateWindowExW(0, L"BUTTON", L"Grid View",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 94, 24, hwnd, (HMENU)IDC_BTN_GRID_VIEW, nullptr, nullptr);
            st->hwndBtnToggleList = CreateWindowExW(0, L"BUTTON", L"Hide List",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 90, 24, hwnd, (HMENU)IDC_BTN_TOGGLE_LIST, nullptr, nullptr);
            st->hwndBtnToggleTerm = CreateWindowExW(0, L"BUTTON", L"Hide Terminal",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 112, 24, hwnd, (HMENU)IDC_BTN_TOGGLE_TERM, nullptr, nullptr);
            st->hwndBtnResetIds = CreateWindowExW(0, L"BUTTON", L"Reset IDs",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 80, 24, hwnd, (HMENU)IDC_BTN_RESET_IDS, nullptr, nullptr);
            st->hwndBtnHideTray = CreateWindowExW(0, L"BUTTON", Tr(L"トレイに格納 (Tray)", L"Tray"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 150, 24, hwnd, (HMENU)IDC_BTN_HIDE_TRAY, nullptr, nullptr);

            LoadShellConfig(st);
            UpdateLaunchButtonLabel(st);
            UpdateHideEndedButtonLabel(st);
            UpdatePanelToggleLabels(st);

            st->hwndList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                0, 0, 600, 160, hwnd, (HMENU)IDC_LIST_PROCS, nullptr, nullptr);
            ListView_SetExtendedListViewStyle(st->hwndList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

            const int colWidths[11] = {56, 115, 125, 145, 110, 95, 80, 80, 60, 270, 280};
            for (int i = 0; i < 11; ++i) {
                LVCOLUMNW lvc = {};
                lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                lvc.pszText = const_cast<wchar_t*>(ColumnTitle(i));
                lvc.cx = colWidths[i];
                lvc.iSubItem = i;
                ListView_InsertColumn(st->hwndList, i, &lvc);
            }
            UpdateListHeaderSortIndicator(st);

            st->hwndTab = CreateWindowExW(0, WC_TABCONTROLW, L"",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_TABS | TCS_SINGLELINE,
                0, 0, 600, 26, hwnd, (HMENU)IDC_TAB_TERMINALS, nullptr, nullptr);
            g_tabHwnd = st->hwndTab;

            HWND uiCtrls[] = {
                st->hwndBtnStart, st->hwndBtnConfig, st->hwndBtnKillSel, st->hwndBtnKillAll,
                st->hwndBtnClear, st->hwndBtnHideEnded, st->hwndBtnGridView,
                st->hwndBtnToggleList, st->hwndBtnToggleTerm, st->hwndBtnResetIds, st->hwndBtnHideTray,
                st->hwndList, st->hwndTab
            };
            for (HWND h : uiCtrls) {
                if (h) SendMessageW(h, WM_SETFONT, (WPARAM)st->hFontUi, TRUE);
            }

            st->gridView = std::make_unique<TerminalGridView>();
            st->gridView->Create(hwnd);

            InitTrayIcon(st);

            SetTimer(hwnd, TIMER_REFRESH_ID, 250, nullptr);
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
            mmi->ptMinTrackSize.y = 780;
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
                RefreshDashboardData(st);
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
                if (PtInRect(&st->rcSplitter, pt)) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                    return TRUE;
                }
            }
            break;
        }

        case WM_LBUTTONDOWN: {
            if (st) {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                if (PtInRect(&st->rcSplitter, pt)) {
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
            if (st && st->splitterDragging) {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                st->processListHeight = pt.y - st->processListTop;
                RECT rcClient = {};
                GetClientRect(hwnd, &rcClient);
                LayoutControls(st, rcClient.right, rcClient.bottom);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        }

        case WM_LBUTTONUP: {
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
            if (st) st->splitterDragging = false;
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

        case WM_COMMAND: {
            if (!st) return 0;
            int id = LOWORD(wParam);
            if (id == IDC_BTN_START) {
                LaunchFromToolbar(st);
            } else if (id == IDC_BTN_CONFIG) {
                ShowShellConfigDialog(st);
            } else if (id == IDC_BTN_KILL_SEL) {
                if (st->selectedProcId > 0) {
                    if (MessageBoxW(hwnd, L"Are you sure you want to kill this process tree?", L"Confirm Kill", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                        std::string err;
                        st->pm->KillProcess(st->selectedProcId, err);
                        RefreshDashboardData(st);
                    }
                }
            } else if (id == IDC_BTN_KILL_ALL) {
                if (MessageBoxW(hwnd, L"Are you sure you want to kill ALL running processes?", L"Confirm Kill All", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    st->pm->KillAllRunning();
                    RefreshDashboardData(st);
                }
            } else if (id == IDC_BTN_CLEAR) {
                if (MessageBoxW(hwnd, L"Are you sure you want to clear finished process history?", L"Confirm Clear", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    st->pm->ClearFinished();
                    RefreshDashboardData(st);
                }
            } else if (id == IDC_BTN_HIDE_ENDED) {
                st->hideEndedProcesses = !st->hideEndedProcesses;
                UpdateHideEndedButtonLabel(st);
                SaveShellConfig(st);
                RefreshDashboardData(st);
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
            } else if (id == IDC_BTN_RESET_IDS) {
                if (MessageBoxW(hwnd,
                        L"Reset process IDs? Finished processes will be renumbered from #1.\n"
                        L"All running processes must be stopped first.",
                        L"Reset Process IDs", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    std::string err;
                    int count = st->pm->ResetProcessIds(err);
                    if (count < 0) {
                        MessageBoxW(hwnd, Utf8ToWide(err).c_str(), L"Reset Process IDs", MB_ICONWARNING);
                    } else {
                        st->selectedProcId = -1;
                        RefreshDashboardData(st);
                    }
                }
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
            } else if (nmhdr->idFrom == IDC_LIST_PROCS) {
                if (nmhdr->code == LVN_COLUMNCLICK) {
                    auto* nmlv = reinterpret_cast<NMLISTVIEW*>(lParam);
                    int clickedCol = nmlv->iSubItem;
                    if (clickedCol >= 0 && clickedCol < 10) {
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
                            lvcd->clrTextBk = GetStateRowBgColor(snap);
                            lvcd->clrText = RGB(15, 23, 42);
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

        case WM_CLOSE: {
            if (st && !st->allowExit) {
                HideToTray(st);
                return 0;
            }
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

int RunGuiDashboard(ProcessManager& pm, IpcServer& ipc) {
    SetProcessDPIAware();

    INITCOMMONCONTROLSEX icex = {};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    TerminalView::RegisterWindowClass(hInst);
    TerminalGridView::RegisterWindowClass(hInst);

    DashboardState state;
    state.pm = &pm;
    state.ipc = &ipc;
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

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    pm.SetTerminalCallbacks(nullptr, nullptr);
    ipc.SetNotifyHwnd(nullptr);
    return static_cast<int>(msg.wParam);
}

} // namespace pc
