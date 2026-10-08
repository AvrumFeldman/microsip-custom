// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace BluetoothRecovery {

// Wired means a positively identified path that cannot request Windows SCO;
// this also includes A2DP-only render paths in the application's WinMM backend.
enum class DeviceKind { Unknown, Wired, ClassicBluetooth };
struct DeviceBinding {
    DeviceKind kind;
    unsigned long long address;
};

// PJSIP's WinMM backend exposes names through the Windows ANSI code page.
// Compare that alias as well as the original Unicode endpoint name. Callers
// must still reject names that identify more than one endpoint.
bool DeviceNameMatches(const std::wstring& actual, const std::wstring& selected);

// Pure state model, also exercised without opening any Windows audio device.
// Arm is an explicit attempt to open capture, not merely a ringing/call state.
class State {
public:
    void Arm(const std::vector<DeviceBinding>& devices);
    void CaptureClosed();
    void SetProviderAvailable(bool available);
    void ObserveSco(unsigned long long address, bool connected);
    bool Ready() const;
    bool Unknown() const;
    void Reset();
private:
    enum class Link { Unknown, Active, Down };
    bool attempted_ = false;
    bool captureClosed_ = true;
    bool providerAvailable_ = false;
    bool unresolved_ = false;
    std::map<unsigned long long, Link> links_;
};

// Passive observer: no inquiry, audio stream, microphone data or profile change.
// Start/Arm finish before the application attempts to open its capture device.
// Names are PJSIP's WMME device names (full endpoint or legacy caps name).
// Empty / "Wave mapper" resolves the actual WinMM mapper preference.
class Observer {
public:
    Observer();
    ~Observer();
    bool Start();
    void Arm(const std::wstring& captureName, const std::wstring& playbackName);
    void CaptureClosed();
    bool Ready() const;
    bool Unknown() const;
    void Reset();
    Observer(const Observer&) = delete;
    Observer& operator=(const Observer&) = delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace BluetoothRecovery
