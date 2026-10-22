cmake_minimum_required(VERSION 3.20)
project(ProcessCommander VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_definitions(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DECODE_ENABLE_SIXEL)

add_executable(ProcessCommander
    src/main.cpp
    src/common.cpp
    src/process_manager.cpp
    src/ipc_server.cpp
    src/ipc_client.cpp
    src/cli.cpp
    src/mcp_server.cpp
    src/gui_dashboard.cpp
    src/app.rc
    src/Terminal/src/ConPtySession.cpp
    src/Terminal/src/ImageManager.cpp
    src/Terminal/src/TerminalBuffer.cpp
    src/Terminal/src/TerminalEmulator.cpp
    src/Terminal/src/TerminalView.cpp
    src/Terminal/src/TerminalGridView.cpp
)

target_include_directories(ProcessCommander PRIVATE
    src
    include
    src/Terminal/include
    src/Terminal/src
)

if(MSVC)
    target_compile_options(ProcessCommander PRIVATE /W4 /utf-8 /EHsc)
endif()

target_link_libraries(ProcessCommander PRIVATE
    user32
    gdi32
    comctl32
    uxtheme
    shell32
    advapi32
    psapi
    ole32
    shlwapi
    imm32
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
)

add_dependencies(pcder ProcessCommander)

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
