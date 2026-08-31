// main.cpp — the bootstrap, and deliberately nothing else.
//
// Four things have to happen before any window exists: refuse to start twice,
// bring COM up on this thread, declare a DPI awareness, and initialise the
// common controls. All four are process-wide and none of them can be undone
// later, which is the only reason they are here rather than in App.

#include <windows.h>

#include <commctrl.h>
#include <objbase.h>
#include <shellscalingapi.h>

#include "app.h"
#include "common.h"
#include "diag.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")

namespace {

// Declares per-monitor DPI awareness, preferring the newest API the running
// build actually has.
//
// All three are resolved with GetProcAddress rather than linked. The v2 context
// arrived in Windows 10 1703 and SetProcessDpiAwareness in 8.1; linking either
// one directly would mean the loader refused to start the process at all on an
// older build, with a dialog about a missing entry point rather than a
// slightly blurrier strip.
void DeclareDpiAwareness() {
    using SetContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    using SetAwarenessFn = HRESULT(WINAPI*)(PROCESS_DPI_AWARENESS);

    if (const HMODULE user32 = ::GetModuleHandleW(L"user32.dll")) {
        const auto setContext = reinterpret_cast<SetContextFn>(
            reinterpret_cast<void*>(::GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
        if (setContext && setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    }

    if (const HMODULE shcore = ::LoadLibraryW(L"shcore.dll")) {
        const auto setAwareness = reinterpret_cast<SetAwarenessFn>(
            reinterpret_cast<void*>(::GetProcAddress(shcore, "SetProcessDpiAwareness")));
        const bool ok = setAwareness && SUCCEEDED(setAwareness(PROCESS_PER_MONITOR_DPI_AWARE));
        ::FreeLibrary(shcore);
        if (ok) return;
    }

    ::SetProcessDPIAware();
}

}  // namespace

int APIENTRY wWinMain(_In_ HINSTANCE instance,
                      _In_opt_ HINSTANCE previous,
                      _In_ LPWSTR commandLine,
                      _In_ int showCommand) {
    UNREFERENCED_PARAMETER(previous);
    UNREFERENCED_PARAMETER(commandLine);
    UNREFERENCED_PARAMETER(showCommand);

    // A second copy would stack a second widget into the taskbar and ring every
    // chime twice, and the user has no way of telling which strip belongs to
    // which process. Exiting silently is right here: this happens when someone
    // launches an app they already have running, and a message box saying so is
    // an interruption rather than information. The handle is deliberately left
    // open for the life of the process.
    const HANDLE instanceLock = ::CreateMutexW(nullptr, TRUE, L"Local\\RollingCalendarSingleInstance");
    if (instanceLock && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::CloseHandle(instanceLock);
        return 0;
    }

    // Apartment-threaded, and held for the life of the process: the alert voice
    // (SAPI) and the startup entry (Task Scheduler) are both COM, and
    // alerts::Init expects COM to be up on the UI thread already.
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    DeclareDpiAwareness();

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_DATE_CLASSES | ICC_UPDOWN_CLASS;
    ::InitCommonControlsEx(&icc);

    // Temporary. Remove this, diag.h, diag.cpp and the diag:: calls in app.cpp
    // and taskbar.cpp once the taskbar hosting behaviour is settled.
    rc::diag::Open();

    int exitCode = 1;
    if (rc::App::Get().Initialize(instance)) exitCode = rc::App::Get().Run();

    rc::diag::Close();

    if (SUCCEEDED(com)) ::CoUninitialize();
    if (instanceLock) ::CloseHandle(instanceLock);
    return exitCode;
}
