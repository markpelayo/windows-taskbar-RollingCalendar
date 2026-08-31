// diag.cpp — see diag.h.

#include "diag.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cwchar>

#include "common.h"

namespace rc {
namespace diag {
namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
std::wstring g_path;

// Appended synchronously and flushed by the write itself. Buffering would risk
// losing the last few lines, and the last few lines are exactly the ones that
// matter when something goes wrong.
void WriteRaw(const std::wstring& text) {
    if (g_file == INVALID_HANDLE_VALUE) return;

    const std::string utf8 = Narrow(text);
    DWORD written = 0;
    ::WriteFile(g_file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

std::wstring Stamp() {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    wchar_t buf[32];
    ::swprintf_s(buf, L"%02d:%02d:%02d.%03d  ", st.wHour, st.wMinute, st.wSecond,
                 st.wMilliseconds);
    return buf;
}

std::wstring ClassOf(HWND hwnd) {
    if (!hwnd) return L"(null)";
    wchar_t cls[160] = {0};
    ::GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    return cls;
}

std::wstring RectStr(const RECT& r) {
    wchar_t buf[96];
    ::swprintf_s(buf, L"(%ld,%ld)-(%ld,%ld) %ldx%ld", r.left, r.top, r.right, r.bottom,
                 r.right - r.left, r.bottom - r.top);
    return buf;
}

std::wstring TryOpenIn(const std::wstring& dir) {
    std::wstring path = dir;
    if (!path.empty() && path.back() != L'\\') path += L'\\';
    path += L"RollingCalendar-log.txt";

    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::wstring();

    g_file = h;
    return path;
}

}  // namespace

void Open(bool enabled) {
    if (!enabled) return;
    if (g_file != INVALID_HANDLE_VALUE) return;

    // Beside the executable first, because that is where someone looking for it
    // will look. Program Files is not writable, so fall back rather than
    // silently logging nothing.
    std::wstring exe = ExecutablePath();
    const size_t slash = exe.find_last_of(L'\\');
    if (slash != std::wstring::npos) {
        g_path = TryOpenIn(exe.substr(0, slash));
    }
    if (g_path.empty()) g_path = TryOpenIn(AppDataDir());
    if (g_path.empty()) return;

    // UTF-8 BOM, so Notepad does not mangle the arrows and dots.
    const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
    DWORD written = 0;
    ::WriteFile(g_file, bom, 3, &written, nullptr);

    OSVERSIONINFOEXW os{};
    os.dwOSVersionInfoSize = sizeof(os);
    // GetVersionEx lies to unmanifested processes, but this app manifests
    // Windows 10 and 11 support, so it reports the truth here. RtlGetVersion
    // would be the alternative and needs a runtime lookup for no extra value.
#pragma warning(push)
#pragma warning(disable : 4996)
    ::GetVersionExW(reinterpret_cast<OSVERSIONINFOW*>(&os));
#pragma warning(pop)

    Log(L"=== Rolling Calendar %s diagnostic log ===", kVersion);
    Log(L"exe        : %s", exe.c_str());
    Log(L"log        : %s", g_path.c_str());
    Log(L"windows    : %lu.%lu build %lu", os.dwMajorVersion, os.dwMinorVersion,
        os.dwBuildNumber);
    Log(L"dark mode  : %s", IsDarkMode() ? L"yes" : L"no");
    Log(L"screen     : %dx%d, %d monitors", ::GetSystemMetrics(SM_CXSCREEN),
        ::GetSystemMetrics(SM_CYSCREEN), ::GetSystemMetrics(SM_CMONITORS));
    Log(L"");
}

void Close() {
    if (g_file == INVALID_HANDLE_VALUE) return;
    Log(L"=== closed cleanly ===");
    ::CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
}

bool IsOpen() { return g_file != INVALID_HANDLE_VALUE; }
const std::wstring& Path() { return g_path; }

void Log(const wchar_t* fmt, ...) {
    if (g_file == INVALID_HANDLE_VALUE) return;

    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    const int n = ::_vsnwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, fmt, args);
    va_end(args);
    if (n < 0) return;

    WriteRaw(Stamp() + buf + L"\r\n");
}

void LogWindow(const wchar_t* label, HWND hwnd) {
    if (!hwnd || !::IsWindow(hwnd)) {
        Log(L"%-11s: (not a window) %p", label, hwnd);
        return;
    }

    RECT r{};
    ::GetWindowRect(hwnd, &r);
    const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR ex = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    Log(L"%-11s: hwnd=%p class=%s", label, hwnd, ClassOf(hwnd).c_str());
    Log(L"             rect=%s visible=%s", RectStr(r).c_str(),
        ::IsWindowVisible(hwnd) ? L"yes" : L"no");
    Log(L"             style=0x%08llX%s%s%s%s ex=0x%08llX%s%s%s",
        static_cast<unsigned long long>(style),
        (style & WS_CHILD) ? L" CHILD" : L"",
        (style & WS_POPUP) ? L" POPUP" : L"",
        (style & WS_VISIBLE) ? L" VISIBLE" : L"",
        (style & WS_CLIPSIBLINGS) ? L" CLIPSIBLINGS" : L"",
        static_cast<unsigned long long>(ex),
        (ex & WS_EX_TOPMOST) ? L" TOPMOST" : L"",
        (ex & WS_EX_LAYERED) ? L" LAYERED" : L"",
        (ex & WS_EX_TOOLWINDOW) ? L" TOOLWINDOW" : L"");
    Log(L"             parent=%p (%s)", ::GetParent(hwnd),
        ClassOf(::GetParent(hwnd)).c_str());
}

void LogZOrder(HWND tray, HWND self) {
    if (!tray || !::IsWindow(tray)) {
        Log(L"z-order    : no taskbar window to enumerate");
        return;
    }

    // GetWindow with GW_HWNDFIRST/GW_HWNDNEXT walks siblings front to back,
    // which is the order that decides who covers whom. EnumChildWindows does
    // not promise z-order, so it is the wrong tool for this specific question.
    Log(L"z-order    : Shell_TrayWnd children, front to back");

    HWND child = ::GetWindow(tray, GW_CHILD);
    if (child) child = ::GetWindow(child, GW_HWNDFIRST);

    int index = 0;
    int selfIndex = -1;
    for (; child && index < 60; child = ::GetWindow(child, GW_HWNDNEXT), ++index) {
        RECT r{};
        ::GetWindowRect(child, &r);

        DWORD pid = 0;
        ::GetWindowThreadProcessId(child, &pid);
        const bool ours = (pid == ::GetCurrentProcessId());
        if (child == self) selfIndex = index;

        Log(L"   [%2d]%s %-42s %s %s pid=%lu%s", index, (child == self) ? L" >>" : L"   ",
            ClassOf(child).c_str(), RectStr(r).c_str(),
            ::IsWindowVisible(child) ? L"vis" : L"HID", pid, ours ? L" (ours)" : L"");
    }

    if (selfIndex < 0) {
        Log(L"   the strip is NOT among the taskbar's children");
    } else {
        Log(L"   the strip is at index %d of %d (0 = frontmost)", selfIndex, index);
    }
}

void Snapshot(const wchar_t* reason, HWND self, HWND tray) {
    if (g_file == INVALID_HANDLE_VALUE) return;

    Log(L"---- snapshot: %s ----", reason);
    LogWindow(L"strip", self);
    LogWindow(L"taskbar", tray);

    // Who actually owns the pixel at the centre of the strip. If this reports
    // something other than the strip while the strip claims to be visible and
    // frontmost, then the thing covering it is named right here.
    if (self && ::IsWindow(self)) {
        RECT r{};
        ::GetWindowRect(self, &r);
        POINT mid{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
        const HWND at = ::WindowFromPoint(mid);
        Log(L"pixel owner: %p (%s) at (%ld,%ld)%s", at, ClassOf(at).c_str(), mid.x, mid.y,
            (at == self) ? L"  <- the strip" : L"  <- NOT the strip");

        // Same question asked of the parent, which answers it by sibling
        // z-order rather than by window ownership. ChildWindowFromPointEx wants
        // the parent's client coordinates, not screen ones.
        const HWND parent = ::GetParent(self);
        if (parent) {
            POINT local = mid;
            ::ScreenToClient(parent, &local);
            const HWND top = ::ChildWindowFromPointEx(parent, local, CWP_SKIPINVISIBLE);
            Log(L"child at pt: %p (%s)%s", top, ClassOf(top).c_str(),
                (top == self) ? L"  <- the strip" : L"");
        }
    }

    LogZOrder(tray, self);
    Log(L"");
}

}  // namespace diag
}  // namespace rc
