## 2024-05-18 - Destructive Action Confirmation
**Learning:** Found that "Kill All" acts immediately without user confirmation, which could be surprising and potentially destructive. It's a key UX pattern to add confirmation dialogs for destructive actions.
**Action:** Always verify if destructive bulk actions have confirmations, and add them using MessageBoxW with MB_YESNO | MB_ICONQUESTION if they don't.
## 2024-10-23 - Focus Visible Styles for Owner-Drawn Buttons
**Learning:** In Win32, owner-drawn buttons (`BS_OWNERDRAW`) lose their default keyboard focus indicator (`ODS_FOCUS`), making keyboard navigation difficult and hindering accessibility.
**Action:** Always verify if owner-drawn controls handle the `ODS_FOCUS` state and use `DrawFocusRect` to render a visible focus ring.
## $(date +%Y-%m-%d) - Added missing tooltip to Usage Statistics button
**Learning:** Icon-only buttons (like `BS_OWNERDRAW` containing only icons/emoji) often miss tooltips, causing accessibility issues because screen readers lack an ARIA-label equivalent text.
**Action:** Always ensure that icon-only buttons are properly registered with `TTM_ADDTOOLW` and their text is updated using `TTM_UPDATETIPTEXTW` so that they remain accessible.
