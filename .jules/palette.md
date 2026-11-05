## 2024-05-18 - Destructive Action Confirmation
**Learning:** Found that "Kill All" acts immediately without user confirmation, which could be surprising and potentially destructive. It's a key UX pattern to add confirmation dialogs for destructive actions.
**Action:** Always verify if destructive bulk actions have confirmations, and add them using MessageBoxW with MB_YESNO | MB_ICONQUESTION if they don't.
## 2024-10-23 - Focus Visible Styles for Owner-Drawn Buttons
**Learning:** In Win32, owner-drawn buttons (`BS_OWNERDRAW`) lose their default keyboard focus indicator (`ODS_FOCUS`), making keyboard navigation difficult and hindering accessibility.
**Action:** Always verify if owner-drawn controls handle the `ODS_FOCUS` state and use `DrawFocusRect` to render a visible focus ring.
## 2025-02-12 - Reusing Existing Translations for Accessibility Labels
**Learning:** When adding accessibility tooltips for newly identifiable but context-less buttons (like "Usage Graph"), existing UI text strings (such as from tray menus) can often be reused. In the codebase, it's vital to search for exact strings in existing `Tr()` calls rather than attempting to forge localized strings or falling back to English arbitrarily.
**Action:** Use `grep -rn "Tr("` and look for the specific term before assigning a localized or unlocalized string to ensure maximum localization fidelity.
