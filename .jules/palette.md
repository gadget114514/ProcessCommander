## 2024-05-18 - Destructive Action Confirmation
**Learning:** Found that "Kill All" acts immediately without user confirmation, which could be surprising and potentially destructive. It's a key UX pattern to add confirmation dialogs for destructive actions.
**Action:** Always verify if destructive bulk actions have confirmations, and add them using MessageBoxW with MB_YESNO | MB_ICONQUESTION if they don't.
## 2024-10-23 - Focus Visible Styles for Owner-Drawn Buttons
**Learning:** In Win32, owner-drawn buttons (`BS_OWNERDRAW`) lose their default keyboard focus indicator (`ODS_FOCUS`), making keyboard navigation difficult and hindering accessibility.
**Action:** Always verify if owner-drawn controls handle the `ODS_FOCUS` state and use `DrawFocusRect` to render a visible focus ring.
## 2024-11-04 - Dynamic Tooltips and Latched States for Icon-Only Toggles
**Learning:** In native Win32 UI, icon-only buttons used as toggles require both dynamic tooltip associations (`UpdateControlTooltip` with localized `Tr()` strings, or plain string fallback) for accessibility and explicit management of their visually pressed (latched) state in `WM_DRAWITEM` (and `InvalidateRect` on change) to provide clear visual feedback.
**Action:** Always ensure icon-only toggle buttons have ARIA-equivalent tooltips that reflect their current state and proper pressed-state rendering logic.
