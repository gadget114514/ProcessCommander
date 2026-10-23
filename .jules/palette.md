## 2024-05-18 - Destructive Action Confirmation
**Learning:** Found that "Kill All" acts immediately without user confirmation, which could be surprising and potentially destructive. It's a key UX pattern to add confirmation dialogs for destructive actions.
**Action:** Always verify if destructive bulk actions have confirmations, and add them using MessageBoxW with MB_YESNO | MB_ICONQUESTION if they don't.
## 2023-10-23 - Interactive Element Cursors in Win32
**Learning:** Native Win32 components don't automatically change the mouse cursor to a pointer when hovering over custom interactive elements like drawn banners or bounds. This lack of visual feedback makes it less intuitive that the elements can be clicked.
**Action:** Always intercept `WM_SETCURSOR` and check coordinates against interactive element bounds (`PtInRect`), then manually `SetCursor(LoadCursorW(nullptr, IDC_HAND))` to provide clear interaction cues. Use `#ifndef IDC_HAND` to define it if using older Windows SDKs.
