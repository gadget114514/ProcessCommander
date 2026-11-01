## 2024-05-18 - Destructive Action Confirmation
**Learning:** Found that "Kill All" acts immediately without user confirmation, which could be surprising and potentially destructive. It's a key UX pattern to add confirmation dialogs for destructive actions.
**Action:** Always verify if destructive bulk actions have confirmations, and add them using MessageBoxW with MB_YESNO | MB_ICONQUESTION if they don't.
## 2024-10-23 - Focus Visible Styles for Owner-Drawn Buttons
**Learning:** In Win32, owner-drawn buttons (`BS_OWNERDRAW`) lose their default keyboard focus indicator (`ODS_FOCUS`), making keyboard navigation difficult and hindering accessibility.
**Action:** Always verify if owner-drawn controls handle the `ODS_FOCUS` state and use `DrawFocusRect` to render a visible focus ring.
## 2024-05-24 - Tooltips as ARIA labels in Win32
**Learning:** In native Win32 applications, icon-only buttons (like those with BS_OWNERDRAW) lack screen reader context, making tooltips essential as an ARIA-label equivalent for accessibility. Also, toggle buttons must explicitly manage and invalidate their visual latch state for proper feedback.
**Action:** Always associate TOOLINFOW tooltips with icon-only BS_OWNERDRAW buttons and explicitly manage visual latched states via WM_DRAWITEM and InvalidateRect.
