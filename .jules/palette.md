## 2024-05-24 - Default Focus for Destructive Win32 Dialogs
**Learning:** Destructive UI actions in Win32 applications (such as killing processes or clearing history) using `MessageBoxW` with `MB_YESNO` should default to 'No' to prevent accidental data loss or disruption from users pressing Enter too quickly.
**Action:** Always append the `MB_DEFBUTTON2` flag alongside `MB_YESNO` in `MessageBoxW` when asking for confirmation of a destructive or irreversible action.
