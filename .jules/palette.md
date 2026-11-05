## 2024-05-18 - Destructive Action Confirmation
**Learning:** Found that "Kill All" acts immediately without user confirmation, which could be surprising and potentially destructive. It's a key UX pattern to add confirmation dialogs for destructive actions.
**Action:** Always verify if destructive bulk actions have confirmations, and add them using MessageBoxW with MB_YESNO | MB_ICONQUESTION if they don't.
## 2024-10-23 - Focus Visible Styles for Owner-Drawn Buttons
**Learning:** In Win32, owner-drawn buttons (`BS_OWNERDRAW`) lose their default keyboard focus indicator (`ODS_FOCUS`), making keyboard navigation difficult and hindering accessibility.
**Action:** Always verify if owner-drawn controls handle the `ODS_FOCUS` state and use `DrawFocusRect` to render a visible focus ring.
## 2024-11-05 - Icon-only owner-drawn toggles
**Learning:** Found that owner-drawn toggle buttons without text labels must have tooltip text added during control initialization, updated dynamically upon state changes, and managed visually with latched states in WM_DRAWITEM for accessibility and UX consistency.
**Action:** Always add TOOLINFOW tooltip registrations, update text on toggle via UpdateControlTooltip, and set latched background states in WM_DRAWITEM for all icon-only toggle buttons.
