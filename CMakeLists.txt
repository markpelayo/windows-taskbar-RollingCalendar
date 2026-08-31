# CMakeLists.txt -- for people who would rather not use build.bat.
#
# It produces the same binary. build.bat exists because a single compiler
# invocation is easier to read than a build system, and CMake exists because
# most editors and CI systems already know how to drive it.
#
# No find_package, no FetchContent, no dependencies. That is deliberate: the
# only prerequisite is MSVC and the Windows SDK.

cmake_minimum_required(VERSION 3.20)
project(RollingCalendar VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Rolling Calendar embeds itself in the Windows taskbar. There is nothing here to build on another platform.")
endif()

if(NOT MSVC)
    message(WARNING "Only MSVC is tested. MinGW may work, but the manifest and resource handling differ.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT, so the shipped .exe needs no redistributable installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(RollingCalendar WIN32
    src/main.cpp
    src/app.cpp
    src/menu.cpp
    src/timeline.cpp
    src/taskbar.cpp
    src/daylist.cpp
    src/settings.cpp
    src/ics.cpp
    src/calsource.cpp
    src/demodata.cpp
    src/keywords.cpp
    src/fetch.cpp
    src/alerts.cpp
    src/soundhours.cpp
    src/westminster.cpp
    src/autostart.cpp
    src/dialogs.cpp
    src/common.cpp
    src/tzmap.cpp
    # Temporary: diagnostic log for the taskbar hosting behaviour.
    src/diag.cpp
    src/resources.rc
)

target_compile_definitions(RollingCalendar PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    target_compile_options(RollingCalendar PRIVATE
        /W4
        /permissive-
        /Zc:__cplusplus
        /utf-8
        # A window procedure takes four arguments whether or not a given
        # message uses them all.
        /wd4100
        /wd4996
    )
    # The v6 common controls manifest is what gives themed dialogs, cue
    # banners in edit boxes and TaskDialogIndirect. Without it the app still
    # runs, but every dialog reverts to Windows 95 chrome.
    set_target_properties(RollingCalendar PROPERTIES
        LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:\"${CMAKE_CURRENT_SOURCE_DIR}/src/app.manifest\""
    )
endif()

target_link_libraries(RollingCalendar PRIVATE
    kernel32 user32 gdi32 advapi32 shell32
    comctl32 comdlg32
    ole32 oleaut32
    winhttp winmm
    taskschd secur32 shlwapi
    sapi
)
