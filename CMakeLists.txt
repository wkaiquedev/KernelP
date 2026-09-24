cmake_minimum_required(VERSION 3.20)
project(KernelP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(WV2 ${CMAKE_SOURCE_DIR}/third_party/webview2_pkg/build/native)

add_executable(KernelP WIN32
    src/main.cpp
    src/engine.cpp
    src/app.rc
)

target_include_directories(KernelP PRIVATE ${WV2}/include)

target_link_libraries(KernelP PRIVATE
    ${WV2}/x64/WebView2LoaderStatic.lib
    version.lib
    shlwapi.lib
)

target_compile_definitions(KernelP PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
)

if (MSVC)
    target_compile_options(KernelP PRIVATE /O2 /EHsc /utf-8 /W3)
endif()
