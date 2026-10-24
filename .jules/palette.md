## 2025-01-31 - Safe defaults for destructive actions
**Learning:** When presenting users with a Win32 MessageBoxW for destructive actions, `MB_YESNO` defaults to "Yes". This is a dangerous UX pattern for actions like killing processes or wiping history.
**Action:** Always add the `MB_DEFBUTTON2` flag when combining with `MB_YESNO` in destructive action confirmations to ensure "No" is the default focus.
