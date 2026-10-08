// SPDX-License-Identifier: GPL-2.0-or-later
// Pure state tests: no microphone, Bluetooth adapter, or user's audio is opened.
#include "../audio/BluetoothRecovery.h"
#include <windows.h>
#include <iostream>
#include <stdexcept>

using BluetoothRecovery::DeviceBinding;
using BluetoothRecovery::DeviceKind;
using BluetoothRecovery::State;

static void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static constexpr unsigned long long headset = 0x123456789abcULL;
static constexpr unsigned long long otherHeadset = 0xabcdef012345ULL;
static DeviceBinding Wired() { return { DeviceKind::Wired, 0 }; }
static DeviceBinding Bluetooth(unsigned long long address = headset)
{
    return { DeviceKind::ClassicBluetooth, address };
}

static std::wstring AcpRoundTrip(const std::wstring& value)
{
    const int bytes = WideCharToMultiByte(CP_ACP, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    Check(bytes > 0, "ACP fixture size");
    std::string encoded(bytes, '\0');
    Check(WideCharToMultiByte(CP_ACP, 0, value.c_str(), -1, &encoded[0], bytes, nullptr, nullptr) != 0,
        "ACP fixture conversion");
    const int chars = MultiByteToWideChar(CP_ACP, 0, encoded.c_str(), -1, nullptr, 0);
    Check(chars > 0, "ACP fixture reverse size");
    std::wstring decoded(chars, L'\0');
    Check(MultiByteToWideChar(CP_ACP, 0, encoded.c_str(), -1, &decoded[0], chars) != 0,
        "ACP fixture reverse conversion");
    decoded.resize(chars - 1);
    return decoded;
}

int main()
{
    try {
        const std::wstring friendlyName = L"Headset (\u05d0\u05d5\u05d3\u05d9\u05d5 \U0001f3a7)";
        Check(BluetoothRecovery::DeviceNameMatches(friendlyName, friendlyName), "exact Unicode device name");
        Check(BluetoothRecovery::DeviceNameMatches(friendlyName, AcpRoundTrip(friendlyName)),
            "WinMM ACP device name must resolve on both legacy and UTF-8 system locales");
        Check(!BluetoothRecovery::DeviceNameMatches(friendlyName, L"Different headset"),
            "unrelated device name must not match");
        Check(!BluetoothRecovery::DeviceNameMatches(friendlyName, L"Headset"),
            "partial friendly name must not select an arbitrary headset");
        std::cout << "PASS: Unicode and Windows ACP device names resolve without unrelated or partial matches\n";

        State state;
        Check(state.Ready(), "idle and calls cancelled before capture need no recovery");
        state.CaptureClosed();
        Check(state.Ready(), "closing an unopened capture must remain ready");

        state.Arm({ Wired() });
        Check(!state.Ready(), "wired capture still open");
        state.SetProviderAvailable(false);
        state.CaptureClosed();
        Check(state.Ready(), "wired capture closure must not depend on a Bluetooth radio");
        std::cout << "PASS: idle, cancellation before capture, and wired capture closure\n";

        state.Reset();
        state.SetProviderAvailable(true);
        state.Arm({ Bluetooth() });
        Check(state.Unknown(), "unobserved Bluetooth transport state must be reported unknown");
        state.ObserveSco(headset, false);
        Check(state.Unknown(), "disconnect from an earlier attempt cannot establish current transport state");
        Check(!state.Ready(), "transport closure cannot release an open microphone");
        state.CaptureClosed();
        Check(!state.Ready(), "capture closure must not legitimize a stale pre-close disconnect");
        state.ObserveSco(headset, false);
        Check(!state.Unknown(), "fresh disconnected transport is not unknown");
        Check(state.Ready(), "matching disconnect and closed microphone should release");
        state.ObserveSco(headset, false);
        Check(state.Ready(), "duplicate disconnect must be harmless");
        state.ObserveSco(headset, true);
        Check(!state.Ready(), "SCO reconnect must revoke readiness");
        Check(!state.Unknown(), "known active SCO is pending rather than unknown");
        state.ObserveSco(headset, false);
        Check(state.Ready(), "subsequent disconnect must restore readiness");
        state.Arm({ Bluetooth() });
        state.ObserveSco(headset, true);
        state.ObserveSco(headset, false);
        state.ObserveSco(headset, false);
        Check(!state.Unknown(), "connected then disconnected current transport has valid evidence");
        Check(!state.Ready(), "valid transport evidence still cannot release an open capture");
        state.CaptureClosed();
        Check(state.Ready(), "current connect/disconnect evidence and capture close should release");
        std::cout << "PASS: microphone and transport gate independently; duplicate and reconnect events\n";

        state.Arm({ Bluetooth() });
        state.CaptureClosed();
        Check(!state.Ready(), "new capture attempt must not inherit an old disconnect");
        state.ObserveSco(otherHeadset, false);
        Check(!state.Ready(), "unrelated headset disconnect must not release muting");
        state.ObserveSco(headset, true);
        Check(!state.Ready(), "connected headset must retain muting");
        state.ObserveSco(headset, false);
        Check(state.Ready(), "relevant headset disconnect should release muting");
        std::cout << "PASS: rapid new calls require fresh matching transport evidence\n";

        state.Arm({ Bluetooth(), Bluetooth(otherHeadset), Bluetooth() });
        state.CaptureClosed();
        state.ObserveSco(headset, false);
        Check(!state.Ready(), "one disconnected scoped headset cannot release another");
        state.ObserveSco(otherHeadset, false);
        Check(state.Ready(), "all scoped transports disconnected");
        state.Arm({ Wired() });
        state.CaptureClosed();
        Check(state.Ready(), "wired capture must retain valid disconnect evidence for unused earlier transports");
        state.Arm({ Bluetooth(otherHeadset) });
        state.CaptureClosed();
        Check(!state.Ready(), "reused Bluetooth transport must supply fresh evidence");
        state.ObserveSco(headset, false);
        Check(!state.Ready(), "unused earlier transport cannot substitute for reused transport evidence");
        state.ObserveSco(otherHeadset, false);
        Check(state.Ready(), "reused transport can supply fresh disconnect evidence");
        std::cout << "PASS: multiple headsets, duplicate bindings, and device changes\n";

        state.Reset();
        state.SetProviderAvailable(true);
        state.Arm({ Bluetooth() });
        state.CaptureClosed();
        state.ObserveSco(headset, false);
        Check(state.Ready(), "provider-loss setup");
        state.SetProviderAvailable(false);
        Check(!state.Ready(), "provider failure must invalidate stale evidence");
        Check(state.Unknown(), "provider failure must be reported unknown");
        state.SetProviderAvailable(true);
        Check(!state.Ready(), "provider recovery must not invent a disconnect");
        state.ObserveSco(headset, false);
        Check(state.Ready(), "fresh evidence after provider recovery");
        std::cout << "PASS: radio/provider failure cannot silently restore audio\n";

        state.Reset();
        state.SetProviderAvailable(true);
        state.Arm({ { DeviceKind::Unknown, 0 } });
        state.CaptureClosed();
        Check(state.Unknown(), "unresolved device identity must be reported unknown");
        state.ObserveSco(headset, false);
        Check(!state.Ready(), "unknown endpoint identity cannot use an arbitrary disconnect");
        state.Arm({ Wired() });
        state.CaptureClosed();
        Check(!state.Ready(), "later endpoint mapping cannot erase an unresolved earlier attempt");
        state.Reset();
        Check(state.Ready(), "explicit completed-cycle reset returns to idle");
        state.Arm({ Bluetooth(0) });
        state.CaptureClosed();
        state.ObserveSco(0, false);
        Check(!state.Ready(), "missing Bluetooth address cannot be treated as known transport");
        std::cout << "PASS: unknown endpoint and address fail closed until explicit reset\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
