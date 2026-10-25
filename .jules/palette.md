## 2024-05-18 - Destructive Action Confirmation
**Learning:** Found that "Kill All" acts immediately without user confirmation, which could be surprising and potentially destructive. It's a key UX pattern to add confirmation dialogs for destructive actions.
**Action:** Always verify if destructive bulk actions have confirmations, and add them using MessageBoxW with MB_YESNO | MB_ICONQUESTION if they don't.

## 2024-10-25 - Safe Defaults for Destructive Actions
**Learning:** Destructive actions with confirmation dialogs (like killing processes or clearing history) should default to "No" (using `MB_DEFBUTTON2` in `MessageBoxW`). Otherwise, users who accidentally press Enter will confirm the action, leading to unintended data loss or process termination.
**Action:** Always append `MB_DEFBUTTON2` to `MessageBoxW` calls when presenting a destructive `MB_YESNO` dialog to ensure the safest option is focused by default.
