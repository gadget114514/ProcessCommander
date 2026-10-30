cmake_minimum_required(VERSION 3.20)
project(ProcessCommander VERSION 2.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Link the MSVC runtime statically so the executables run without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_definitions(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DECODE_ENABLE_SIXEL)

add_executable(ProcessCommander
    src/main.cpp
    src/common.cpp
    src/process_manager.cpp
    src/taskqueue.cpp
    src/chain_guard.cpp
    src/ipc_server.cpp
    src/ipc_client.cpp
    src/cli.cpp
    src/mcp_server.cpp
    src/llm_stream.cpp
    src/gui_dashboard.cpp
    src/app.rc
    src/Terminal/src/ConPtySession.cpp
    src/Terminal/src/ImageManager.cpp
    src/Terminal/src/TerminalBuffer.cpp
    src/Terminal/src/TerminalEmulator.cpp
    src/Terminal/src/TerminalView.cpp
    src/Terminal/src/TerminalGridView.cpp
)

# Version macros for C++ (PC_VERSION) and the VERSIONINFO resource (src/version_info.rc2).
set(PC_VERSION_DEFINITIONS
    PC_VERSION="${PROJECT_VERSION}"
    PC_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    PC_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    PC_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

target_compile_definitions(ProcessCommander PRIVATE ${PC_VERSION_DEFINITIONS})

target_include_directories(ProcessCommander PRIVATE
    src
    include
    src/Terminal/include
    src/Terminal/src
)

if(MSVC)
    target_compile_options(ProcessCommander PRIVATE /W4 /utf-8 /EHsc)
    target_link_options(ProcessCommander PRIVATE
        "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")
endif()

target_link_libraries(ProcessCommander PRIVATE
    user32
    gdi32
    comctl32
    comdlg32
    uxtheme
    shell32
    advapi32
    psapi
    ole32
    shlwapi
    imm32
    winmm
    d2d1
    dwrite
    dwmapi
    windowscodecs
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/libsixel.lib"
)

add_custom_command(TARGET ProcessCommander POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/conpty.dll"
        "$<TARGET_FILE_DIR:ProcessCommander>/conpty.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/OpenConsole.exe"
        "$<TARGET_FILE_DIR:ProcessCommander>/OpenConsole.exe"
    COMMENT "Copying lib/conpty.dll and lib/OpenConsole.exe to output directory"
)

# Console client (human-friendly by default, --json for machine-readable output)
add_executable(pcder
    src/cli_main.cpp
    src/cli.cpp
    src/ipc_client.cpp
    src/common.cpp
    src/pcder.rc
)

add_dependencies(pcder ProcessCommander)

target_compile_definitions(pcder PRIVATE ${PC_VERSION_DEFINITIONS})

target_include_directories(pcder PRIVATE
    src
    include
)

if(MSVC)
    target_compile_options(pcder PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(pcder PRIVATE
    user32
    shell32
    advapi32
)

# Headless smoke test for the task queue engine (deps, retries, chain guard, persistence).
add_executable(taskqueue_smoke
    tests/taskqueue_smoke.cpp
    src/taskqueue.cpp
    src/chain_guard.cpp
    src/process_manager.cpp
    src/llm_stream.cpp
    src/common.cpp
    src/Terminal/src/ConPtySession.cpp
    src/Terminal/src/ImageManager.cpp
    src/Terminal/src/TerminalBuffer.cpp
    src/Terminal/src/TerminalEmulator.cpp
    src/Terminal/src/TerminalView.cpp
)

target_include_directories(taskqueue_smoke PRIVATE
    src
    include
    src/Terminal/include
    src/Terminal/src
)

if(MSVC)
    target_compile_options(taskqueue_smoke PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(taskqueue_smoke PRIVATE
    user32
    gdi32
    comctl32
    shell32
    advapi32
    psapi
    ole32
    shlwapi
    imm32
    winmm
    d2d1
    dwrite
    dwmapi
    windowscodecs
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/libsixel.lib"
)

add_custom_command(TARGET taskqueue_smoke POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/conpty.dll"
        "$<TARGET_FILE_DIR:taskqueue_smoke>/conpty.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/lib/OpenConsole.exe"
        "$<TARGET_FILE_DIR:taskqueue_smoke>/OpenConsole.exe"
    COMMENT "Copying lib/conpty.dll and lib/OpenConsole.exe for taskqueue_smoke"
)
