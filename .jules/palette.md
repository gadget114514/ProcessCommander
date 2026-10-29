## 2024-05-18 - Destructive Action Confirmation
**Learning:** Found that "Kill All" acts immediately without user confirmation, which could be surprising and potentially destructive. It's a key UX pattern to add confirmation dialogs for destructive actions.
**Action:** Always verify if destructive bulk actions have confirmations, and add them using MessageBoxW with MB_YESNO | MB_ICONQUESTION if they don't.
## 2024-10-23 - Focus Visible Styles for Owner-Drawn Buttons
**Learning:** In Win32, owner-drawn buttons (`BS_OWNERDRAW`) lose their default keyboard focus indicator (`ODS_FOCUS`), making keyboard navigation difficult and hindering accessibility.
**Action:** Always verify if owner-drawn controls handle the `ODS_FOCUS` state and use `DrawFocusRect` to render a visible focus ring.
## 2024-03-24 - Missing ARIA label equivalent on icon-only Win32 Buttons
**Learning:** In native Win32 apps, icon-only buttons (`BS_OWNERDRAW` showing just emoji/icons) fail accessibility without explicit tooltip associations (`TOOLINFOW`). We found `hwndBtnUsage` completely lacked this.
**Action:** Always verify every icon-only button is registered with `TTM_ADDTOOLW` and has localized text strings in `TTM_UPDATETIPTEXTW` for screen readers and mouse users.
