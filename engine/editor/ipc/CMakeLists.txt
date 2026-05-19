# SPDX-License-Identifier: MIT
# psynder_editor_ipc — Lane 22. Owns: local HTTP + WebSocket server, msgpack protocol, IDL stub gen.
psynder_add_lane(editor_ipc LINKS psynder_core psynder_scene psynder_editor_core)

# Platform sockets. Server.cpp uses POSIX sockets on Mac+Linux and Winsock2
# on Win32; `#pragma comment(lib, "Ws2_32.lib")` covers MSVC but clang-cl /
# MinGW need the explicit link.
if(WIN32)
    target_link_libraries(psynder_editor_ipc PRIVATE Ws2_32)
else()
    find_package(Threads QUIET)
    if(Threads_FOUND)
        target_link_libraries(psynder_editor_ipc PRIVATE Threads::Threads)
    endif()
endif()
