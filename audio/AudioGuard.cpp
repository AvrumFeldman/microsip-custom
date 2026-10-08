// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone, microphone-free helper. No registry or endpoint volume changes.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
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

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

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
        // Volume-only adjustments do not give up our mute lease. An explicit
        // unmute does: respect the user's override for this session/call.
        if ((!context || *context != kMuteContext) && !mute) externalMuteChange = true;
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
        if (changed && events && !events->externalMuteChange) {
            // Already-muted sessions are never changed or unmuted by us.
            volume->SetMute(FALSE, &kMuteContext);
        }
        if (events) control->UnregisterAudioSessionNotification(events);
    }
};

class AudioGuard {
public:
    AudioGuard(DWORD parent, int mode, const std::wstring& apps)
        : parent_(parent), mode_(mode), apps_(ParseApps(apps)) {}

    void Scan() {
        CComPtr<IMMDeviceEnumerator> devices;
        if (FAILED(devices.CoCreateInstance(__uuidof(MMDeviceEnumerator)))) return;
        CComPtr<IMMDeviceCollection> outputs;
        if (FAILED(devices->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &outputs))) return;
        UINT count = 0;
        if (FAILED(outputs->GetCount(&count))) return;
        for (UINT i = 0; i < count; ++i) {
            CComPtr<IMMDevice> device;
            CComPtr<IAudioSessionManager2> manager;
            CComPtr<IAudioSessionEnumerator> sessions;
            if (FAILED(outputs->Item(i, &device)) ||
                FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                    reinterpret_cast<void**>(&manager))) ||
                FAILED(manager->GetSessionEnumerator(&sessions))) continue;
            int sessionCount = 0;
            if (FAILED(sessions->GetCount(&sessionCount))) continue;
            for (int j = 0; j < sessionCount; ++j) {
                CComPtr<IAudioSessionControl> control;
                if (FAILED(sessions->GetSession(j, &control))) continue;
                CComQIPtr<IAudioSessionControl2> extended(control);
                if (extended) Consider(extended);
            }
        }
        // Release expired sessions so long calls do not retain dead players.
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            AudioSessionState state;
            if (SUCCEEDED(it->second->control->GetState(&state)) && state == AudioSessionStateExpired)
                it = sessions_.erase(it);
            else ++it;
        }
    }

private:
    void Consider(IAudioSessionControl2* control) {
        DWORD pid = 0;
        if (FAILED(control->GetProcessId(&pid)) || !pid || pid == parent_ ||
            pid == GetCurrentProcessId() || control->IsSystemSoundsSession() == S_OK) return;
        if (mode_ == 2 && apps_.count(ProcessName(pid)) == 0) return;
        AudioSessionState state;
        if (FAILED(control->GetState(&state)) || state == AudioSessionStateExpired) return;
        LPWSTR rawId = nullptr;
        if (FAILED(control->GetSessionInstanceIdentifier(&rawId))) return;
        std::wstring id(rawId);
        CoTaskMemFree(rawId);
        if (sessions_.count(id)) return;
        CComQIPtr<ISimpleAudioVolume> volume(control);
        BOOL muted = FALSE;
        if (!volume || FAILED(volume->GetMute(&muted))) return;
        auto entry = std::make_unique<MutedSession>();
        entry->control = control;
        entry->volume = volume;
        if (!muted) {
            entry->events.Attach(new SessionEvents());
            if (FAILED(control->RegisterAudioSessionNotification(entry->events))) return;
            entry->changed = SUCCEEDED(volume->SetMute(TRUE, &kMuteContext));
            if (!entry->changed) return; // retry on the next scan
        }
        sessions_.emplace(id, std::move(entry));
    }
    DWORD parent_;
    int mode_;
    std::set<std::wstring> apps_;
    std::map<std::wstring, std::unique_ptr<MutedSession>> sessions_;
};

#ifndef AUDIO_GUARD_NO_MAIN
int wmain(int argc, wchar_t** argv)
{
    if (argc != 5) return 2;
    HANDLE lease = reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(_wcstoui64(argv[1], nullptr, 10)));
    const DWORD parent = wcstoul(argv[2], nullptr, 10);
    const int mode = _wtoi(argv[3]);
    if (!parent || (mode != 1 && mode != 2) || GetFileType(lease) != FILE_TYPE_PIPE) return 2;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 3;
    {
        AudioGuard guard(parent, mode, argv[4]);
        DWORD available;
        // EOF/broken pipe also happens if MicroSIP is killed or crashes.
        while (PeekNamedPipe(lease, nullptr, 0, nullptr, &available, nullptr)) {
            guard.Scan();
            Sleep(200);
        }
    } // restore sessions before COM is uninitialized
    CoUninitialize();
    CloseHandle(lease);
    return 0;
}
#endif
