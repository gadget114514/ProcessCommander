## 2024-05-18 - Destructive Action Confirmation
**Learning:** Found that "Kill All" acts immediately without user confirmation, which could be surprising and potentially destructive. It's a key UX pattern to add confirmation dialogs for destructive actions.
**Action:** Always verify if destructive bulk actions have confirmations, and add them using MessageBoxW with MB_YESNO | MB_ICONQUESTION if they don't.
## 2024-10-23 - Focus Visible Styles for Owner-Drawn Buttons
**Learning:** In Win32, owner-drawn buttons (`BS_OWNERDRAW`) lose their default keyboard focus indicator (`ODS_FOCUS`), making keyboard navigation difficult and hindering accessibility.
**Action:** Always verify if owner-drawn controls handle the `ODS_FOCUS` state and use `DrawFocusRect` to render a visible focus ring.
## 2024-10-24 - Dynamic Tooltips and Latched State for Icon Toggle Buttons
**Learning:** Icon-only toggle buttons in native applications lack explicit state indicators. It is vital for a11y to dynamically update their tooltips to reflect their active/inactive state and provide visual latched cues.
**Action:** When adding or updating icon-only toggle buttons (like BS_OWNERDRAW), verify they have dynamic TOOLINFOW tooltips bound to their state variables, and visually reflect their toggled states.
