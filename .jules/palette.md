## 2024-05-18 - Destructive Action Confirmation
**Learning:** Found that "Kill All" acts immediately without user confirmation, which could be surprising and potentially destructive. It's a key UX pattern to add confirmation dialogs for destructive actions.
**Action:** Always verify if destructive bulk actions have confirmations, and add them using MessageBoxW with MB_YESNO | MB_ICONQUESTION if they don't.
## 2024-10-23 - Focus Visible Styles for Owner-Drawn Buttons
**Learning:** In Win32, owner-drawn buttons (`BS_OWNERDRAW`) lose their default keyboard focus indicator (`ODS_FOCUS`), making keyboard navigation difficult and hindering accessibility.
**Action:** Always verify if owner-drawn controls handle the `ODS_FOCUS` state and use `DrawFocusRect` to render a visible focus ring.
## 2024-10-28 - Header Flicker with Owner-Drawn Headers
**Learning:** Owner-drawn Win32 ListView headers (`HDF_OWNERDRAW`) often suffer from severe rendering flicker when resizing or hovering because Windows defaults to erasing the background completely before allowing the application to paint over it.
**Action:** When implementing owner-drawn headers, always enable double buffering by applying `HDS_DOUBLEBUFFER` to the header's style (via `SetWindowLongPtrW`), and suppress the default background fill by intercepting `WM_ERASEBKGND` to simply return `1` (true).
