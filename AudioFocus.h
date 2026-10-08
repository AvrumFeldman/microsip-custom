// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>
#include <string>

// UI-thread owner. The pipe's lifetime is the mute lease: closing it, including
// by process termination, tells the independent guard to restore other apps.
class AudioFocus {
public:
    ~AudioFocus();
    bool Update(bool active, int mode, const std::wstring& apps);
    void Stop();
private:
    bool Start(int mode, const std::wstring& apps);
    HANDLE pipe_ = nullptr;
    HANDLE process_ = nullptr;
    int mode_ = 0;
    std::wstring apps_;
};
