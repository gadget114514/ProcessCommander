## 2024-05-18 - Destructive Action Confirmation
**Learning:** Found that "Kill All" acts immediately without user confirmation, which could be surprising and potentially destructive. It's a key UX pattern to add confirmation dialogs for destructive actions.
**Action:** Always verify if destructive bulk actions have confirmations, and add them using MessageBoxW with MB_YESNO | MB_ICONQUESTION if they don't.

## 2026-10-26 - Destructive action dialog default focus
**Learning:** Destructive UI actions (like killing processes or clearing history) need explicit protection against accidental triggers, which are common when primary actions share muscle memory.
**Action:** Always guard destructive actions with `MessageBoxW` and the `MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2` flags to ensure 'No' is the default focus.
