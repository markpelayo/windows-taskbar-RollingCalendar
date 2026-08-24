// dialogs.cpp — see dialogs.h for what and why.
//
// Every dialog here is assembled at run time into a DLGTEMPLATE in a heap
// buffer and shown with DialogBoxIndirectParamW. The alternative -- a .rc file
// with a DIALOGEX block, a resource.h with the control IDs, and a switch
// statement in the dialog procedure -- means that adding one field is an edit
// in three files that must be kept in agreement by hand, with nothing but a
// link error at the far end if they are not. Here a field is one more row in
// an array of ItemDesc, next to the code that reads it.
//
// The cost is the template layout rules, which are fiddly and undocumented in
// any one place: the header is followed by menu, class and title strings, then
// the font when DS_SETFONT is set, and each DLGITEMTEMPLATE that follows must
// begin on a DWORD boundary. BuildDialog is the only place in the app that has
// to know that.
//
// Coordinates below are dialog units, not pixels, which is what makes the
// layout survive a change of system font or DPI without arithmetic.

#include "dialogs.h"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>

#include <cmath>
#include <cstdlib>
#include <cwchar>

#include "raii.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")

namespace rc {
namespace dialogs {

namespace {

// ------------------------------------------------------------ foreground
//
// The app has no taskbar button and never owns the foreground window, so a
// modal dialog raised from a tray or strip click opens *behind* the taskbar
// and behind whatever the user was actually looking at. The user sees a click
// that did nothing, waits, clicks again, and now there are two dialogs. The
// foreground-lock rules will only hand the foreground to a process that the
// user has just interacted with, which is exactly the case here, but the
// permission has to be claimed explicitly. Every entry point in this file
// calls this first; that is the one and only reason.
void PrepareForeground(HWND owner) {
    ::AllowSetForegroundWindow(ASFW_ANY);
    if (owner && ::IsWindow(owner)) ::SetForegroundWindow(owner);
}

// ------------------------------------------------------- template builder

constexpr const wchar_t* kClassStatic = L"STATIC";
constexpr const wchar_t* kClassEdit = L"EDIT";
constexpr const wchar_t* kClassButton = L"BUTTON";
constexpr const wchar_t* kClassDateTime = L"SysDateTimePick32";

constexpr WORD kFontPoints = 9;
constexpr const wchar_t* kFontFace = L"Segoe UI";

enum ControlId : WORD {
    kIdPrompt = 100,
    kIdEdit1 = 101,
    kIdLabel2 = 102,
    kIdEdit2 = 103,
    kIdDate = 104,
    kIdTime = 105,
    kIdUseCurrent = 106,
};

struct ItemDesc {
    const wchar_t* cls;
    const wchar_t* text;
    DWORD style;
    DWORD exStyle;
    short x, y, cx, cy;
    WORD id;
};

class TemplateBuffer {
public:
    void PutWord(WORD v) {
        const BYTE* p = reinterpret_cast<const BYTE*>(&v);
        bytes_.insert(bytes_.end(), p, p + sizeof(v));
    }
    void PutDword(DWORD v) {
        const BYTE* p = reinterpret_cast<const BYTE*>(&v);
        bytes_.insert(bytes_.end(), p, p + sizeof(v));
    }
    void PutShort(short v) { PutWord(static_cast<WORD>(v)); }

    // Template strings are NUL-terminated WCHAR runs, never counted.
    void PutString(const wchar_t* s) {
        if (!s) s = L"";
        for (; *s; ++s) PutWord(static_cast<WORD>(*s));
        PutWord(0);
    }

    void AlignDword() {
        while (bytes_.size() % 4 != 0) bytes_.push_back(0);
    }

    LPCDLGTEMPLATEW Template() const {
        return reinterpret_cast<LPCDLGTEMPLATEW>(bytes_.data());
    }

private:
    std::vector<BYTE> bytes_;
};

// The template lives in the returned buffer; hand DialogBoxIndirectParamW a
// pointer into it and nothing else. std::vector's allocation is at least
// pointer-aligned, which satisfies the DWORD alignment the header requires.
void BuildDialog(TemplateBuffer* out,
                 const std::wstring& title,
                 short cx, short cy,
                 const ItemDesc* items, size_t count) {
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME |
                        DS_SETFONT | DS_CENTER;

    out->PutDword(style);
    out->PutDword(0);                                // dwExtendedStyle
    out->PutWord(static_cast<WORD>(count));          // cdit
    out->PutShort(0);                                // x, ignored under DS_CENTER
    out->PutShort(0);                                // y
    out->PutShort(cx);
    out->PutShort(cy);
    out->PutWord(0);                                 // no menu
    out->PutWord(0);                                 // default dialog class
    out->PutString(title.c_str());
    out->PutWord(kFontPoints);                       // required by DS_SETFONT
    out->PutString(kFontFace);

    for (size_t i = 0; i < count; ++i) {
        const ItemDesc& d = items[i];
        out->AlignDword();
        out->PutDword(d.style | WS_CHILD | WS_VISIBLE);
        out->PutDword(d.exStyle);
        out->PutShort(d.x);
        out->PutShort(d.y);
        out->PutShort(d.cx);
        out->PutShort(d.cy);
        out->PutWord(d.id);
        out->PutString(d.cls);
        out->PutString(d.text);
        out->PutWord(0);                             // no creation data
    }
}

// --------------------------------------------------------------- helpers

std::wstring ControlText(HWND dlg, int id) {
    HWND h = ::GetDlgItem(dlg, id);
    if (!h) return std::wstring();
    const int len = ::GetWindowTextLengthW(h);
    if (len <= 0) return std::wstring();
    std::wstring s(static_cast<size_t>(len) + 1, L'\0');
    const int got = ::GetWindowTextW(h, &s[0], len + 1);
    s.resize(got > 0 ? static_cast<size_t>(got) : 0);
    return s;
}

void SetCue(HWND dlg, int id, const std::wstring& placeholder) {
    if (placeholder.empty()) return;
    HWND h = ::GetDlgItem(dlg, id);
    if (!h) return;
    // TRUE keeps the hint visible once the field has focus, which matters when
    // the hint is an example of the format rather than a label.
    ::SendMessageW(h, EM_SETCUEBANNER, TRUE,
                   reinterpret_cast<LPARAM>(placeholder.c_str()));
}

// Date and time pickers live in comctl32's date classes, which are registered
// on first use of InitCommonControlsEx and not before. The v6 manifest gets
// the right DLL loaded but does not register any window class, and no other
// module in this app has a reason to ask for ICC_DATE_CLASSES, so the call
// cannot be assumed to have happened. Doing it here, once, keeps the
// dependency next to the only code that has it.
void EnsureDateClasses() {
    static bool done = false;
    if (done) return;
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_DATE_CLASSES;
    ::InitCommonControlsEx(&icc);
    done = true;
}

HINSTANCE SelfInstance() { return ::GetModuleHandleW(nullptr); }

// ------------------------------------------------------------- TextInput

struct TextCtx {
    const std::wstring* placeholder;
    std::wstring* value;
};

INT_PTR CALLBACK TextProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG: {
            ::SetWindowLongPtrW(dlg, DWLP_USER, static_cast<LONG_PTR>(lp));
            TextCtx* ctx = reinterpret_cast<TextCtx*>(lp);
            ::SetDlgItemTextW(dlg, kIdEdit1, ctx->value->c_str());
            SetCue(dlg, kIdEdit1, *ctx->placeholder);
            HWND edit = ::GetDlgItem(dlg, kIdEdit1);
            ::SendMessageW(edit, EM_SETSEL, 0, -1);
            ::SetFocus(edit);
            return FALSE;  // focus was set by hand
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDOK: {
                    TextCtx* ctx =
                        reinterpret_cast<TextCtx*>(::GetWindowLongPtrW(dlg, DWLP_USER));
                    *ctx->value = ControlText(dlg, kIdEdit1);
                    ::EndDialog(dlg, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    ::EndDialog(dlg, IDCANCEL);
                    return TRUE;
                default:
                    break;
            }
            break;
        default:
            break;
    }
    return FALSE;
}

// ------------------------------------------------------- TwoFieldInput

struct TwoFieldCtx {
    const std::wstring* placeholder1;
    const std::wstring* placeholder2;
    std::wstring* value1;
    std::wstring* value2;
};

INT_PTR CALLBACK TwoFieldProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG: {
            ::SetWindowLongPtrW(dlg, DWLP_USER, static_cast<LONG_PTR>(lp));
            TwoFieldCtx* ctx = reinterpret_cast<TwoFieldCtx*>(lp);
            ::SetDlgItemTextW(dlg, kIdEdit1, ctx->value1->c_str());
            ::SetDlgItemTextW(dlg, kIdEdit2, ctx->value2->c_str());
            SetCue(dlg, kIdEdit1, *ctx->placeholder1);
            SetCue(dlg, kIdEdit2, *ctx->placeholder2);
            HWND edit = ::GetDlgItem(dlg, kIdEdit1);
            ::SendMessageW(edit, EM_SETSEL, 0, -1);
            ::SetFocus(edit);
            return FALSE;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDOK: {
                    TwoFieldCtx* ctx =
                        reinterpret_cast<TwoFieldCtx*>(::GetWindowLongPtrW(dlg, DWLP_USER));
                    *ctx->value1 = ControlText(dlg, kIdEdit1);
                    *ctx->value2 = ControlText(dlg, kIdEdit2);
                    ::EndDialog(dlg, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    ::EndDialog(dlg, IDCANCEL);
                    return TRUE;
                default:
                    break;
            }
            break;
        default:
            break;
    }
    return FALSE;
}

// ----------------------------------------------------------- NumberInput

struct NumberCtx {
    double minValue;
    double maxValue;
    bool wholeNumbers;
    double* value;
    const std::wstring* title;
};

std::wstring FormatNumber(double v, bool whole) {
    if (whole) return Format(L"%d", static_cast<int>(std::llround(v)));
    return Format(L"%g", v);
}

INT_PTR CALLBACK NumberProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG: {
            ::SetWindowLongPtrW(dlg, DWLP_USER, static_cast<LONG_PTR>(lp));
            NumberCtx* ctx = reinterpret_cast<NumberCtx*>(lp);
            ::SetDlgItemTextW(dlg, kIdEdit1,
                              FormatNumber(*ctx->value, ctx->wholeNumbers).c_str());
            HWND edit = ::GetDlgItem(dlg, kIdEdit1);
            ::SendMessageW(edit, EM_SETSEL, 0, -1);
            ::SetFocus(edit);
            return FALSE;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDOK: {
                    NumberCtx* ctx =
                        reinterpret_cast<NumberCtx*>(::GetWindowLongPtrW(dlg, DWLP_USER));
                    const std::wstring text = Trim(ControlText(dlg, kIdEdit1));

                    wchar_t* end = nullptr;
                    const double parsed = ::wcstod(text.c_str(), &end);
                    const bool numeric =
                        !text.empty() && end && *end == L'\0';

                    // A rejected entry leaves the dialog open with the text
                    // intact: retyping a lead time from scratch because of a
                    // stray character is a punishment, not a correction.
                    if (!numeric) {
                        ::MessageBoxW(dlg, L"That is not a number.",
                                      ctx->title->c_str(),
                                      MB_OK | MB_ICONWARNING);
                        ::SetFocus(::GetDlgItem(dlg, kIdEdit1));
                        return TRUE;
                    }
                    if (ctx->wholeNumbers && parsed != std::floor(parsed)) {
                        ::MessageBoxW(dlg, L"Enter a whole number.",
                                      ctx->title->c_str(),
                                      MB_OK | MB_ICONWARNING);
                        ::SetFocus(::GetDlgItem(dlg, kIdEdit1));
                        return TRUE;
                    }
                    if (parsed < ctx->minValue || parsed > ctx->maxValue) {
                        const std::wstring msg =
                            Format(L"Enter a number between %s and %s.",
                                   FormatNumber(ctx->minValue, ctx->wholeNumbers).c_str(),
                                   FormatNumber(ctx->maxValue, ctx->wholeNumbers).c_str());
                        ::MessageBoxW(dlg, msg.c_str(), ctx->title->c_str(),
                                      MB_OK | MB_ICONWARNING);
                        ::SetFocus(::GetDlgItem(dlg, kIdEdit1));
                        return TRUE;
                    }

                    *ctx->value = parsed;
                    ::EndDialog(dlg, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    ::EndDialog(dlg, IDCANCEL);
                    return TRUE;
                default:
                    break;
            }
            break;
        default:
            break;
    }
    return FALSE;
}

// ------------------------------------------------------- DebugTimePicker

struct DebugTimeCtx {
    const TimeZone* zone;
    Seconds initial;
    Seconds* picked;
};

INT_PTR CALLBACK DebugTimeProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG: {
            ::SetWindowLongPtrW(dlg, DWLP_USER, static_cast<LONG_PTR>(lp));
            DebugTimeCtx* ctx = reinterpret_cast<DebugTimeCtx*>(lp);

            // The pickers show wall-clock time in the display zone, not UTC:
            // the user is choosing the moment they would see on the strip.
            const TimeZone::Parts p = ctx->zone->Break(ctx->initial);
            SYSTEMTIME st{};
            st.wYear = static_cast<WORD>(p.year);
            st.wMonth = static_cast<WORD>(p.month);
            st.wDay = static_cast<WORD>(p.day);
            st.wDayOfWeek = static_cast<WORD>(p.weekday);
            st.wHour = static_cast<WORD>(p.hour);
            st.wMinute = static_cast<WORD>(p.minute);
            st.wSecond = static_cast<WORD>(p.second);

            ::SendMessageW(::GetDlgItem(dlg, kIdDate), DTM_SETSYSTEMTIME,
                           GDT_VALID, reinterpret_cast<LPARAM>(&st));
            ::SendMessageW(::GetDlgItem(dlg, kIdTime), DTM_SETSYSTEMTIME,
                           GDT_VALID, reinterpret_cast<LPARAM>(&st));
            return TRUE;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDOK: {
                    DebugTimeCtx* ctx = reinterpret_cast<DebugTimeCtx*>(
                        ::GetWindowLongPtrW(dlg, DWLP_USER));
                    SYSTEMTIME date{}, time{};
                    ::SendMessageW(::GetDlgItem(dlg, kIdDate), DTM_GETSYSTEMTIME, 0,
                                   reinterpret_cast<LPARAM>(&date));
                    ::SendMessageW(::GetDlgItem(dlg, kIdTime), DTM_GETSYSTEMTIME, 0,
                                   reinterpret_cast<LPARAM>(&time));
                    // Two controls, one instant: the date comes from one and
                    // the time of day from the other, recombined in the
                    // display zone so a DST edge resolves the same way the
                    // strip would resolve it.
                    *ctx->picked = ctx->zone->Make(date.wYear, date.wMonth, date.wDay,
                                                   time.wHour, time.wMinute, time.wSecond);
                    ::EndDialog(dlg, 1);
                    return TRUE;
                }
                case kIdUseCurrent:
                    ::EndDialog(dlg, 2);
                    return TRUE;
                case IDCANCEL:
                    ::EndDialog(dlg, 0);
                    return TRUE;
                default:
                    break;
            }
            break;
        default:
            break;
    }
    return FALSE;
}

// ---------------------------------------------------------------- Confirm

typedef HRESULT(WINAPI* TaskDialogIndirectFn)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);

TaskDialogIndirectFn ResolveTaskDialog() {
    static TaskDialogIndirectFn fn = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        HMODULE h = ::GetModuleHandleW(L"comctl32.dll");
        if (!h) h = ::LoadLibraryW(L"comctl32.dll");
        if (h) {
            fn = reinterpret_cast<TaskDialogIndirectFn>(
                reinterpret_cast<void*>(::GetProcAddress(h, "TaskDialogIndirect")));
        }
    }
    return fn;
}

constexpr int kConfirmButtonId = 1001;

// ------------------------------------------------------------ file filters

// The common dialogs take a filter as a run of NUL-separated pairs terminated
// by a second NUL, which no std::wstring literal can express.
std::vector<wchar_t> BuildFilter(const std::wstring& label, const std::wstring& pattern) {
    std::vector<wchar_t> f;
    f.insert(f.end(), label.begin(), label.end());
    f.push_back(L'\0');
    f.insert(f.end(), pattern.begin(), pattern.end());
    f.push_back(L'\0');
    f.push_back(L'\0');
    return f;
}

}  // namespace

// ================================================================ TextInput

bool TextInput(HWND owner,
               const std::wstring& title,
               const std::wstring& prompt,
               const std::wstring& placeholder,
               std::wstring* value) {
    if (!value) return false;
    PrepareForeground(owner);

    const ItemDesc items[] = {
        {kClassStatic, prompt.c_str(), SS_LEFT, 0, 7, 7, 246, 16, kIdPrompt},
        {kClassEdit, L"", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE,
         7, 27, 246, 14, kIdEdit1},
        {kClassButton, L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 143, 51, 54, 14,
         static_cast<WORD>(IDOK)},
        {kClassButton, L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, 0, 201, 51, 54, 14,
         static_cast<WORD>(IDCANCEL)},
    };

    TemplateBuffer buf;
    BuildDialog(&buf, title, 262, 72, items, ARRAYSIZE(items));

    TextCtx ctx{&placeholder, value};
    const INT_PTR r = ::DialogBoxIndirectParamW(SelfInstance(), buf.Template(), owner,
                                                TextProc, reinterpret_cast<LPARAM>(&ctx));
    return r == IDOK;
}

// =========================================================== TwoFieldInput

bool TwoFieldInput(HWND owner,
                   const std::wstring& title,
                   const std::wstring& label1, const std::wstring& placeholder1,
                   const std::wstring& label2, const std::wstring& placeholder2,
                   std::wstring* value1, std::wstring* value2) {
    if (!value1 || !value2) return false;
    PrepareForeground(owner);

    const ItemDesc items[] = {
        {kClassStatic, label1.c_str(), SS_LEFT, 0, 7, 8, 300, 9, kIdPrompt},
        {kClassEdit, L"", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE,
         7, 20, 300, 14, kIdEdit1},
        {kClassStatic, label2.c_str(), SS_LEFT, 0, 7, 43, 300, 9, kIdLabel2},
        {kClassEdit, L"", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE,
         7, 55, 300, 14, kIdEdit2},
        {kClassButton, L"Save", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 197, 80, 54, 14,
         static_cast<WORD>(IDOK)},
        {kClassButton, L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, 0, 255, 80, 54, 14,
         static_cast<WORD>(IDCANCEL)},
    };

    TemplateBuffer buf;
    BuildDialog(&buf, title, 316, 101, items, ARRAYSIZE(items));

    TwoFieldCtx ctx{&placeholder1, &placeholder2, value1, value2};
    const INT_PTR r = ::DialogBoxIndirectParamW(SelfInstance(), buf.Template(), owner,
                                                TwoFieldProc, reinterpret_cast<LPARAM>(&ctx));
    return r == IDOK;
}

// ============================================================= NumberInput

bool NumberInput(HWND owner,
                 const std::wstring& title,
                 const std::wstring& prompt,
                 double minValue, double maxValue,
                 bool wholeNumbers,
                 double* value) {
    if (!value) return false;
    if (minValue > maxValue) std::swap(minValue, maxValue);
    PrepareForeground(owner);

    const ItemDesc items[] = {
        {kClassStatic, prompt.c_str(), SS_LEFT, 0, 7, 7, 246, 16, kIdPrompt},
        {kClassEdit, L"", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE,
         7, 27, 100, 14, kIdEdit1},
        {kClassButton, L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 143, 51, 54, 14,
         static_cast<WORD>(IDOK)},
        {kClassButton, L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, 0, 201, 51, 54, 14,
         static_cast<WORD>(IDCANCEL)},
    };

    TemplateBuffer buf;
    BuildDialog(&buf, title, 262, 72, items, ARRAYSIZE(items));

    NumberCtx ctx{minValue, maxValue, wholeNumbers, value, &title};
    const INT_PTR r = ::DialogBoxIndirectParamW(SelfInstance(), buf.Template(), owner,
                                                NumberProc, reinterpret_cast<LPARAM>(&ctx));
    return r == IDOK;
}

// ========================================================= DebugTimePicker

int DebugTimePicker(HWND owner, const TimeZone& zone, Seconds initial, Seconds* picked) {
    if (!picked) return 0;
    EnsureDateClasses();
    PrepareForeground(owner);

    // The picker draws its own frame; WS_BORDER or WS_EX_CLIENTEDGE on top of
    // it produces a doubled edge under the themed control.
    const DWORD dtpBase = WS_TABSTOP;

    const ItemDesc items[] = {
        {kClassStatic, L"Date", SS_LEFT, 0, 7, 10, 40, 9, kIdPrompt},
        {kClassDateTime, L"", dtpBase | DTS_SHORTDATECENTURYFORMAT, 0,
         50, 7, 120, 14, kIdDate},
        {kClassStatic, L"Time", SS_LEFT, 0, 7, 31, 40, 9, kIdLabel2},
        {kClassDateTime, L"", dtpBase | DTS_TIMEFORMAT, 0,
         50, 28, 120, 14, kIdTime},
        {kClassButton, L"Simulate", WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 88, 54, 54, 14,
         static_cast<WORD>(IDOK)},
        {kClassButton, L"Use Current Time", WS_TABSTOP | BS_PUSHBUTTON, 0, 146, 54, 86, 14,
         kIdUseCurrent},
        {kClassButton, L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, 0, 236, 54, 54, 14,
         static_cast<WORD>(IDCANCEL)},
    };

    TemplateBuffer buf;
    BuildDialog(&buf, L"Debug Time", 297, 75, items, ARRAYSIZE(items));

    DebugTimeCtx ctx{&zone, initial, picked};
    const INT_PTR r = ::DialogBoxIndirectParamW(SelfInstance(), buf.Template(), owner,
                                                DebugTimeProc, reinterpret_cast<LPARAM>(&ctx));
    if (r == 1) return 1;
    if (r == 2) return 2;
    return 0;
}

// ================================================================= Confirm

bool Confirm(HWND owner,
             const std::wstring& title,
             const std::wstring& body,
             const std::wstring& confirmLabel,
             bool destructive) {
    PrepareForeground(owner);

    // MessageBox cannot relabel its buttons, and "OK / Cancel" in front of a
    // wipe tells the user nothing about what OK does. TaskDialogIndirect can,
    // so it is preferred where it exists.
    if (TaskDialogIndirectFn taskDialog = ResolveTaskDialog()) {
        TASKDIALOG_BUTTON custom{};
        custom.nButtonID = kConfirmButtonId;
        custom.pszButtonText = confirmLabel.c_str();

        TASKDIALOGCONFIG cfg{};
        cfg.cbSize = sizeof(cfg);
        cfg.hwndParent = owner;
        cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION |
                      TDF_POSITION_RELATIVE_TO_WINDOW;
        cfg.dwCommonButtons = TDCBF_CANCEL_BUTTON;
        cfg.pszWindowTitle = title.c_str();
        cfg.pszMainIcon = TD_WARNING_ICON;
        cfg.pszContent = body.c_str();
        cfg.cButtons = 1;
        cfg.pButtons = &custom;
        // Return must not be the fast path to a wipe.
        cfg.nDefaultButton = destructive ? IDCANCEL : kConfirmButtonId;

        int pressed = IDCANCEL;
        const HRESULT hr = taskDialog(&cfg, &pressed, nullptr, nullptr);
        if (SUCCEEDED(hr)) return pressed == kConfirmButtonId;
        // Falls through on failure: TaskDialogIndirect returns E_INVALIDARG
        // when the process is running without a comctl32 v6 activation
        // context, which is a deployment mistake rather than a user error,
        // and a working dialog with worse labels beats no dialog at all.
    }

    UINT flags = MB_OKCANCEL | MB_ICONWARNING;
    if (destructive) flags |= MB_DEFBUTTON2;
    return ::MessageBoxW(owner, body.c_str(), title.c_str(), flags) == IDOK;
}

// ============================================================ Info / Error

void Info(HWND owner, const std::wstring& title, const std::wstring& body) {
    PrepareForeground(owner);
    ::MessageBoxW(owner, body.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
}

void Error(HWND owner, const std::wstring& title, const std::wstring& body) {
    PrepareForeground(owner);
    ::MessageBoxW(owner, body.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
}

// ================================================== OpenFile / SaveFile

bool OpenFile(HWND owner,
              const std::wstring& title,
              const std::wstring& filterLabel,
              const std::wstring& filterPattern,
              std::wstring* path) {
    if (!path) return false;
    PrepareForeground(owner);

    std::vector<wchar_t> filter = BuildFilter(filterLabel, filterPattern);
    std::vector<wchar_t> buffer(4096, L'\0');

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter.data();
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrTitle = title.c_str();
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (!::GetOpenFileNameW(&ofn)) return false;
    *path = buffer.data();
    return !path->empty();
}

bool SaveFile(HWND owner,
              const std::wstring& title,
              const std::wstring& defaultName,
              const std::wstring& filterLabel,
              const std::wstring& filterPattern,
              std::wstring* path) {
    if (!path) return false;
    PrepareForeground(owner);

    std::vector<wchar_t> filter = BuildFilter(filterLabel, filterPattern);
    std::vector<wchar_t> buffer(4096, L'\0');
    const size_t copy = defaultName.size() < buffer.size() - 1 ? defaultName.size()
                                                              : buffer.size() - 1;
    ::wmemcpy(buffer.data(), defaultName.c_str(), copy);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter.data();
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrTitle = title.c_str();
    // OFN_FILEMUSTEXIST is deliberately absent here: an export is normally to
    // a name that does not exist yet, and requiring one that does would make
    // the dialog refuse every correct answer.
    ofn.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (!::GetSaveFileNameW(&ofn)) return false;
    *path = buffer.data();
    return !path->empty();
}

}  // namespace dialogs
}  // namespace rc
