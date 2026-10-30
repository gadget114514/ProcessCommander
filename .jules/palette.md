## 2024-05-18 - Destructive Action Confirmation
**Learning:** Found that "Kill All" acts immediately without user confirmation, which could be surprising and potentially destructive. It's a key UX pattern to add confirmation dialogs for destructive actions.
**Action:** Always verify if destructive bulk actions have confirmations, and add them using MessageBoxW with MB_YESNO | MB_ICONQUESTION if they don't.
## 2024-10-23 - Focus Visible Styles for Owner-Drawn Buttons
**Learning:** In Win32, owner-drawn buttons (`BS_OWNERDRAW`) lose their default keyboard focus indicator (`ODS_FOCUS`), making keyboard navigation difficult and hindering accessibility.
**Action:** Always verify if owner-drawn controls handle the `ODS_FOCUS` state and use `DrawFocusRect` to render a visible focus ring.
## 2025-02-18 - Missing Tooltips on Icon-Only Dashboard Toggles
**Learning:** Win32 native applications relying on `BS_OWNERDRAW` for icon-only buttons (like the `📊` Usage panel button) are inherently inaccessible to screen readers without an explicit `TOOLINFOW` tooltip association.
**Action:** When adding new icon-only buttons in the ProcessCommander dashboard, always ensure they are registered with `TTM_ADDTOOLW` and have their text state synced in `UpdatePanelToggleLabels` to provide an ARIA-label equivalent.
