// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <string>

namespace AudioGuardProtocol { struct Shared; }

// Calls are serialized across the UI and PJSIP sound-device callback threads.
// The independent guard retains the mute lease until capture and Bluetooth have
// both released, including after this process has stopped.
class AudioFocus {
public:
    static constexpr unsigned long long AnyCaptureGeneration = ~0ULL;
    ~AudioFocus();
    bool PrepareCapture(const std::wstring& input, const std::wstring& output);
    unsigned long long CaptureGeneration() const;
    bool Update(bool active, int mode, const std::wstring& apps, bool captureReleased = true,
        unsigned long long expectedCaptureGeneration = AnyCaptureGeneration);
    bool IsRecovering() const;
    bool RecoveryUnknown() const;
    void RestoreNow();
    void Stop(); // caller must close capture first
private:
    bool Start();
    bool Send(int command, bool active, int mode, const std::wstring& apps,
        bool captureReleased, const std::wstring& input = {}, const std::wstring& output = {});
    void Close();
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    HANDLE mapping_ = nullptr;
    HANDLE command_ = nullptr;
    HANDLE acknowledgement_ = nullptr;
    HANDLE process_ = nullptr;
    AudioGuardProtocol::Shared* shared_ = nullptr;
    LONG sequence_ = 0;
    int mode_ = 0;
    std::wstring apps_;
    std::wstring input_ = L"<unobserved capture>";
    std::wstring output_ = L"<unobserved capture>";
    unsigned long long captureGeneration_ = 0;
};
