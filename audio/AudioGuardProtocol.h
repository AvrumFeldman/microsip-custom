// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <windows.h>

namespace AudioGuardProtocol {
constexpr LONG Version = 1;
enum Command { Update = 1, PrepareCapture = 2 };
enum State { Idle = 0, Holding = 1, Recovering = 2, Unknown = 3 };
struct Shared {
    LONG version;
    volatile LONG ready;
    volatile LONG sequence;
    volatile LONG acknowledged;
    volatile LONG result;
    volatile LONG state;
    volatile LONG forceRestore;
    volatile LONG detached;
    volatile LONG unobservedCapture;
    LONG command;
    LONG active;
    LONG mode;
    LONG captureReleased;
    wchar_t input[512];
    wchar_t output[512];
    wchar_t apps[8192];
};
// Non-command diagnostics mapping: Local\MicroSIPAudioGuard.Status.<helper PID>.
struct Status {
    LONG version;
    volatile LONG state;
    volatile LONG captureReleased;
    volatile LONG observerAvailable;
    LONG parent;
};
}
