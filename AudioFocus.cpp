// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AUDIO_FOCUS_STANDALONE
#include "stdafx.h"
#endif
#include "AudioFocus.h"
#include "audio/AudioGuardProtocol.h"
#include <vector>

static std::wstring QuoteArgument(const std::wstring& value)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += ch;
    }
    result.append(slashes * 2, L'\\');
    return result + L"\"";
}

namespace {
struct FocusLock {
    SRWLOCK& lock;
    explicit FocusLock(SRWLOCK& value) : lock(value) { AcquireSRWLockExclusive(&lock); }
    ~FocusLock() { ReleaseSRWLockExclusive(&lock); }
};
}

AudioFocus::~AudioFocus() { Stop(); }

void AudioFocus::Close()
{
    if (shared_) { UnmapViewOfFile(shared_); shared_ = nullptr; }
    for (HANDLE* handle : { &mapping_, &command_, &acknowledgement_, &process_ }) {
        if (*handle) { CloseHandle(*handle); *handle = nullptr; }
    }
    sequence_ = 0;
}

void AudioFocus::Stop()
{
    FocusLock lock(lock_);
    if (shared_) {
        // Not an unmute command: Bluetooth must release after capture closes.
        InterlockedExchange(&shared_->detached, 1);
        SetEvent(command_);
    }
    Close(); // never terminate a guard while it is restoring audio
}

bool AudioFocus::IsRecovering() const
{
    FocusLock lock(lock_);
    if (!shared_) return false;
    const LONG state = InterlockedCompareExchange(&shared_->state, 0, 0);
    return state == AudioGuardProtocol::Recovering || state == AudioGuardProtocol::Unknown;
}

bool AudioFocus::RecoveryUnknown() const
{
    FocusLock lock(lock_);
    return shared_ && InterlockedCompareExchange(&shared_->state, 0, 0) == AudioGuardProtocol::Unknown;
}

void AudioFocus::RestoreNow()
{
    FocusLock lock(lock_);
    if (shared_) {
        InterlockedExchange(&shared_->forceRestore, 1);
        SetEvent(command_);
    }
}

bool AudioFocus::PrepareCapture(const std::wstring& input, const std::wstring& output)
{
    FocusLock lock(lock_);
    if (++captureGeneration_ == AnyCaptureGeneration) ++captureGeneration_;
    input_ = input;
    output_ = output;
    if (mode_ < 1 || mode_ > 2) return true;
    const bool prepared = Send(AudioGuardProtocol::PrepareCapture, true, mode_, apps_, false, input, output);
    if (!prepared && shared_) {
        // PJSIP does not honor a failure return from its device callback. If a
        // stream can open without the acknowledged observer, never treat that
        // attempt as "no Bluetooth scope" and restore on capture-close alone.
        InterlockedExchange(&shared_->unobservedCapture, 1);
        SetEvent(command_);
    }
    return prepared;
}

unsigned long long AudioFocus::CaptureGeneration() const
{
    FocusLock lock(lock_);
    return captureGeneration_;
}

bool AudioFocus::Update(bool active, int mode, const std::wstring& apps, bool captureReleased,
    unsigned long long expectedCaptureGeneration)
{
    FocusLock lock(lock_);
    const bool wasEnabled = mode_ == 1 || mode_ == 2;
    mode_ = mode;
    apps_ = apps;
    if (mode < 1 || mode > 2) {
        if (wasEnabled && shared_) { InterlockedExchange(&shared_->forceRestore, 1); SetEvent(command_); }
        return true;
    }
    // The UI samples PJSIP without holding this lock. A worker can prepare a
    // new stream between that sample and this update; an older "closed" sample
    // must never overwrite the newly armed capture. Preferences remain current,
    // and disabling muting above is an explicit user override in either case.
    if (expectedCaptureGeneration != AnyCaptureGeneration &&
        expectedCaptureGeneration != captureGeneration_) return true;
    return Send(AudioGuardProtocol::Update, active, mode, apps, captureReleased, input_, output_);
}

bool AudioFocus::Send(int command, bool active, int mode, const std::wstring& apps,
    bool captureReleased, const std::wstring& input, const std::wstring& output)
{
    if (process_ && WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) Close();
    if (!shared_ && !Start()) return false;
    if (input.size() >= _countof(shared_->input) || output.size() >= _countof(shared_->output) ||
        apps.size() >= _countof(shared_->apps)) return false;
    // A timed-out command may still be running. Never overwrite its payload.
    if (InterlockedCompareExchange(&shared_->acknowledged, 0, 0) != sequence_ ||
        !InterlockedCompareExchange(&shared_->ready, 0, 0)) return false;
    shared_->command = command;
    shared_->active = active;
    shared_->mode = mode;
    shared_->captureReleased = captureReleased;
    wcscpy_s(shared_->input, input.c_str());
    wcscpy_s(shared_->output, output.c_str());
    wcscpy_s(shared_->apps, apps.c_str());
    ResetEvent(acknowledgement_);
    InterlockedExchange(&shared_->sequence, ++sequence_);
    SetEvent(command_);
    HANDLE waits[] = { acknowledgement_, process_ };
    // Bounds a failed helper handshake; it never authorizes restoration.
    if (WaitForMultipleObjects(_countof(waits), waits, FALSE, 10000) != WAIT_OBJECT_0) return false;
    return InterlockedCompareExchange(&shared_->acknowledged, 0, 0) == sequence_ &&
        InterlockedCompareExchange(&shared_->result, 0, 0) != 0;
}

bool AudioFocus::Start()
{
    wchar_t module[32768];
    const DWORD length = GetModuleFileNameW(nullptr, module, _countof(module));
    if (!length || length >= _countof(module)) return false;
    std::wstring exe(module, length);
    exe.resize(exe.find_last_of(L"\\/") + 1);
    exe += L"MicroSIPAudioGuard.exe";
    SECURITY_ATTRIBUTES security = { sizeof(security), nullptr, TRUE };
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0,
        sizeof(AudioGuardProtocol::Shared), nullptr);
    command_ = CreateEventW(&security, FALSE, FALSE, nullptr);
    acknowledgement_ = CreateEventW(&security, TRUE, FALSE, nullptr);
    if (!mapping_ || !command_ || !acknowledgement_) { Close(); return false; }
    shared_ = static_cast<AudioGuardProtocol::Shared*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!shared_) { Close(); return false; }
    ZeroMemory(shared_, sizeof(*shared_));
    shared_->version = AudioGuardProtocol::Version;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<BYTE> storage(size);
    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size)) { Close(); return false; }
    BOOL created = FALSE;
    HANDLE inherited[] = { mapping_, command_, acknowledgement_ };
    if (UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        inherited, sizeof(inherited), nullptr, nullptr)) {
        std::wstring command = QuoteArgument(exe);
        for (HANDLE handle : inherited) command += L" " + std::to_wstring(reinterpret_cast<UINT_PTR>(handle));
        command += L" " + std::to_wstring(GetCurrentProcessId());
        PROCESS_INFORMATION process = {};
        created = CreateProcessW(exe.c_str(), &command[0], nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &process);
        if (created) { process_ = process.hProcess; CloseHandle(process.hThread); }
    }
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    for (HANDLE handle : inherited) SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
    if (!created) { Close(); return false; }
    HANDLE waits[] = { acknowledgement_, process_ };
    return WaitForMultipleObjects(_countof(waits), waits, FALSE, 10000) == WAIT_OBJECT_0 &&
        InterlockedCompareExchange(&shared_->ready, 0, 0) != 0;
}
