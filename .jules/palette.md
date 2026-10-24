## 2024-05-18 - Destructive Action Confirmation
**Learning:** Found that "Kill All" acts immediately without user confirmation, which could be surprising and potentially destructive. It's a key UX pattern to add confirmation dialogs for destructive actions.
**Action:** Always verify if destructive bulk actions have confirmations, and add them using MessageBoxW with MB_YESNO | MB_ICONQUESTION if they don't.
## 2026-10-24 - Safe Default Focus for Destructive Actions
**Learning:** Found that confirmation dialogs for destructive actions (like Kill All, Kill Process Tree, Clear History) had 'Yes' as the default focused button, which could lead to accidental execution if the user presses Enter out of habit.
**Action:** Always add the MB_DEFBUTTON2 flag to MessageBoxW when using MB_YESNO for destructive actions, ensuring 'No' is the default focus and preventing accidental data loss or disruption.
