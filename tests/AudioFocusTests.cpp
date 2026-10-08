// SPDX-License-Identifier: GPL-2.0-or-later
// Real Windows audio sessions, silent render streams only. Never touches the
// user's audio: guard selection is restricted to this test executable.
#define AUDIO_FOCUS_STANDALONE
#include "../AudioFocus.cpp"
#define AUDIO_GUARD_NO_MAIN
#include "../audio/AudioGuard.cpp"
#include <audioclient.h>
#include <iostream>
#include <stdexcept>
#include <functional>

static void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
static void Hr(HRESULT hr, const char* message) { Check(SUCCEEDED(hr), message); }
static bool Eventually(const std::function<bool()>& predicate) {
    for (int i = 0; i < 60; ++i) { if (predicate()) return true; Sleep(50); }
    return false;
}

struct TestSession {
    CComPtr<IAudioClient> client;
    CComPtr<ISimpleAudioVolume> volume;
    TestSession(float level, bool mute) {
        CComPtr<IMMDeviceEnumerator> enumerator;
        Hr(enumerator.CoCreateInstance(__uuidof(MMDeviceEnumerator)), "device enumerator");
        CComPtr<IMMDevice> device;
        Hr(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device), "default render device");
        Hr(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client)), "audio client");
        WAVEFORMATEX* format = nullptr;
        Hr(client->GetMixFormat(&format), "mix format");
        GUID id;
        CoCreateGuid(&id);
        HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_NOPERSIST,
            1000000, 0, format, &id);
        CoTaskMemFree(format);
        Hr(hr, "silent test stream initialization");
        Hr(client->GetService(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&volume)), "session volume");
        Hr(volume->SetMasterVolume(level, nullptr), "initial test volume");
        Hr(volume->SetMute(mute, nullptr), "initial test mute");
    }
    bool Muted() { BOOL value; Hr(volume->GetMute(&value), "read mute"); return value != FALSE; }
    float Level() { float value; Hr(volume->GetMasterVolume(&value), "read volume"); return value; }
};

struct Controller {
    HANDLE stop = nullptr;
    HANDLE process = nullptr;
    Controller(const wchar_t* apps = L"AudioFocusTests.exe") {
        wchar_t path[32768];
        Check(GetModuleFileNameW(nullptr, path, _countof(path)) != 0, "module path");
        GUID id; CoCreateGuid(&id);
        wchar_t guid[40]; StringFromGUID2(id, guid, _countof(guid));
        std::wstring event = L"Local\\MicroSIPAudioTest-" + std::wstring(guid);
        stop = CreateEventW(nullptr, TRUE, FALSE, event.c_str());
        Check(stop != nullptr, "test stop event");
        std::wstring cmd = QuoteArgument(path) + L" --controller " + QuoteArgument(event) + L" " + QuoteArgument(apps);
        STARTUPINFOW startup = { sizeof(startup) };
        PROCESS_INFORMATION info = {};
        Check(CreateProcessW(path, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &info) != FALSE, "test controller launch");
        process = info.hProcess;
        CloseHandle(info.hThread);
    }
    void Finish(bool crash = false) {
        if (crash) Check(TerminateProcess(process, 99) != FALSE, "terminate test controller");
        else SetEvent(stop);
        Check(WaitForSingleObject(process, 5000) == WAIT_OBJECT_0, "controller shutdown");
    }
    ~Controller() {
        if (stop) SetEvent(stop);
        if (process) { WaitForSingleObject(process, 5000); CloseHandle(process); }
        if (stop) CloseHandle(stop);
    }
};

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring(argv[1]) == L"--controller") {
        HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, argv[2]);
        if (!stop) return 2;
        AudioFocus focus;
        while (WaitForSingleObject(stop, 50) == WAIT_TIMEOUT) {
            if (!focus.Update(true, 2, argv[3])) return 3;
        }
        focus.Stop();
        CloseHandle(stop);
        return 0;
    }
    Hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM init");
    int result = 0;
    try {
        Check(ParseApps(L" Spotify.EXE ; C:\\Tools\\vlc.exe, \"msedge.exe\" ") ==
            std::set<std::wstring>{L"spotify.exe", L"vlc.exe", L"msedge.exe"}, "app list parsing");
        TestSession a(0.37f, false);
        TestSession preMuted(0.61f, true);
        {
            Controller noMatch(L"not-a-real-music-app.exe");
            Sleep(600);
            Check(!a.Muted() && preMuted.Muted(), "selected-app exclusion");
            noMatch.Finish();
        }
        std::cout << "PASS: selected-app filtering and case/path parsing\n";
        {
            Controller controller;
            Check(Eventually([&] { return a.Muted(); }), "existing session was not muted");
            Check(preMuted.Muted(), "pre-muted session changed");
            Check(a.Level() == 0.37f, "volume changed while muting");
            TestSession createdDuringCall(0.23f, false);
            Check(Eventually([&] { return createdDuringCall.Muted(); }), "new session was not muted");
            createdDuringCall.volume->SetMasterVolume(0.42f, nullptr);
            TestSession manual(0.15f, false);
            Check(Eventually([&] { return manual.Muted(); }), "manual test setup");
            manual.volume->SetMute(FALSE, nullptr);
            Sleep(350);
            Check(!manual.Muted(), "user's unmute was overridden");
            manual.volume->SetMute(TRUE, nullptr);
            Sleep(100);
            controller.Finish();
            Check(Eventually([&] { return !a.Muted() && !createdDuringCall.Muted(); }), "normal restoration");
            Check(preMuted.Muted(), "pre-muted session was unmuted");
            Check(manual.Muted(), "user's later mute was overwritten");
            Check(createdDuringCall.Level() == 0.42f && a.Level() == 0.37f, "volume levels were overwritten");
        }
        std::cout << "PASS: mute, new sessions, normal restoration, pre-muted apps, volume changes, manual override\n";
        {
            Controller controller;
            Check(Eventually([&] { return a.Muted(); }), "crash test setup");
            controller.Finish(true);
            Check(Eventually([&] { return !a.Muted(); }), "restore after parent crash");
            Check(preMuted.Muted(), "crash recovery changed pre-muted app");
        }
        std::cout << "PASS: restoration after forced parent termination\n";
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n'; result = 1;
    }
    CoUninitialize();
    return result;
}
