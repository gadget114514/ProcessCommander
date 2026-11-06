## 2024-05-18 - Destructive Action Confirmation
**Learning:** Found that "Kill All" acts immediately without user confirmation, which could be surprising and potentially destructive. It's a key UX pattern to add confirmation dialogs for destructive actions.
**Action:** Always verify if destructive bulk actions have confirmations, and add them using MessageBoxW with MB_YESNO | MB_ICONQUESTION if they don't.
## 2024-10-23 - Focus Visible Styles for Owner-Drawn Buttons
**Learning:** In Win32, owner-drawn buttons (`BS_OWNERDRAW`) lose their default keyboard focus indicator (`ODS_FOCUS`), making keyboard navigation difficult and hindering accessibility.
**Action:** Always verify if owner-drawn controls handle the `ODS_FOCUS` state and use `DrawFocusRect` to render a visible focus ring.
## 2026-11-06 - Tooltips for Owner-Drawn Icon Buttons
**Learning:** Icon-only owner-drawn buttons (`BS_OWNERDRAW`) in Win32 lack inherent textual context, making them completely inaccessible to screen readers and confusing to users if the icon is ambiguous.
**Action:** Always verify that icon-only buttons are associated with a tooltip using `TOOLINFOW` (`TTM_ADDTOOLW` and `TTM_UPDATETIPTEXTW`) to provide an ARIA-equivalent label, and ensure toggle buttons update this text based on their state.
