## 2024-05-18 - Destructive Action Confirmation
**Learning:** Found that "Kill All" acts immediately without user confirmation, which could be surprising and potentially destructive. It's a key UX pattern to add confirmation dialogs for destructive actions.
**Action:** Always verify if destructive bulk actions have confirmations, and add them using MessageBoxW with MB_YESNO | MB_ICONQUESTION if they don't.
## 2024-10-23 - Fix ListView Header Flicker
**Learning:** Owner-drawn Windows ListView header controls can flicker when painted. The background erase message (`WM_ERASEBKGND`) interacts poorly with double buffering. If a custom `WM_ERASEBKGND` paints the background, and then an owner-draw paints it again, it causes flicker. Also `HDS_DOUBLEBUFFER` uses an off-screen DC for both `WM_ERASEBKGND` and `WM_DRAWITEM`.
**Action:** Always enable `HDS_DOUBLEBUFFER` via `Get/SetWindowLongW` on the header HWND. Do NOT return an empty erase to `WM_ERASEBKGND` if you want to support trailing empty header areas in custom themes. Keep painting the background in `WM_ERASEBKGND`, because double buffering makes it safe and flicker-free.
