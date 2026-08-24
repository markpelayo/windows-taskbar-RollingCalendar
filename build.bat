@echo off
setlocal enabledelayedexpansion

rem build.bat -- one compiler, no package manager, no project file.
rem
rem Run this from an "x64 Native Tools Command Prompt for VS 2022". That prompt
rem is the only prerequisite: everything the app needs ships with the Windows
rem SDK, so there is nothing to restore and nothing to download.
rem
rem The build is statically linked (/MT) so the result is one .exe that runs on
rem a machine with no redistributable installed. That is the whole distribution
rem story: unzip, run.

where cl >nul 2>nul
if errorlevel 1 (
    echo.
    echo   cl.exe was not found.
    echo.
    echo   Open "x64 Native Tools Command Prompt for VS 2022" from the Start
    echo   menu and run this script from there. Visual Studio 2022 Community or
    echo   the standalone Build Tools both provide it.
    echo.
    exit /b 1
)

set OUT=build
if not exist %OUT% mkdir %OUT%

rem /permissive- and /Zc:__cplusplus so __cplusplus reports the truth; without
rem the second, MSVC still claims C++98 regardless of /std.
rem
rem NOMINMAX is not optional. windows.h defines min and max as macros, so
rem std::min(a, b) expands to std::(a) < (b) ? ... and the compiler reports it
rem as "illegal token on right side of ::" a hundred times over, in files that
rem look perfectly correct. CMakeLists.txt sets it too; the two must agree or
rem one build path fails while the other passes.
rem
rem WIN32_LEAN_AND_MEAN trims the rarely-used parts of windows.h. The modules
rem that need what it excludes -- mmsystem.h in alerts.cpp and westminster.cpp
rem -- include it explicitly, which is the better habit anyway.
set CXXFLAGS=/nologo /std:c++17 /permissive- /Zc:__cplusplus /EHsc /W4 /MT /O2 /GS /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DNDEBUG /utf-8

rem /W4 with two exclusions:
rem   4100 unreferenced formal parameter -- window procedures take four
rem        arguments whether or not a given message uses them all.
rem   4996 deprecation -- the SDK marks some perfectly serviceable calls.
set CXXFLAGS=%CXXFLAGS% /wd4100 /wd4996

set LIBS=kernel32.lib user32.lib gdi32.lib advapi32.lib shell32.lib comctl32.lib comdlg32.lib ole32.lib oleaut32.lib winhttp.lib winmm.lib taskschd.lib secur32.lib shlwapi.lib sapi.lib

set SOURCES=src\main.cpp src\app.cpp src\menu.cpp src\timeline.cpp src\taskbar.cpp src\daylist.cpp src\settings.cpp src\ics.cpp src\calsource.cpp src\demodata.cpp src\keywords.cpp src\fetch.cpp src\alerts.cpp src\soundhours.cpp src\westminster.cpp src\autostart.cpp src\dialogs.cpp src\common.cpp src\tzmap.cpp

echo Compiling resources...
rc /nologo /fo %OUT%\resources.res src\resources.rc
if errorlevel 1 goto :failed

echo Compiling...
cl %CXXFLAGS% /Fo%OUT%\ /Fe%OUT%\RollingCalendar.exe %SOURCES% %OUT%\resources.res /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:src\app.manifest %LIBS%
if errorlevel 1 goto :failed

echo.
echo   Built %OUT%\RollingCalendar.exe
echo.
echo   Run it: %OUT%\RollingCalendar.exe
echo   A strip appears in your taskbar showing a demo day. Right-click it to
echo   point it at your own calendar.
echo.
exit /b 0

:failed
echo.
echo   Build failed.
exit /b 1
