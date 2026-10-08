// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone, microphone-free helper. No registry or endpoint volume changes.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <atlbase.h>
#include <algorithm>
#include <atomic>
#include <cwctype>
#include <map>
#include <memory>
#include <set>
#include <string>
#include "AudioGuardProtocol.h"
#include "BluetoothRecovery.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

static const GUID kMuteContext = { 0xa7d851e1, 0x5b09, 0x446c, { 0x98, 0xa6, 0x64, 0x47, 0x9b, 0x5c, 0x3c, 0x21 } };

static std::wstring Lower(std::wstring text)
{
    std::transform(text.begin(), text.end(), text.begin(), towlower);
    return text;
}

static std::set<std::wstring> ParseApps(const std::wstring& text)
{
    std::set<std::wstring> result;
    size_t begin = 0;
    while (begin < text.size()) {
        size_t end = text.find_first_of(L";,\r\n", begin);
        std::wstring name = text.substr(begin, end == std::wstring::npos ? end : end - begin);
        const auto first = name.find_first_not_of(L" \t\"");
        const auto last = name.find_last_not_of(L" \t\"");
        if (first != std::wstring::npos) {
            name = name.substr(first, last - first + 1);
            const auto slash = name.find_last_of(L"\\/");
            if (slash != std::wstring::npos) name = name.substr(slash + 1);
            if (!name.empty()) result.insert(Lower(name));
        }
        if (end == std::wstring::npos) break;
        begin = end + 1;
    }
    return result;
}

static std::wstring ProcessName(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[32768];
    DWORD length = _countof(path);
    const BOOL ok = QueryFullProcessImageNameW(process, 0, path, &length);
    CloseHandle(process);
    if (!ok) return {};
    std::wstring name(path, length);
    return Lower(name.substr(name.find_last_of(L"\\/") + 1));
}

class SessionEvents : public IAudioSessionEvents {
public:
    std::atomic<bool> externalMuteChange{false};
    std::atomic<bool> lastMute{false};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IAudioSessionEvents)) {
            *out = static_cast<IAudioSessionEvents*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG refs = --refs_; if (!refs) delete this; return refs;
    }
    HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(float, BOOL mute, LPCGUID context) override {
        const bool previous = lastMute.exchange(mute != FALSE);
        // A volume-only notification while muted retains our lease. An unmute,
        // or a mute transition before we acquired it, belongs to the user. Once
        // relinquished it stays relinquished through call and recovery alike.
        if ((!context || *context != kMuteContext) && (!mute || !previous))
            externalMuteChange = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDisplayNameChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged(DWORD, float[], DWORD, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnStateChanged(AudioSessionState) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSessionDisconnected(AudioSessionDisconnectReason) override { return S_OK; }
private:
    std::atomic<ULONG> refs_{1};
};

struct MutedSession {
    CComPtr<IAudioSessionControl2> control;
    CComPtr<ISimpleAudioVolume> volume;
    CComPtr<SessionEvents> events;
    bool changed = false;
    ~MutedSession() {
        // Destruction (including expired-session cleanup) is never permission
        // to unmute. Only the transport/capture gate calls Restore().
        if (events) control->UnregisterAudioSessionNotification(events);
    }
    bool Restore() {
        if (changed && events && !events->externalMuteChange) {
            BOOL muted = FALSE;
            HRESULT result = volume->GetMute(&muted);
            if (SUCCEEDED(result) && muted && !events->externalMuteChange)
                result = volume->SetMute(FALSE, &kMuteContext);
            if (FAILED(result)) {
                AudioSessionState state;
                if (FAILED(control->GetState(&state)) || state != AudioSessionStateExpired) return false;
            }
        }
        changed = false;
        return true;
    }
};

// A restarted copy can begin a call while its predecessor still owns mutes.
// Publish only the local need for a hold, not whether restoration is pending:
// otherwise two helpers waiting for one another could never finish. The same
// mutex serializes a new lease with the actual restoration of old sessions.
class HoldCoordinator {
    struct Slot { DWORD pid; FILETIME created; LONG needed; };
    struct Shared { LONG version; LONG overrideSerial; Slot slots[64]; };
    HANDLE mutex_ = nullptr;
    HANDLE mapping_ = nullptr;
    Shared* shared_ = nullptr;
    int slot_ = -1;
    LONG overrideSerial_ = 0;
    bool restoreLocked_ = false;
    bool Locked() const {
        if (!mutex_) return false;
        const DWORD result = WaitForSingleObject(mutex_, 10000);
        return result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }
    void Prune() {
        for (Slot& slot : shared_->slots) {
            if (!slot.pid) continue;
            HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, slot.pid);
            bool dead = false;
            if (process) {
                FILETIME created{}, exited{}, kernel{}, user{};
                dead = WaitForSingleObject(process, 0) == WAIT_OBJECT_0 ||
                    (GetProcessTimes(process, &created, &exited, &kernel, &user) &&
                     CompareFileTime(&created, &slot.created) != 0);
                CloseHandle(process);
            } else dead = GetLastError() == ERROR_INVALID_PARAMETER;
            if (dead) ZeroMemory(&slot, sizeof(slot));
        }
    }
public:
    HoldCoordinator() {
        mutex_ = CreateMutexW(nullptr, FALSE, L"Local\\MicroSIPAudioGuard.Holds.v1.Lock");
        if (!Locked()) return;
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            sizeof(Shared), L"Local\\MicroSIPAudioGuard.Holds.v1");
        const bool fresh = mapping_ && GetLastError() != ERROR_ALREADY_EXISTS;
        if (mapping_) shared_ = static_cast<Shared*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (shared_) {
            if (fresh) { ZeroMemory(shared_, sizeof(*shared_)); shared_->version = 1; }
            if (shared_->version == 1) {
                Prune();
                FILETIME created{}, exited{}, kernel{}, user{};
                if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
                    for (int i = 0; i < _countof(shared_->slots); ++i) if (!shared_->slots[i].pid) {
                        shared_->slots[i] = {GetCurrentProcessId(), created, FALSE}; slot_ = i; break;
                    }
                }
                overrideSerial_ = shared_->overrideSerial;
            }
        }
        ReleaseMutex(mutex_);
    }
    ~HoldCoordinator() {
        if (slot_ >= 0 && Locked()) { ZeroMemory(&shared_->slots[slot_], sizeof(Slot)); ReleaseMutex(mutex_); }
        if (shared_) UnmapViewOfFile(shared_);
        if (mapping_) CloseHandle(mapping_);
        if (mutex_) CloseHandle(mutex_);
    }
    bool NeedHold() {
        if (slot_ < 0 || !Locked()) return false;
        shared_->slots[slot_].needed = TRUE;
        ReleaseMutex(mutex_);
        return true;
    }
    // On success the caller owns the mutex through all session restoration.
    bool BeginRestore(bool force) {
        restoreLocked_ = false;
        if (slot_ < 0 || !Locked()) return force;
        shared_->slots[slot_].needed = FALSE;
        Prune();
        if (!force) for (const Slot& slot : shared_->slots) if (slot.pid && slot.needed) {
            ReleaseMutex(mutex_); return false;
        }
        restoreLocked_ = true;
        return true;
    }
    void EndRestore() {
        if (restoreLocked_) { restoreLocked_ = false; ReleaseMutex(mutex_); }
    }
    void BroadcastOverride() {
        if (slot_ >= 0 && Locked()) { ++shared_->overrideSerial; ReleaseMutex(mutex_); }
    }
    bool ConsumeOverride() {
        if (slot_ < 0 || !Locked()) return false;
        const bool changed = overrideSerial_ != shared_->overrideSerial;
        overrideSerial_ = shared_->overrideSerial;
        ReleaseMutex(mutex_);
        return changed;
    }
};

class AudioGuard {
public:
    AudioGuard(DWORD parent, int mode, const std::wstring& apps)
        : parent_(parent), mode_(mode), apps_(ParseApps(apps)) {
        holding_ = mode == 1 || mode == 2;
        if (holding_) coordinated_ = coordinator_.NeedHold();
    }

    void BeginHold(int mode, const std::wstring& apps) {
        mode_ = mode;
        apps_ = ParseApps(apps);
        holding_ = true;
        restoring_ = false;
        restoreForced_ = false;
        coordinated_ = coordinator_.NeedHold();
        // Do not clear sessions here: a new call during recovery must retain
        // original pre-mute state and manual overrides, with no audible gap.
    }
    bool IsHolding() const { return holding_; }
    bool IsRestoring() const { return restoring_; }
    void RequestRestoreNow() { coordinator_.BroadcastOverride(); restoreForced_ = true; }
    bool ConsumeRestoreRequest() {
        if (!coordinator_.ConsumeOverride()) return false;
        restoreForced_ = true;
        return true;
    }
    void Restore(bool force = false) {
        restoring_ = true;
        restoreForced_ = restoreForced_ || force;
        if (!sessions_.empty() && !coordinator_.BeginRestore(restoreForced_)) return;
        const bool locked = !sessions_.empty();
        if (!locked) {
            // An empty guard still has to withdraw its local hold vote.
            if (coordinator_.BeginRestore(restoreForced_)) coordinator_.EndRestore();
        }
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (it->second->Restore()) it = sessions_.erase(it);
            else ++it;
        }
        if (locked) coordinator_.EndRestore();
        holding_ = !sessions_.empty();
        if (!holding_) restoring_ = false;
    }

    bool Scan() {
        if (!holding_ || restoring_) return true;
        CComPtr<IMMDeviceEnumerator> devices;
        if (FAILED(devices.CoCreateInstance(__uuidof(MMDeviceEnumerator)))) return false;
        CComPtr<IMMDeviceCollection> outputs;
        if (FAILED(devices->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &outputs))) return false;
        UINT count = 0;
        if (FAILED(outputs->GetCount(&count))) return false;
        bool success = true;
        for (UINT i = 0; i < count; ++i) {
            CComPtr<IMMDevice> device;
            CComPtr<IAudioSessionManager2> manager;
            CComPtr<IAudioSessionEnumerator> sessions;
            if (FAILED(outputs->Item(i, &device)) ||
                FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                    reinterpret_cast<void**>(&manager))) ||
                FAILED(manager->GetSessionEnumerator(&sessions))) { success = false; continue; }
            int sessionCount = 0;
            if (FAILED(sessions->GetCount(&sessionCount))) continue;
            for (int j = 0; j < sessionCount; ++j) {
                CComPtr<IAudioSessionControl> control;
                if (FAILED(sessions->GetSession(j, &control))) continue;
                CComQIPtr<IAudioSessionControl2> extended(control);
                if (extended && !Consider(extended)) success = false;
            }
        }
        // Release expired sessions so long calls do not retain dead players.
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            AudioSessionState state;
            if (SUCCEEDED(it->second->control->GetState(&state)) && state == AudioSessionStateExpired)
                it = sessions_.erase(it);
            else ++it;
        }
        return success && coordinated_;
    }

private:
    bool Consider(IAudioSessionControl2* control) {
        DWORD pid = 0;
        if (FAILED(control->GetProcessId(&pid)) || !pid || pid == parent_ ||
            pid == GetCurrentProcessId() || control->IsSystemSoundsSession() == S_OK) return true;
        if (mode_ == 2 && apps_.count(ProcessName(pid)) == 0) return true;
        AudioSessionState state;
        if (FAILED(control->GetState(&state)) || state == AudioSessionStateExpired) return true;
        LPWSTR rawId = nullptr;
        if (FAILED(control->GetSessionInstanceIdentifier(&rawId))) return false;
        std::wstring id(rawId);
        CoTaskMemFree(rawId);
        if (sessions_.count(id)) return true;
        CComQIPtr<ISimpleAudioVolume> volume(control);
        BOOL muted = FALSE;
        if (!volume || FAILED(volume->GetMute(&muted))) return false;
        auto entry = std::make_unique<MutedSession>();
        entry->control = control;
        entry->volume = volume;
        if (!muted) {
            entry->events.Attach(new SessionEvents());
            if (FAILED(control->RegisterAudioSessionNotification(entry->events))) return false;
            // Re-read after subscribing so a concurrent manual mute is never
            // claimed just because our first observation saw an unmuted app.
            if (FAILED(volume->GetMute(&muted))) return false;
            if (!muted && !entry->events->externalMuteChange) {
                entry->changed = SUCCEEDED(volume->SetMute(TRUE, &kMuteContext));
                if (!entry->changed) return false; // retry on the next scan
            }
        }
        sessions_.emplace(id, std::move(entry));
        return true;
    }
    DWORD parent_;
    int mode_;
    std::set<std::wstring> apps_;
    std::map<std::wstring, std::unique_ptr<MutedSession>> sessions_;
    bool holding_ = true;
    bool restoring_ = false;
    bool restoreForced_ = false;
    bool coordinated_ = false;
    HoldCoordinator coordinator_;
};

#ifndef AUDIO_GUARD_NO_MAIN
namespace {
constexpr UINT kTrayMessage = WM_APP + 1;
struct RecoveryTray {
    HWND window = nullptr;
    bool visible = false;
    bool restoreRequested = false;
    NOTIFYICONDATAW icon = {};
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<RecoveryTray*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<RecoveryTray*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == kTrayMessage && (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP)) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"Restore audio now");
            POINT point; GetCursorPos(&point);
            SetForegroundWindow(window);
            if (TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                point.x, point.y, 0, window, nullptr) == 1) self->restoreRequested = true;
            DestroyMenu(menu);
            PostMessageW(window, WM_NULL, 0, 0);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
    RecoveryTray() {
        WNDCLASSW cls = {};
        cls.lpfnWndProc = WindowProc;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"MicroSIPAudioRecoveryGuard";
        RegisterClassW(&cls);
        window = CreateWindowW(cls.lpszClassName, L"MicroSIP audio recovery", 0,
            0, 0, 0, 0, nullptr, nullptr, cls.hInstance, this);
        icon.cbSize = sizeof(icon);
        icon.hWnd = window;
        icon.uID = 1;
        icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        icon.uCallbackMessage = kTrayMessage;
        icon.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
    }
    void Show(bool show, bool unknown) {
        if (!window) return;
        if (show) {
            wcscpy_s(icon.szTip, unknown ? L"MicroSIP: audio recovery unknown. Click to restore audio."
                : L"MicroSIP: waiting for Bluetooth release. Click to restore audio.");
            if (Shell_NotifyIconW(visible ? NIM_MODIFY : NIM_ADD, &icon)) visible = true;
        } else if (visible) { Shell_NotifyIconW(NIM_DELETE, &icon); visible = false; }
    }
    ~RecoveryTray() { Show(false, false); if (window) DestroyWindow(window); }
};

int RunGuard(AudioGuardProtocol::Shared* shared, HANDLE command, HANDLE ack, HANDLE owner, DWORD parent)
{
    using namespace AudioGuardProtocol;
    BluetoothRecovery::Observer recovery;
    const bool observerAvailable = recovery.Start();
    RecoveryTray tray;
    const std::wstring statusName = L"Local\\MicroSIPAudioGuard.Status." + std::to_wstring(GetCurrentProcessId());
    HANDLE statusMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
        sizeof(Status), statusName.c_str());
    auto* status = statusMapping ? static_cast<Status*>(MapViewOfFile(statusMapping, FILE_MAP_ALL_ACCESS, 0, 0, 0)) : nullptr;
    if (status) { ZeroMemory(status, sizeof(*status)); status->version = Version; status->parent = parent; }
    AudioGuard guard(parent, 0, L"");
    guard.Restore();
    bool active = false;
    bool captureReleased = true;
    bool captureArmed = false;
    bool detached = false;
    LONG acknowledged = 0;
    InterlockedExchange(&shared->ready, 1);
    SetEvent(ack);
    for (;;) {
        HANDLE waits[] = { command, owner };
        MsgWaitForMultipleObjects(detached ? 1 : 2, waits, FALSE, 100, QS_ALLINPUT);
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        const LONG sequence = InterlockedCompareExchange(&shared->sequence, 0, 0);
        if (sequence != acknowledged) {
            bool result = true;
            const int mode = shared->mode;
            if (mode < 1 || mode > 2 || shared->input[_countof(shared->input) - 1] ||
                shared->output[_countof(shared->output) - 1] || shared->apps[_countof(shared->apps) - 1]) result = false;
            else if (shared->command == PrepareCapture) {
                active = true;
                guard.BeginHold(mode, shared->apps);
                // Observe and mute before the owner opens a duplex stream. A
                // failed handshake never releases an existing mute lease.
                result = observerAvailable && guard.Scan();
                recovery.Arm(shared->input, shared->output);
                captureArmed = true;
                captureReleased = false;
            } else if (shared->command == Update) {
                active = shared->active != 0;
                captureReleased = shared->captureReleased != 0;
                if (active) guard.BeginHold(mode, shared->apps);
                if (!captureReleased && !captureArmed) {
                    // Muting may have been enabled after a call opened capture.
                    // The controller caches the actual device names even while
                    // muting is off; a future matching disconnect is required.
                    recovery.Arm(shared->input, shared->output);
                    captureArmed = true;
                }
                if (captureReleased) recovery.CaptureClosed();
                result = guard.Scan();
            } else result = false;
            InterlockedExchange(&shared->result, result);
            acknowledged = sequence;
            InterlockedExchange(&shared->acknowledged, acknowledged);
            SetEvent(ack);
        }
        if (InterlockedExchange(&shared->unobservedCapture, 0)) {
            recovery.Arm(L"<unobserved capture>", L"<unobserved capture>");
            captureArmed = true;
        }
        if (!detached && (InterlockedCompareExchange(&shared->detached, 0, 0) ||
            WaitForSingleObject(owner, 0) == WAIT_OBJECT_0)) {
            detached = true;
            active = false;
            captureReleased = true;
            recovery.CaptureClosed();
        }
        const bool forceRestore = InterlockedExchange(&shared->forceRestore, 0) != 0 || tray.restoreRequested;
        tray.restoreRequested = false;
        if (forceRestore) guard.RequestRestoreNow();
        const bool sharedOverride = guard.ConsumeRestoreRequest();
        if (forceRestore || sharedOverride || guard.IsRestoring() || (!active && captureReleased && recovery.Ready())) {
            guard.Restore();
            recovery.Reset();
            captureArmed = false;
        }
        guard.Scan(); // includes sessions created during Bluetooth recovery
        LONG state = Idle;
        if (guard.IsHolding()) state = active ? Holding : recovery.Unknown() ? Unknown : Recovering;
        InterlockedExchange(&shared->state, state);
        if (status) {
            InterlockedExchange(&status->state, state);
            InterlockedExchange(&status->captureReleased, captureReleased);
            InterlockedExchange(&status->observerAvailable, observerAvailable);
        }
        tray.Show(guard.IsHolding() && !active && (detached || state == Unknown), state == Unknown);
        if (detached && !guard.IsHolding()) break;
    }
    if (status) UnmapViewOfFile(status);
    if (statusMapping) CloseHandle(statusMapping);
    return 0;
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 5) return 2;
    HANDLE mapping = reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(_wcstoui64(argv[1], nullptr, 10)));
    HANDLE command = reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(_wcstoui64(argv[2], nullptr, 10)));
    HANDLE ack = reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(_wcstoui64(argv[3], nullptr, 10)));
    const DWORD parent = wcstoul(argv[4], nullptr, 10);
    if (!parent) return 2;
    auto* shared = static_cast<AudioGuardProtocol::Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0,
        sizeof(AudioGuardProtocol::Shared)));
    HANDLE owner = OpenProcess(SYNCHRONIZE, FALSE, parent);
    if (!shared || !owner || shared->version != AudioGuardProtocol::Version) return 2;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 3;
    const int result = RunGuard(shared, command, ack, owner, parent);
    CoUninitialize();
    UnmapViewOfFile(shared);
    CloseHandle(mapping); CloseHandle(command); CloseHandle(ack); CloseHandle(owner);
    return result;
}
#endif
