cmake_minimum_required(VERSION 3.20)
project(Reflex VERSION 1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /W2 /GS- /GL- /O2 /MT")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /W2 /GS- /MTd")
set(CMAKE_MSVC_RUNTIME_LIBRARY "")

set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /DYNAMICBASE /NXCOMPAT")

add_library(Reflex SHARED
    src/dllmain.cpp
    src/minhook/buffer.c
    src/minhook/hook.c
    src/minhook/trampoline.c
    src/minhook/hde/hde32.c
    src/minhook/hde/hde64.c
)

target_include_directories(Reflex PRIVATE src)

target_link_libraries(Reflex PRIVATE
    d3d11.lib
    dxgi.lib
    d3dcompiler.lib
    winmm.lib
)
