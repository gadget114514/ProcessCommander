## 2024-05-18 - Destructive Action Confirmation
**Learning:** Found that "Kill All" acts immediately without user confirmation, which could be surprising and potentially destructive. It's a key UX pattern to add confirmation dialogs for destructive actions.
**Action:** Always verify if destructive bulk actions have confirmations, and add them using MessageBoxW with MB_YESNO | MB_ICONQUESTION if they don't.
## 2024-10-23 - Focus Visible Styles for Owner-Drawn Buttons
**Learning:** In Win32, owner-drawn buttons (`BS_OWNERDRAW`) lose their default keyboard focus indicator (`ODS_FOCUS`), making keyboard navigation difficult and hindering accessibility.
**Action:** Always verify if owner-drawn controls handle the `ODS_FOCUS` state and use `DrawFocusRect` to render a visible focus ring.
## 2024-11-20 - Missing Tooltips on Icon-Only Owner-Drawn Toggles
**Learning:** Found that the "Usage" toggle button (📊) was an icon-only `BS_OWNERDRAW` button lacking both an ARIA-equivalent tooltip and a latched (pressed) visual state when active, making its state and purpose unclear.
**Action:** Always ensure icon-only buttons have tooltips via `TTM_ADDTOOLW`/`TTM_UPDATETIPTEXTW` and that toggle buttons reflect their active state visually in the `WM_DRAWITEM` handler.
