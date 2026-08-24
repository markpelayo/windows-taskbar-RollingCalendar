// autostart.cpp — see autostart.h for what and why.
//
// Task Scheduler's object model is a COM automation interface, so this file is
// mostly ceremony: connect, walk to the root folder, build a definition out of
// half a dozen sub-objects, register it. The interesting decisions are few and
// commented where they are made.
//
// Nothing here keeps a COM apartment alive. Each entry point opens its own
// rc::ComScope and closes it on the way out. Initialising COM twice in a
// session costs a few hundred microseconds; holding a global apartment open
// for the life of a process, for a feature the user touches once and then
// forgets, costs a proxy manager, an RPC thread and a class of shutdown-order
// bug that only shows up on other people's machines.

#include "autostart.h"

#define SECURITY_WIN32

#include <windows.h>
#include <security.h>
#include <taskschd.h>

#include <cstdio>
#include <cwchar>

#include "raii.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "secur32.lib")

namespace rc {
namespace autostart {

const wchar_t* const kTaskName = L"RollingCalendar";

namespace {

// A BSTR is not a wchar_t*: it carries its length in the four bytes before the
// pointer and must go back to the OLE allocator. Every string handed to or
// taken from these interfaces is one, and forgetting a single SysFreeString in
// a function with eight of them is the obvious failure mode. This makes it not
// a decision.
class BStr {
public:
    BStr() = default;
    explicit BStr(const wchar_t* s) : b_(::SysAllocString(s ? s : L"")) {}
    explicit BStr(const std::wstring& s) : b_(::SysAllocString(s.c_str())) {}
    ~BStr() { free(); }

    BStr(const BStr&) = delete;
    BStr& operator=(const BStr&) = delete;

    BSTR get() const { return b_; }
    BSTR* put() {
        free();
        return &b_;
    }
    bool empty() const { return b_ == nullptr || *b_ == L'\0'; }
    std::wstring str() const { return b_ ? std::wstring(b_) : std::wstring(); }

private:
    void free() {
        if (b_) {
            ::SysFreeString(b_);
            b_ = nullptr;
        }
    }
    BSTR b_ = nullptr;
};

// RegisterTaskDefinition takes VARIANTs it will not read when the logon type
// is an interactive token; VT_EMPTY is how "not supplied" is spelled.
struct EmptyVariant {
    VARIANT v;
    EmptyVariant() { ::VariantInit(&v); }
    ~EmptyVariant() { ::VariantClear(&v); }
    EmptyVariant(const EmptyVariant&) = delete;
    EmptyVariant& operator=(const EmptyVariant&) = delete;
};

constexpr const wchar_t* kRootFolder = L"\\";
constexpr const wchar_t* kAuthor = L"Rolling Calendar";
constexpr const wchar_t* kTriggerId = L"RollingCalendarLogon";
constexpr const wchar_t* kActionId = L"RollingCalendarExec";

bool Fail(std::wstring* error, const wchar_t* what, HRESULT hr) {
    if (error) {
        // The HRESULT goes in the message verbatim. It is the only thing the
        // user can usefully paste into a search or a bug report, and the
        // friendly text alone never distinguishes "policy said no" from
        // "the service is not running".
        *error = Format(L"%s (0x%08X). Group Policy or a managed configuration "
                        L"can block scheduled-task creation; if so, put a "
                        L"shortcut to Rolling Calendar in shell:startup "
                        L"instead, which starts it without a delay.",
                        what, static_cast<unsigned>(hr));
    }
    return false;
}

std::wstring FolderOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring();
    return path.substr(0, slash);
}

// "PT20S" and friends. Only the forms this file writes need to round-trip, but
// a task edited by hand in the Task Scheduler UI can come back as "PT1M30S",
// so hours and minutes are honoured too. Anything unrecognised reads as zero,
// which degrades to "start immediately" rather than to a wrong delay.
int ParseIsoDurationSeconds(const std::wstring& s) {
    if (s.empty() || s[0] != L'P') return 0;
    long long total = 0;
    long long value = 0;
    bool haveDigits = false;
    bool inTime = false;

    for (size_t i = 1; i < s.size(); ++i) {
        const wchar_t c = s[i];
        if (c == L'T') {
            inTime = true;
            value = 0;
            haveDigits = false;
            continue;
        }
        if (c >= L'0' && c <= L'9') {
            value = value * 10 + (c - L'0');
            haveDigits = true;
            continue;
        }
        if (!haveDigits) return 0;
        switch (c) {
            case L'D': total += value * 86400; break;
            case L'H': total += value * 3600; break;
            // Before the T separator M is months, which this app never writes
            // and which has no fixed length in seconds. Ignored deliberately.
            case L'M': if (inTime) total += value * 60; break;
            case L'S': total += value; break;
            default: return 0;
        }
        value = 0;
        haveDigits = false;
    }

    if (total < 0) return 0;
    if (total > 86400) return 86400;
    return static_cast<int>(total);
}

std::wstring CurrentUserSam() {
    ULONG size = 0;
    ::GetUserNameExW(NameSamCompatible, nullptr, &size);
    if (size == 0) return std::wstring();
    std::wstring buf(size, L'\0');
    if (!::GetUserNameExW(NameSamCompatible, &buf[0], &size)) return std::wstring();
    buf.resize(size);
    return buf;
}

// Connects to the local scheduler and returns its root folder. Callers must
// already hold a ComScope.
HRESULT OpenRootFolder(ComPtr<ITaskService>* service, ComPtr<ITaskFolder>* root) {
    HRESULT hr = ::CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_ITaskService,
                                    reinterpret_cast<void**>(service->put()));
    if (FAILED(hr)) return hr;

    EmptyVariant server, user, domain, password;
    hr = (*service)->Connect(server.v, user.v, domain.v, password.v);
    if (FAILED(hr)) return hr;

    BStr path(kRootFolder);
    return (*service)->GetFolder(path.get(), root->put());
}

// Reads the registered task, if any. `execPath` and `delaySeconds` are only
// written when the task exists and carries them.
bool ReadRegisteredTask(std::wstring* execPath, int* delaySeconds) {
    ComScope com;
    if (!com.ok()) return false;

    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    if (FAILED(OpenRootFolder(&service, &root))) return false;

    ComPtr<IRegisteredTask> task;
    BStr name(kTaskName);
    if (FAILED(root->GetTask(name.get(), task.put()))) return false;

    ComPtr<ITaskDefinition> def;
    if (FAILED(task->get_Definition(def.put()))) return false;

    if (execPath) {
        ComPtr<IActionCollection> actions;
        if (SUCCEEDED(def->get_Actions(actions.put()))) {
            ComPtr<IAction> action;
            if (SUCCEEDED(actions->get_Item(1, action.put()))) {
                ComPtr<IExecAction> exec;
                if (SUCCEEDED(action->QueryInterface(
                        IID_IExecAction, reinterpret_cast<void**>(exec.put())))) {
                    BStr path;
                    if (SUCCEEDED(exec->get_Path(path.put()))) *execPath = path.str();
                }
            }
        }
    }

    if (delaySeconds) {
        *delaySeconds = 0;
        ComPtr<ITriggerCollection> triggers;
        if (SUCCEEDED(def->get_Triggers(triggers.put()))) {
            ComPtr<ITrigger> trigger;
            if (SUCCEEDED(triggers->get_Item(1, trigger.put()))) {
                ComPtr<ILogonTrigger> logon;
                if (SUCCEEDED(trigger->QueryInterface(
                        IID_ILogonTrigger, reinterpret_cast<void**>(logon.put())))) {
                    BStr delay;
                    if (SUCCEEDED(logon->get_Delay(delay.put())) && !delay.empty()) {
                        *delaySeconds = ParseIsoDurationSeconds(delay.str());
                    }
                }
            }
        }
    }

    return true;
}

}  // namespace

// ================================================================== queries

bool IsEnabled() {
    return ReadRegisteredTask(nullptr, nullptr);
}

int DelaySeconds() {
    int delay = 0;
    if (!ReadRegisteredTask(nullptr, &delay)) return 0;
    return delay;
}

std::wstring Describe() {
    int delay = 0;
    if (!ReadRegisteredTask(nullptr, &delay)) return L"Off";
    if (delay <= 0) return L"On";
    return Format(L"After %d s", delay);
}

// =================================================================== Enable

bool Enable(int delaySeconds, std::wstring* error) {
    if (error) error->clear();
    if (delaySeconds < 0) delaySeconds = 0;

    ComScope com;
    if (!com.ok()) return Fail(error, L"COM could not be initialised", E_FAIL);

    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    HRESULT hr = OpenRootFolder(&service, &root);
    if (FAILED(hr)) return Fail(error, L"Could not reach the Task Scheduler service", hr);

    ComPtr<ITaskDefinition> def;
    hr = service->NewTask(0, def.put());
    if (FAILED(hr)) return Fail(error, L"Could not create a task definition", hr);

    // ---- registration info
    {
        ComPtr<IRegistrationInfo> info;
        hr = def->get_RegistrationInfo(info.put());
        if (FAILED(hr)) return Fail(error, L"Could not set the task author", hr);
        BStr author(kAuthor);
        info->put_Author(author.get());
        BStr description(L"Starts Rolling Calendar when you sign in.");
        info->put_Description(description.get());
    }

    // ---- principal
    {
        ComPtr<IPrincipal> principal;
        hr = def->get_Principal(principal.put());
        if (FAILED(hr)) return Fail(error, L"Could not set the task principal", hr);
        principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
        // TASK_RUNLEVEL_LUA, not HIGHEST: the app writes to %APPDATA% and
        // fetches over HTTPS and needs no privilege beyond the user's own.
        // Asking for elevation would put a shield on the entry and, on an
        // account with UAC, a consent prompt at every sign-in -- in exchange
        // for rights it would never use.
        principal->put_RunLevel(TASK_RUNLEVEL_LUA);
    }

    // ---- logon trigger
    {
        ComPtr<ITriggerCollection> triggers;
        hr = def->get_Triggers(triggers.put());
        if (FAILED(hr)) return Fail(error, L"Could not create the logon trigger", hr);

        ComPtr<ITrigger> trigger;
        hr = triggers->Create(TASK_TRIGGER_LOGON, trigger.put());
        if (FAILED(hr)) return Fail(error, L"Could not create the logon trigger", hr);

        ComPtr<ILogonTrigger> logon;
        hr = trigger->QueryInterface(IID_ILogonTrigger,
                                     reinterpret_cast<void**>(logon.put()));
        if (FAILED(hr)) return Fail(error, L"Could not configure the logon trigger", hr);

        BStr id(kTriggerId);
        logon->put_Id(id.get());

        // Scoping the trigger to this user keeps the task from firing in every
        // other profile on a shared machine. If the name lookup fails the
        // field is left unset, which means "any user" -- a slightly too broad
        // trigger beats no autostart at all.
        const std::wstring sam = CurrentUserSam();
        if (!sam.empty()) {
            BStr userId(sam);
            logon->put_UserId(userId.get());
        }

        if (delaySeconds > 0) {
            BStr delay(Format(L"PT%dS", delaySeconds));
            logon->put_Delay(delay.get());
        }
    }

    // ---- exec action
    {
        const std::wstring exe = ExecutablePath();
        if (exe.empty()) return Fail(error, L"Could not determine this program's path", E_FAIL);

        ComPtr<IActionCollection> actions;
        hr = def->get_Actions(actions.put());
        if (FAILED(hr)) return Fail(error, L"Could not create the task action", hr);

        ComPtr<IAction> action;
        hr = actions->Create(TASK_ACTION_EXEC, action.put());
        if (FAILED(hr)) return Fail(error, L"Could not create the task action", hr);

        ComPtr<IExecAction> exec;
        hr = action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(exec.put()));
        if (FAILED(hr)) return Fail(error, L"Could not configure the task action", hr);

        BStr id(kActionId);
        exec->put_Id(id.get());
        BStr path(exe);
        exec->put_Path(path.get());
        // A scheduled task otherwise starts in %WINDIR%\system32, which is
        // both wrong and a place the app has no business being.
        BStr dir(FolderOf(exe));
        exec->put_WorkingDirectory(dir.get());
    }

    // ---- settings
    {
        ComPtr<ITaskSettings> settings;
        hr = def->get_Settings(settings.put());
        if (FAILED(hr)) return Fail(error, L"Could not configure the task settings", hr);

        // The defaults are written for batch jobs, not for a desktop widget:
        // left alone, the task would refuse to start on a laptop running on
        // battery and would then be killed after three days of uptime.
        settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        BStr noLimit(L"PT0S");
        settings->put_ExecutionTimeLimit(noLimit.get());
        settings->put_StartWhenAvailable(VARIANT_TRUE);
        settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
        settings->put_Hidden(VARIANT_FALSE);
        settings->put_Enabled(VARIANT_TRUE);
        settings->put_AllowDemandStart(VARIANT_TRUE);
    }

    EmptyVariant user, password, sddl;
    ComPtr<IRegisteredTask> registered;
    BStr name(kTaskName);
    hr = root->RegisterTaskDefinition(name.get(), def.get(), TASK_CREATE_OR_UPDATE,
                                      user.v, password.v, TASK_LOGON_INTERACTIVE_TOKEN,
                                      sddl.v, registered.put());
    if (FAILED(hr)) return Fail(error, L"Windows refused to register the startup task", hr);

    return true;
}

// ================================================================== Disable

bool Disable(std::wstring* error) {
    if (error) error->clear();

    ComScope com;
    if (!com.ok()) return Fail(error, L"COM could not be initialised", E_FAIL);

    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    HRESULT hr = OpenRootFolder(&service, &root);
    if (FAILED(hr)) return Fail(error, L"Could not reach the Task Scheduler service", hr);

    BStr name(kTaskName);
    hr = root->DeleteTask(name.get(), 0);

    // Nothing to delete is the state the caller asked for, so it is a success.
    // Reporting it as a failure would make a second click on Off produce an
    // error message about a task that is already gone.
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
        hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) {
        return true;
    }
    if (FAILED(hr)) return Fail(error, L"Windows refused to remove the startup task", hr);
    return true;
}

// ============================================================ RepairIfMoved

void RepairIfMoved() {
    std::wstring registeredPath;
    int delay = 0;
    if (!ReadRegisteredTask(&registeredPath, &delay)) return;   // not enabled
    if (registeredPath.empty()) return;

    const std::wstring current = ExecutablePath();
    if (current.empty()) return;

    // Case-insensitive because NTFS is, and because the path came back from
    // the scheduler in whatever case it was registered with, not in the case
    // the file system uses today.
    if (::_wcsicmp(registeredPath.c_str(), current.c_str()) == 0) return;

    // Move the folder and the entry corrects itself on the next run rather
    // than pointing at a file that is no longer there. The alternative is an
    // autostart that silently stops working and a Task Scheduler entry the
    // user has to find and delete by hand.
    std::wstring error;
    Enable(delay, &error);
}

}  // namespace autostart
}  // namespace rc
