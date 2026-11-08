import os
import sys

def verify_code():
    with open('src/gui_dashboard.cpp', 'r', encoding='utf-8') as f:
        content = f.read()

    errors = []

    if 'addTool(st->hwndBtnUsage);' not in content:
        errors.append("addTool for hwndBtnUsage is missing")

    if 'UpdateControlTooltip(st->hwndToolbarTip, st->hwndMain, st->hwndBtnUsage' not in content:
        errors.append("UpdateControlTooltip for hwndBtnUsage is missing")

    if 'st->showUsagePanel ? L"Hide Usage Details" : L"Show Usage Details"' not in content:
        errors.append("Strings for usage tooltip are missing or incorrect")

    if 'for (HWND b : {st->hwndBtnToggleList, st->hwndBtnToggleTerm, st->hwndBtnStreamPane, st->hwndBtnUsage})' not in content:
        errors.append("hwndBtnUsage is missing from invalidate rect loop")

    if errors:
        for err in errors:
            print("ERROR: " + err)
        sys.exit(1)

    print("Verification OK!")

if __name__ == "__main__":
    verify_code()
