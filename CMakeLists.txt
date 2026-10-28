cmake_minimum_required(VERSION 3.20)
project(LiteWallpaperMF VERSION 0.1.0 LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "LiteWallpaper-MF builds on Windows 10/11 only")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "LiteWallpaper-MF requires the Visual Studio C++ toolchain")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(LiteWallpaperMF WIN32
    src/main.cpp
    src/core/config.cpp
    src/core/playback_clock.cpp
    src/decoder/mf_decoder.cpp
    src/platform/win32/desktop_injector.cpp
    src/platform/win32/d3d11_presenter.cpp
    src/platform/win32/power_governor.cpp
    src/platform/win32/tray_icon.cpp
    src/app.rc
)

target_include_directories(LiteWallpaperMF PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(LiteWallpaperMF PRIVATE
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_options(LiteWallpaperMF PRIVATE
    /O1 /Oi /Gy /GL /GR- /EHsc /W4 /utf-8
)
target_link_options(LiteWallpaperMF PRIVATE
    /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO
)

target_link_libraries(LiteWallpaperMF PRIVATE
    d3d11
    dxgi
    mfplat
    mfreadwrite
    mfuuid
    dwmapi
    user32
    shell32
    ole32
    uuid
    wtsapi32
    advapi32
)

set_target_properties(LiteWallpaperMF PROPERTIES
    OUTPUT_NAME "LiteWallpaper-MF"
)
