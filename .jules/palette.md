## 2024-05-24 - Remove WM_SETREDRAW for double-buffered ListView
**Learning:** Using `WM_SETREDRAW` on a ListView that already has `LVS_EX_DOUBLEBUFFER` enabled causes its child Header control to unnecessarily invalidate and flicker, especially when updated frequently on a timer.
**Action:** Do not use `WM_SETREDRAW` around ListView updates when `LVS_EX_DOUBLEBUFFER` is already active to ensure flicker-free rendering of both the list and its header.
