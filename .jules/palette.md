## 2024-05-18 - Destructive Action Confirmation
**Learning:** Found that "Kill All" acts immediately without user confirmation, which could be surprising and potentially destructive. It's a key UX pattern to add confirmation dialogs for destructive actions.
**Action:** Always verify if destructive bulk actions have confirmations, and add them using MessageBoxW with MB_YESNO | MB_ICONQUESTION if they don't.

## 2026-10-27 - Safe Defaults for Destructive Actions
**Learning:** Found that destructive confirmation dialogs (like killing processes or clearing history) defaulted to "Yes", which can lead to accidental data loss or disruption if the user accidentally presses Enter quickly.
**Action:** Always append MB_DEFBUTTON2 to MessageBoxW flags for MB_YESNO dialogs that perform destructive actions, ensuring "No" is the default focus.
