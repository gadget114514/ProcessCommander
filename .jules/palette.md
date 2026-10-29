## 2024-05-18 - Destructive Action Confirmation
**Learning:** Found that "Kill All" acts immediately without user confirmation, which could be surprising and potentially destructive. It's a key UX pattern to add confirmation dialogs for destructive actions.
**Action:** Always verify if destructive bulk actions have confirmations, and add them using MessageBoxW with MB_YESNO | MB_ICONQUESTION if they don't.
## 2024-10-23 - Focus Visible Styles for Owner-Drawn Buttons
**Learning:** In Win32, owner-drawn buttons (`BS_OWNERDRAW`) lose their default keyboard focus indicator (`ODS_FOCUS`), making keyboard navigation difficult and hindering accessibility.
**Action:** Always verify if owner-drawn controls handle the `ODS_FOCUS` state and use `DrawFocusRect` to render a visible focus ring.
## 2024-10-29 - Consistent Destructive Context Menu Actions
**Learning:** Some destructive actions in context menus were missing confirmation dialogs, leading to accidental process kills or queue purges which frustrated users. Consistency in confirmation dialogs (especially using default "No" focus with `MB_DEFBUTTON2`) is crucial for these kinds of UI elements.
**Action:** When adding destructive context menu items, always ensure they are wrapped in a `MessageBoxW` confirmation with `MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2` to match standard safety UX patterns.
