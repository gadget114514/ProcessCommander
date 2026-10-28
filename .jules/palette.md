## 2024-05-18 - Destructive Action Confirmation
**Learning:** Found that "Kill All" acts immediately without user confirmation, which could be surprising and potentially destructive. It's a key UX pattern to add confirmation dialogs for destructive actions.
**Action:** Always verify if destructive bulk actions have confirmations, and add them using MessageBoxW with MB_YESNO | MB_ICONQUESTION if they don't.
## 2024-10-23 - Focus Visible Styles for Owner-Drawn Buttons
**Learning:** In Win32, owner-drawn buttons (`BS_OWNERDRAW`) lose their default keyboard focus indicator (`ODS_FOCUS`), making keyboard navigation difficult and hindering accessibility.
**Action:** Always verify if owner-drawn controls handle the `ODS_FOCUS` state and use `DrawFocusRect` to render a visible focus ring.
## 2024-05-24 - Confirming Destructive Actions
**Learning:** For destructive actions (like killing processes or clearing logs), a confirmation dialog makes users feel much more comfortable. Using MB_DEFBUTTON2 correctly avoids accidental confirmations from rapid Enter presses.
**Action:** When adding or maintaining context menus for operations that delete or kill, ensure `MessageBoxW` uses `MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2`.
