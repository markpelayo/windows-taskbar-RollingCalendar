// raii.h — handle wrappers, about eighty lines of what WIL would give you.
//
// Kept in-tree deliberately: cloning and building this project needs a compiler
// and nothing else. No vcpkg, no NuGet, no submodules.
//
// This matters more than tidiness. A process is capped at 10,000 GDI handles,
// so leaking one brush per redraw kills the app in under three hours at one
// repaint per second. Every handle in the app is owned by something here.

#pragma once

#include <windows.h>

#include <utility>

namespace rc {

template <typename T, typename Deleter>
class Unique {
public:
    Unique() = default;
    explicit Unique(T h) : h_(h) {}
    ~Unique() { reset(); }

    Unique(const Unique&) = delete;
    Unique& operator=(const Unique&) = delete;

    Unique(Unique&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    Unique& operator=(Unique&& o) noexcept {
        if (this != &o) {
            reset();
            h_ = o.h_;
            o.h_ = nullptr;
        }
        return *this;
    }

    T get() const { return h_; }
    operator T() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }

    T release() {
        T t = h_;
        h_ = nullptr;
        return t;
    }

    void reset(T h = nullptr) {
        if (h_ && h_ != h) Deleter{}(h_);
        h_ = h;
    }

private:
    T h_ = nullptr;
};

struct GdiDeleter {
    void operator()(HGDIOBJ h) const { ::DeleteObject(h); }
};
struct DcDeleter {
    void operator()(HDC h) const { ::DeleteDC(h); }
};
struct MenuDeleter {
    void operator()(HMENU h) const { ::DestroyMenu(h); }
};
struct HandleDeleter {
    void operator()(HANDLE h) const {
        if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
    }
};

using UniqueBrush = Unique<HBRUSH, GdiDeleter>;
using UniquePen = Unique<HPEN, GdiDeleter>;
using UniqueFont = Unique<HFONT, GdiDeleter>;
using UniqueBitmap = Unique<HBITMAP, GdiDeleter>;
using UniqueRgn = Unique<HRGN, GdiDeleter>;
using UniqueMemDC = Unique<HDC, DcDeleter>;
using UniqueMenu = Unique<HMENU, MenuDeleter>;
using UniqueHandle = Unique<HANDLE, HandleDeleter>;

// Restores whatever was selected into the DC when it goes out of scope.
class SelectGuard {
public:
    SelectGuard(HDC dc, HGDIOBJ obj) : dc_(dc), old_(::SelectObject(dc, obj)) {}
    ~SelectGuard() {
        if (dc_ && old_) ::SelectObject(dc_, old_);
    }
    SelectGuard(const SelectGuard&) = delete;
    SelectGuard& operator=(const SelectGuard&) = delete;

private:
    HDC dc_;
    HGDIOBJ old_;
};

// Saves and restores the whole DC state (clip region, transform, colours).
class DcStateGuard {
public:
    explicit DcStateGuard(HDC dc) : dc_(dc), state_(::SaveDC(dc)) {}
    ~DcStateGuard() {
        if (dc_ && state_) ::RestoreDC(dc_, state_);
    }
    DcStateGuard(const DcStateGuard&) = delete;
    DcStateGuard& operator=(const DcStateGuard&) = delete;

private:
    HDC dc_;
    int state_;
};

// A CRITICAL_SECTION that owns its own lifetime, plus a scoped lock.
class Lock {
public:
    Lock() { ::InitializeCriticalSection(&cs_); }
    ~Lock() { ::DeleteCriticalSection(&cs_); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

    void enter() { ::EnterCriticalSection(&cs_); }
    void leave() { ::LeaveCriticalSection(&cs_); }

private:
    CRITICAL_SECTION cs_;
};

class Guard {
public:
    explicit Guard(Lock& l) : l_(l) { l_.enter(); }
    ~Guard() { l_.leave(); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

private:
    Lock& l_;
};

// Balanced CoInitializeEx, for the threads that talk to Task Scheduler or SAPI.
class ComScope {
public:
    ComScope() { ok_ = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)); }
    ~ComScope() {
        if (ok_) ::CoUninitialize();
    }
    bool ok() const { return ok_; }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    bool ok_ = false;
};

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }

    T** put() {
        reset();
        return &p_;
    }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    void reset() {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }

private:
    T* p_ = nullptr;
};

}  // namespace rc
