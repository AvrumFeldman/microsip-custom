// SPDX-License-Identifier: GPL-2.0-or-later
// Real Windows audio sessions, silent render streams only. Never touches the
// user's audio: guard selection is restricted to this test executable.
#define AUDIO_FOCUS_STANDALONE
#include "../AudioFocus.cpp"
#define AUDIO_GUARD_NO_MAIN
#include "../audio/AudioGuard.cpp"
#include "../audio/BluetoothRecovery.h"
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

// Test-only IPC. It lives entirely in this executable; the production guard has
// no command or setting that can inject a fake Bluetooth disconnect.
enum class TestCommand : LONG {
    None, BeginBluetooth, CaptureClosed, RelevantDisconnect, OtherDisconnect,
    ProviderLost, ProviderRecovered, Stop, DisableMuting, PrepareUnknown,
    EnableOpenCapture, EndCapture, RestoreNow, EnableWithoutCapture,
    CaptureSnapshot, StaleCaptureClose
};
struct TestControl {
    volatile LONG command;
    volatile LONG sequence;
    volatile LONG acknowledged;
    volatile LONG holding;
    volatile LONG unknown;
    volatile LONG succeeded;
};
static constexpr unsigned long long testHeadset = 0x123456789abcULL;

static int RecoveryChild(const wchar_t* name)
{
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!mapping) return 2;
    auto control = static_cast<TestControl*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(TestControl)));
    if (!control) { CloseHandle(mapping); return 3; }
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 4;
    {
        // The child owns no render session. The only eligible sessions belong
        // to the parent AudioFocusTests executable, never another application.
        AudioGuard guard(GetCurrentProcessId(), 2, L"AudioFocusTests.exe");
        guard.Restore();
        BluetoothRecovery::State recovery;
        recovery.SetProviderAvailable(true);
        bool stop = false;
        while (!stop) {
            const LONG sequence = InterlockedCompareExchange(&control->sequence, 0, 0);
            if (sequence != InterlockedCompareExchange(&control->acknowledged, 0, 0)) {
                switch (static_cast<TestCommand>(InterlockedCompareExchange(&control->command, 0, 0))) {
                case TestCommand::BeginBluetooth:
                    recovery.Arm({ { BluetoothRecovery::DeviceKind::ClassicBluetooth, testHeadset } });
                    guard.BeginHold(2, L"AudioFocusTests.exe");
                    break;
                case TestCommand::CaptureClosed: recovery.CaptureClosed(); break;
                case TestCommand::RelevantDisconnect: recovery.ObserveSco(testHeadset, false); break;
                case TestCommand::OtherDisconnect: recovery.ObserveSco(testHeadset + 1, false); break;
                case TestCommand::ProviderLost: recovery.SetProviderAvailable(false); break;
                case TestCommand::ProviderRecovered: recovery.SetProviderAvailable(true); break;
                case TestCommand::Stop: stop = true; break;
                default: break;
                }
            }
            if (guard.IsHolding()) {
                if (recovery.Ready()) { guard.Restore(); recovery.Reset(); }
                else guard.Scan();
            }
            InterlockedExchange(&control->holding, guard.IsHolding() ? 1 : 0);
            InterlockedExchange(&control->succeeded, 1);
            InterlockedExchange(&control->acknowledged, sequence);
            Sleep(10);
        }
        guard.Restore(); // explicit fixture cleanup, including failed assertions
    }
    CoUninitialize();
    UnmapViewOfFile(control);
    CloseHandle(mapping);
    return 0;
}

// Exercise the real controller and separately built production helper. Only
// invented device names are supplied: this reports capture state without ever
// opening a microphone or manufacturing a Bluetooth provider event.
static int FocusChild(const wchar_t* name)
{
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!mapping) return 2;
    auto control = static_cast<TestControl*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(TestControl)));
    if (!control) { CloseHandle(mapping); return 3; }
    {
        AudioFocus focus;
        const std::wstring apps = L"AudioFocusTests.exe";
        unsigned long long captureSnapshot = 0;
        bool stop = false;
        while (!stop) {
            const LONG sequence = InterlockedCompareExchange(&control->sequence, 0, 0);
            if (sequence != InterlockedCompareExchange(&control->acknowledged, 0, 0)) {
                bool succeeded = true;
                switch (static_cast<TestCommand>(InterlockedCompareExchange(&control->command, 0, 0))) {
                case TestCommand::DisableMuting: succeeded = focus.Update(true, 0, apps, false); break;
                case TestCommand::PrepareUnknown:
                    succeeded = focus.PrepareCapture(L"MicroSIP test nonexistent microphone 07ef976f",
                        L"MicroSIP test nonexistent speaker 07ef976f");
                    break;
                case TestCommand::EnableOpenCapture: succeeded = focus.Update(true, 2, apps, false); break;
                case TestCommand::EnableWithoutCapture: succeeded = focus.Update(true, 2, apps, true); break;
                case TestCommand::EndCapture: succeeded = focus.Update(false, 2, apps, true); break;
                case TestCommand::CaptureSnapshot: captureSnapshot = focus.CaptureGeneration(); break;
                case TestCommand::StaleCaptureClose:
                    succeeded = focus.Update(false, 2, apps, true, captureSnapshot);
                    break;
                case TestCommand::RestoreNow: focus.RestoreNow(); break;
                case TestCommand::Stop: stop = true; break;
                default: succeeded = false; break;
                }
                InterlockedExchange(&control->succeeded, succeeded ? 1 : 0);
            }
            InterlockedExchange(&control->holding, focus.IsRecovering() ? 1 : 0);
            InterlockedExchange(&control->unknown, focus.RecoveryUnknown() ? 1 : 0);
            InterlockedExchange(&control->acknowledged, sequence);
            Sleep(10);
        }
        focus.RestoreNow(); // cleanup only this test's synthetic unresolved state
        focus.Stop();
    }
    UnmapViewOfFile(control);
    CloseHandle(mapping);
    return 0;
}

struct RecoveryController {
    HANDLE mapping = nullptr;
    HANDLE process = nullptr;
    TestControl* control = nullptr;
    explicit RecoveryController(bool production = false) {
        GUID id; CoCreateGuid(&id);
        wchar_t guid[40]; StringFromGUID2(id, guid, _countof(guid));
        std::wstring name = L"Local\\MicroSIPRecoveryTest-" + std::wstring(guid);
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(TestControl), name.c_str());
        Check(mapping != nullptr, "recovery test mapping");
        control = static_cast<TestControl*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(TestControl)));
        Check(control != nullptr, "recovery test mapping view");
        ZeroMemory(control, sizeof(*control));
        wchar_t path[32768];
        Check(GetModuleFileNameW(nullptr, path, _countof(path)) != 0, "recovery test executable path");
        std::wstring command = QuoteArgument(path) +
            (production ? L" --focus-child " : L" --recovery-child ") + QuoteArgument(name);
        STARTUPINFOW startup = { sizeof(startup) };
        PROCESS_INFORMATION info = {};
        Check(CreateProcessW(path, &command[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &info) != FALSE, "recovery test child launch");
        process = info.hProcess;
        CloseHandle(info.hThread);
        Send(production ? TestCommand::DisableMuting : TestCommand::BeginBluetooth);
    }
    void Send(TestCommand command) {
        InterlockedExchange(&control->command, static_cast<LONG>(command));
        const LONG sequence = InterlockedIncrement(&control->sequence);
        Check(Eventually([&] {
            return InterlockedCompareExchange(&control->acknowledged, 0, 0) == sequence;
        }), "recovery test command acknowledgment");
        Check(InterlockedCompareExchange(&control->succeeded, 0, 0) != 0, "recovery test command failed");
    }
    bool Holding() { return InterlockedCompareExchange(&control->holding, 0, 0) != 0; }
    bool Unknown() { return InterlockedCompareExchange(&control->unknown, 0, 0) != 0; }
    ~RecoveryController() {
        if (process) {
            InterlockedExchange(&control->command, static_cast<LONG>(TestCommand::Stop));
            InterlockedIncrement(&control->sequence);
            WaitForSingleObject(process, 5000);
            CloseHandle(process);
        }
        if (control) UnmapViewOfFile(control);
        if (mapping) CloseHandle(mapping);
    }
};

static void RemainsMuted(TestSession& session, DWORD milliseconds)
{
    const ULONGLONG until = GetTickCount64() + milliseconds;
    do {
        Check(session.Muted(), "audio restored before recovery state was ready");
        Sleep(20);
    } while (GetTickCount64() < until);
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring(argv[1]) == L"--recovery-child")
        return RecoveryChild(argv[2]);
    if (argc == 3 && std::wstring(argv[1]) == L"--focus-child")
        return FocusChild(argv[2]);
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
            TestSession unchanged(0.29f, false);
            TestSession alreadyMuted(0.59f, true);
            TestSession previouslyMutedThenUnmuted(0.53f, true);
            TestSession userOverride(0.19f, false);
            RecoveryController controller;
            Check(Eventually([&] { return unchanged.Muted() && userOverride.Muted(); }), "Bluetooth recovery setup");
            controller.Send(TestCommand::CaptureClosed);
            Check(controller.Holding(), "capture closure released mute before Bluetooth recovery");
            controller.Send(TestCommand::OtherDisconnect);
            RemainsMuted(unchanged, 1000);
            TestSession startedDuringRecovery(0.31f, false);
            TestSession mutedDuringRecovery(0.57f, true);
            Check(Eventually([&] { return startedDuringRecovery.Muted(); }), "new session escaped recovery muting");
            Hr(startedDuringRecovery.volume->SetMasterVolume(0.47f, nullptr), "recovery volume adjustment");
            Hr(alreadyMuted.volume->SetMasterVolume(0.73f, nullptr), "pre-muted volume adjustment");
            Hr(previouslyMutedThenUnmuted.volume->SetMute(FALSE, nullptr), "pre-muted user's unmute");
            Hr(userOverride.volume->SetMute(FALSE, nullptr), "recovery manual unmute");
            Sleep(350);
            Check(!userOverride.Muted(), "recovery overrode user's unmute");
            Check(!previouslyMutedThenUnmuted.Muted(), "recovery reclaimed an originally muted session after user unmute");
            Hr(userOverride.volume->SetMute(TRUE, nullptr), "recovery manual remute");
            Sleep(150);
            controller.Send(TestCommand::RelevantDisconnect);
            Check(!controller.Holding(), "matching transport recovery did not end hold");
            Check(Eventually([&] { return !unchanged.Muted() && !startedDuringRecovery.Muted(); }), "recovery did not restore owned mutes");
            Check(alreadyMuted.Muted(), "recovery unmuted an originally muted session");
            Check(mutedDuringRecovery.Muted(), "recovery unmuted a new session that was already muted");
            Check(!previouslyMutedThenUnmuted.Muted(), "restoration undid user's unmute of an originally muted session");
            Check(userOverride.Muted(), "recovery overrode user's later mute");
            Check(unchanged.Level() == 0.29f && startedDuringRecovery.Level() == 0.47f &&
                alreadyMuted.Level() == 0.73f, "recovery changed session volume levels");
        }
        std::cout << "PASS: state-driven recovery preserves pre-muted apps, manual changes and volumes; new sessions stay muted\n";
        {
            TestSession held(0.33f, false);
            RecoveryController controller;
            Check(Eventually([&] { return held.Muted(); }), "rapid-call setup");
            controller.Send(TestCommand::CaptureClosed);
            controller.Send(TestCommand::BeginBluetooth);
            controller.Send(TestCommand::RelevantDisconnect);
            Check(controller.Holding(), "disconnect during a new open capture restored audio");
            RemainsMuted(held, 300);
            controller.Send(TestCommand::CaptureClosed);
            Check(controller.Holding(), "new call accepted a stale pre-close disconnect");
            controller.Send(TestCommand::RelevantDisconnect);
            Check(Eventually([&] { return !held.Muted(); }), "new call completion did not restore audio");
            controller.Send(TestCommand::BeginBluetooth);
            Check(Eventually([&] { return held.Muted(); }), "new independent call did not acquire a mute");
            controller.Send(TestCommand::CaptureClosed);
            controller.Send(TestCommand::ProviderLost);
            controller.Send(TestCommand::RelevantDisconnect);
            Check(controller.Holding(), "unavailable Bluetooth provider restored audio");
            controller.Send(TestCommand::ProviderRecovered);
            Check(controller.Holding(), "provider reappearance reused stale transport evidence");
            controller.Send(TestCommand::RelevantDisconnect);
            Check(Eventually([&] { return !held.Muted(); }), "fresh disconnect after provider recovery did not restore audio");
        }
        std::cout << "PASS: rapid new calls and Bluetooth provider failures retain mute ownership\n";
        {
            TestSession owned(0.27f, false);
            TestSession originalMute(0.67f, true);
            RecoveryController first;
            Check(Eventually([&] { return owned.Muted(); }), "overlapping guards setup");
            RecoveryController second;
            first.Send(TestCommand::CaptureClosed);
            first.Send(TestCommand::RelevantDisconnect);
            Check(first.Holding(), "original mute owner ignored another guard's active call");
            RemainsMuted(owned, 300);
            second.Send(TestCommand::CaptureClosed);
            second.Send(TestCommand::RelevantDisconnect);
            Check(Eventually([&] { return !owned.Muted() && !first.Holding() && !second.Holding(); }),
                "overlapping guards did not restore after both recovered");
            Check(originalMute.Muted(), "overlapping guards unmuted an originally muted session");

            first.Send(TestCommand::BeginBluetooth);
            Check(Eventually([&] { return owned.Muted(); }), "reverse guard order setup");
            second.Send(TestCommand::BeginBluetooth);
            second.Send(TestCommand::CaptureClosed);
            second.Send(TestCommand::RelevantDisconnect);
            RemainsMuted(owned, 300);
            first.Send(TestCommand::CaptureClosed);
            first.Send(TestCommand::RelevantDisconnect);
            Check(Eventually([&] { return !owned.Muted() && !first.Holding() && !second.Holding(); }),
                "reversed guard completion order lost original mute ownership");
            Check(originalMute.Muted(), "reversed guard order changed an original mute");
        }
        std::cout << "PASS: overlapping guard processes preserve mute ownership across both completion orders\n";
        {
            TestSession owned(0.36f, false);
            TestSession originalMute(0.66f, true);
            RecoveryController controller(true);
            controller.Send(TestCommand::PrepareUnknown);
            Check(!owned.Muted(), "disabled audio muting changed a test session");
            controller.Send(TestCommand::EnableOpenCapture);
            Check(Eventually([&] { return owned.Muted(); }), "enabling muting during capture did not mute");
            controller.Send(TestCommand::EndCapture);
            Check(Eventually([&] { return controller.Holding() && controller.Unknown(); }),
                "mid-capture enabling failed to retain the unresolved device scope");
            RemainsMuted(owned, 300);
            controller.Send(TestCommand::RestoreNow);
            Check(Eventually([&] { return !owned.Muted() && !controller.Holding(); }),
                "explicit restoration did not release synthetic unknown recovery");
            Check(originalMute.Muted(), "explicit restoration changed an original mute");
        }
        std::cout << "PASS: production controller retains unknown capture scope when muting is enabled mid-call\n";
        {
            TestSession owned(0.34f, false);
            RecoveryController controller(true);
            controller.Send(TestCommand::EnableWithoutCapture);
            Check(Eventually([&] { return owned.Muted(); }), "capture generation setup");
            controller.Send(TestCommand::CaptureSnapshot);
            controller.Send(TestCommand::PrepareUnknown);
            controller.Send(TestCommand::StaleCaptureClose);
            RemainsMuted(owned, 300);
            Check(!controller.Holding() && !controller.Unknown(),
                "stale capture-close snapshot overwrote a newer active capture generation");
            controller.Send(TestCommand::EndCapture);
            Check(Eventually([&] { return controller.Holding() && controller.Unknown(); }),
                "fresh capture-close snapshot did not enter recovery");
            controller.Send(TestCommand::RestoreNow);
            Check(Eventually([&] { return !owned.Muted(); }), "capture generation test restoration");
        }
        std::cout << "PASS: stale capture snapshots cannot close a newer capture generation\n";
        {
            TestSession owned(0.38f, false);
            RecoveryController disabled(true);
            disabled.Send(TestCommand::EnableWithoutCapture);
            Check(Eventually([&] { return owned.Muted(); }), "disable transition setup");
            disabled.Send(TestCommand::DisableMuting);
            Check(Eventually([&] { return !owned.Muted(); }), "disabling muting did not restore its lease");
            RecoveryController active(true);
            active.Send(TestCommand::EnableWithoutCapture);
            Check(Eventually([&] { return owned.Muted(); }), "separate active guard setup");
            for (int i = 0; i < 4; ++i) {
                disabled.Send(TestCommand::DisableMuting);
                RemainsMuted(owned, 120);
            }
            active.Send(TestCommand::EndCapture);
            Check(Eventually([&] { return !owned.Muted(); }), "active guard did not restore its own lease");
        }
        std::cout << "PASS: repeated disabled updates do not override a separate active guard\n";
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
