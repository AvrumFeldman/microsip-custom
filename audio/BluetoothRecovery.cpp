// SPDX-License-Identifier: GPL-2.0-or-later
#include "BluetoothRecovery.h"
#include <winsock2.h>
#include <windows.h>
#include <initguid.h>
#include <bluetoothapis.h>
#include <bthdef.h>
#include <dbt.h>
#include <mmsystem.h>
#include <mmddk.h>
#include <mmdeviceapi.h>
#include <devicetopology.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <atlbase.h>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <set>
#include <thread>

namespace BluetoothRecovery {

bool DeviceNameMatches(const std::wstring& actual, const std::wstring& selected)
{
    if (actual == selected) return true;
    if (actual.empty() || actual.size() > 32767) return false;
    // Mirror pj_unicode_to_ansi rather than guessing which substituted glyphs
    // refer to a device. ResolveDevice collects all matching endpoint IDs and
    // rejects collisions caused by this inherently lossy legacy conversion.
    int bytes = WideCharToMultiByte(CP_ACP, 0, actual.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (!bytes || bytes > 131072) return false;
    std::vector<char> encoded(bytes);
    if (!WideCharToMultiByte(CP_ACP, 0, actual.c_str(), -1, encoded.data(), bytes, nullptr, nullptr)) return false;
    int chars = MultiByteToWideChar(CP_ACP, 0, encoded.data(), -1, nullptr, 0);
    if (!chars || chars > 32768) return false;
    std::vector<wchar_t> alias(chars);
    if (!MultiByteToWideChar(CP_ACP, 0, encoded.data(), -1, alias.data(), chars)) return false;
    return selected == alias.data();
}

void State::Arm(const std::vector<DeviceBinding>& devices)
{
    attempted_ = true;
    captureClosed_ = false;
    // A fresh attempt cannot reuse old release evidence for devices it uses.
    // Other pending devices remain scoped, but an already released old headset
    // need not emit another disconnect when a new call uses a different device.
    for (const auto& device : devices) {
        if (device.kind == DeviceKind::Unknown) unresolved_ = true;
        if (device.kind == DeviceKind::ClassicBluetooth) {
            if (device.address) links_[device.address] = Link::Unknown;
            else unresolved_ = true;
        }
    }
    if (devices.empty()) unresolved_ = true;
}

void State::CaptureClosed() { captureClosed_ = true; }

void State::SetProviderAvailable(bool available)
{
    if (!available) for (auto& link : links_) link.second = Link::Unknown;
    providerAvailable_ = available;
}

void State::ObserveSco(unsigned long long address, bool connected)
{
    if (!providerAvailable_) return;
    auto link = links_.find(address);
    if (link == links_.end()) return;
    if (connected) link->second = Link::Active;
    else if (captureClosed_ || link->second == Link::Active || link->second == Link::Down)
        link->second = Link::Down;
    // Arm precedes the physical open. An old queued disconnect arriving in
    // that gap cannot prove the new attempt released. Before capture closure,
    // require a connect in this attempt; duplicate proven-down events are OK.
    // Windows does not attach application transaction IDs to these events, so
    // missing/ambiguous evidence remains unknown rather than timing out safe.
}

bool State::Unknown() const
{
    if (!attempted_) return false;
    if (unresolved_ || (!links_.empty() && !providerAvailable_)) return true;
    for (const auto& link : links_) if (link.second == Link::Unknown) return true;
    return false;
}

bool State::Ready() const
{
    if (!attempted_) return true;
    if (!captureClosed_ || Unknown()) return false;
    for (const auto& link : links_) if (link.second != Link::Down) return false;
    return true;
}

void State::Reset()
{
    attempted_ = false;
    captureClosed_ = true;
    unresolved_ = false;
    links_.clear();
}

namespace {

// DEVPKEY_Bluetooth_DeviceAddress from the Windows SDK's bthguid.h. Defining the
// property here avoids requiring a kernel-driver include path for this desktop app.
const DEVPROPKEY BluetoothAddressProperty = {
    { 0x2bd67d8b, 0x8beb, 0x48d5, { 0x87, 0xe0, 0x6c, 0xda, 0x34, 0x28, 0x04, 0x0a } }, 1
};

bool HexAddress(const std::wstring& value, unsigned long long& address)
{
    if (value.size() != 12) return false;
    unsigned long long result = 0;
    for (wchar_t ch : value) {
        unsigned digit;
        if (ch >= L'0' && ch <= L'9') digit = ch - L'0';
        else if (ch >= L'a' && ch <= L'f') digit = ch - L'a' + 10;
        else if (ch >= L'A' && ch <= L'F') digit = ch - L'A' + 10;
        else return false;
        result = (result << 4) | digit;
    }
    if (!result) return false;
    address = result;
    return true;
}

bool Starts(const std::wstring& text, const wchar_t* prefix)
{
    return text.compare(0, wcslen(prefix), prefix) == 0;
}

bool DevicePropertyString(DEVINST node, const DEVPROPKEY& key, std::wstring& result)
{
    DEVPROPTYPE type = 0;
    ULONG bytes = 0;
    if (CM_Get_DevNode_PropertyW(node, &key, &type, nullptr, &bytes, 0) != CR_BUFFER_SMALL ||
        type != DEVPROP_TYPE_STRING || bytes < sizeof(wchar_t) || bytes > 65536 || bytes % sizeof(wchar_t)) return false;
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t), 0);
    if (CM_Get_DevNode_PropertyW(node, &key, &type, reinterpret_cast<PBYTE>(buffer.data()), &bytes, 0) != CR_SUCCESS ||
        type != DEVPROP_TYPE_STRING || buffer.back() != 0) return false;
    result.assign(buffer.data());
    return true;
}

DeviceBinding ClassifyInstance(const std::wstring& instance, bool capture)
{
    DEVINST node;
    if (CM_Locate_DevNodeW(&node, const_cast<wchar_t*>(instance.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
        return { DeviceKind::Unknown, 0 };
    bool physicalWired = false, bluetooth = false, lowEnergy = false;
    bool handsFree = false, stereoSink = false;
    unsigned long long address = 0;
    for (unsigned depth = 0; depth < 32; ++depth) {
        wchar_t id[MAX_DEVICE_ID_LEN]{};
        if (CM_Get_Device_IDW(node, id, _countof(id), 0) != CR_SUCCESS) return { DeviceKind::Unknown, 0 };
        std::wstring value(id);
        std::transform(value.begin(), value.end(), value.begin(), towupper);
        bluetooth = bluetooth || Starts(value, L"BTH");
        lowEnergy = lowEnergy || Starts(value, L"BTHLE");
        handsFree = handsFree || Starts(value, L"BTHHFENUM\\");
        stereoSink = stereoSink || Starts(value, L"BTHENUM\\{0000110B-");
        if (Starts(value, L"BTHENUM\\DEV_")) {
            auto end = value.find(L'\\', 12);
            HexAddress(value.substr(12, end == std::wstring::npos ? end : end - 12), address);
        }
        std::wstring property;
        unsigned long long propertyAddress = 0;
        if (DevicePropertyString(node, BluetoothAddressProperty, property) && HexAddress(property, propertyAddress))
            address = propertyAddress;
        // These are bus/instance identifiers, never user-visible name guesses.
        physicalWired = physicalWired || Starts(value, L"HDAUDIO\\") || Starts(value, L"INTELAUDIO\\") ||
            Starts(value, L"USB\\") || Starts(value, L"PCI\\");
        DEVINST parent;
        CONFIGRET result = CM_Get_Parent(&parent, node, 0);
        if (result == CR_NO_SUCH_DEVNODE) break;
        if (result != CR_SUCCESS || parent == node) return { DeviceKind::Unknown, 0 };
        node = parent;
    }
    if (lowEnergy) return { DeviceKind::Unknown, 0 }; // LE Audio is not Classic SCO.
    // WinMM playback alone does not request the Communications category. A
    // verified A2DP-only output therefore cannot open SCO; the capture endpoint
    // or a distinct hands-free render endpoint determines the SCO scope.
    if (bluetooth && !capture && stereoSink && !handsFree) return {DeviceKind::Wired, 0};
    if (bluetooth) return address ? DeviceBinding{DeviceKind::ClassicBluetooth, address} : DeviceBinding{DeviceKind::Unknown, 0};
    return { physicalWired ? DeviceKind::Wired : DeviceKind::Unknown, 0 };
}

DeviceBinding ClassifyEndpoint(IMMDeviceEnumerator* enumerator, IMMDevice* endpoint, bool capture)
{
    // Endpoint software devnodes can be rooted under SWD rather than their audio
    // adapter. Follow Core Audio's connector to its adapter before walking PnP.
    CComPtr<IDeviceTopology> topology;
    if (SUCCEEDED(endpoint->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&topology)))) {
        UINT count = 0;
        if (SUCCEEDED(topology->GetConnectorCount(&count)) && count <= 32) {
            DeviceBinding binding{DeviceKind::Unknown, 0};
            bool found = false;
            for (UINT index = 0; index < count; ++index) {
                CComPtr<IConnector> connector;
                LPWSTR id = nullptr;
                if (FAILED(topology->GetConnector(index, &connector)) || FAILED(connector->GetDeviceIdConnectedTo(&id))) continue;
                CComPtr<IMMDevice> adapter;
                HRESULT hr = enumerator->GetDevice(id, &adapter);
                CoTaskMemFree(id);
                if (FAILED(hr)) continue;
                CComPtr<IPropertyStore> properties;
                PROPVARIANT property;
                PropVariantInit(&property);
                if (SUCCEEDED(adapter->OpenPropertyStore(STGM_READ, &properties)) &&
                    SUCCEEDED(properties->GetValue(PKEY_Device_InstanceId, &property)) && property.vt == VT_LPWSTR && property.pwszVal) {
                    DeviceBinding current = ClassifyInstance(property.pwszVal, capture);
                    PropVariantClear(&property);
                    if (current.kind == DeviceKind::Unknown) return current;
                    if (found && (current.kind != binding.kind || current.address != binding.address)) return {DeviceKind::Unknown, 0};
                    binding = current;
                    found = true;
                } else PropVariantClear(&property);
            }
            if (found) return binding;
        }
    }
    CComPtr<IPropertyStore> properties;
    PROPVARIANT property;
    PropVariantInit(&property);
    DeviceBinding result{DeviceKind::Unknown, 0};
    if (SUCCEEDED(endpoint->OpenPropertyStore(STGM_READ, &properties)) &&
        SUCCEEDED(properties->GetValue(PKEY_Device_InstanceId, &property)) && property.vt == VT_LPWSTR && property.pwszVal)
        result = ClassifyInstance(property.pwszVal, capture);
    PropVariantClear(&property);
    return result;
}

MMRESULT WaveMessage(bool capture, UINT_PTR index, UINT message, DWORD_PTR first, DWORD_PTR second)
{
    return capture ? waveInMessage(reinterpret_cast<HWAVEIN>(index), message, first, second) :
        waveOutMessage(reinterpret_cast<HWAVEOUT>(index), message, first, second);
}

bool WaveEndpoint(bool capture, UINT_PTR index, std::wstring& endpoint)
{
    ULONG bytes = 0;
    if (WaveMessage(capture, index, DRV_QUERYFUNCTIONINSTANCEIDSIZE, reinterpret_cast<DWORD_PTR>(&bytes), 0) != MMSYSERR_NOERROR ||
        bytes < sizeof(wchar_t) || bytes > 65536 || bytes % sizeof(wchar_t)) return false;
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t), 0);
    if (WaveMessage(capture, index, DRV_QUERYFUNCTIONINSTANCEID, reinterpret_cast<DWORD_PTR>(buffer.data()), bytes) != MMSYSERR_NOERROR ||
        buffer.back() != 0) return false;
    endpoint.assign(buffer.data());
    return !endpoint.empty();
}

DeviceBinding ResolveDevice(IMMDeviceEnumerator* enumerator, bool capture, const std::wstring& name)
{
    UINT count = capture ? waveInGetNumDevs() : waveOutGetNumDevs();
    if (!count || count > 4096) return {DeviceKind::Unknown, 0};
    std::set<std::wstring> matches;
    if (name.empty() || name == L"Wave mapper") {
        DWORD preferred = 0, flags = 0;
        if (WaveMessage(capture, static_cast<UINT_PTR>(WAVE_MAPPER), DRVM_MAPPER_PREFERRED_GET,
            reinterpret_cast<DWORD_PTR>(&preferred), reinterpret_cast<DWORD_PTR>(&flags)) != MMSYSERR_NOERROR || preferred >= count)
            return {DeviceKind::Unknown, 0};
        std::wstring id;
        if (!WaveEndpoint(capture, preferred, id)) return {DeviceKind::Unknown, 0};
        matches.insert(id);
    } else for (UINT index = 0; index < count; ++index) {
        std::wstring id;
        if (!WaveEndpoint(capture, index, id)) continue;
        bool matchesName = false;
        if (capture) {
            WAVEINCAPSW caps{};
            matchesName = waveInGetDevCapsW(index, &caps, sizeof(caps)) == MMSYSERR_NOERROR && DeviceNameMatches(caps.szPname, name);
        } else {
            WAVEOUTCAPSW caps{};
            matchesName = waveOutGetDevCapsW(index, &caps, sizeof(caps)) == MMSYSERR_NOERROR && DeviceNameMatches(caps.szPname, name);
        }
        CComPtr<IMMDevice> endpoint;
        CComPtr<IPropertyStore> properties;
        PROPVARIANT property;
        PropVariantInit(&property);
        if (SUCCEEDED(enumerator->GetDevice(id.c_str(), &endpoint)) &&
            SUCCEEDED(endpoint->OpenPropertyStore(STGM_READ, &properties)) &&
            SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &property)) && property.vt == VT_LPWSTR && property.pwszVal)
            matchesName = matchesName || DeviceNameMatches(property.pwszVal, name);
        PropVariantClear(&property);
        if (matchesName) matches.insert(id);
    }
    if (matches.size() != 1) return {DeviceKind::Unknown, 0};
    CComPtr<IMMDevice> endpoint;
    if (FAILED(enumerator->GetDevice(matches.begin()->c_str(), &endpoint))) return {DeviceKind::Unknown, 0};
    return ClassifyEndpoint(enumerator, endpoint, capture);
}

} // namespace

struct Observer::Impl {
    struct Radio { HANDLE handle; HDEVNOTIFY notification; BTH_ADDR address; };
    mutable std::mutex mutex;
    State state;
    std::thread thread;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HWND window = nullptr;
    bool started = false;
    std::vector<Radio> radios;

    ~Impl()
    {
        if (thread.joinable()) {
            HWND target;
            { std::lock_guard<std::mutex> lock(mutex); target = window; }
            if (target) PostMessageW(target, WM_CLOSE, 0, 0);
            thread.join();
        }
        if (ready) CloseHandle(ready);
    }

    void RefreshRadios()
    {
        BLUETOOTH_FIND_RADIO_PARAMS params{sizeof(params)};
        HANDLE handle = nullptr;
        HBLUETOOTH_RADIO_FIND search = BluetoothFindFirstRadio(&params, &handle);
        std::set<BTH_ADDR> present;
        bool healthy = search != nullptr;
        if (search) {
            do {
                BLUETOOTH_RADIO_INFO info{sizeof(info)};
                if (BluetoothGetRadioInfo(handle, &info) != ERROR_SUCCESS) { CloseHandle(handle); healthy = false; continue; }
                present.insert(info.address.ullLong);
                auto found = std::find_if(radios.begin(), radios.end(), [&](const Radio& r) { return r.address == info.address.ullLong; });
                if (found != radios.end()) { CloseHandle(handle); continue; }
                DEV_BROADCAST_HANDLE filter{};
                filter.dbch_size = sizeof(filter);
                filter.dbch_devicetype = DBT_DEVTYP_HANDLE;
                filter.dbch_handle = handle;
                HDEVNOTIFY notification = RegisterDeviceNotificationW(window, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
                if (!notification) { CloseHandle(handle); healthy = false; continue; }
                radios.push_back({handle, notification, info.address.ullLong});
            } while (BluetoothFindNextRadio(search, &handle));
            if (GetLastError() != ERROR_NO_MORE_ITEMS) healthy = false;
            BluetoothFindRadioClose(search);
        }
        bool removed = false;
        for (auto it = radios.begin(); it != radios.end();) {
            if (!present.count(it->address)) {
                UnregisterDeviceNotification(it->notification);
                CloseHandle(it->handle);
                it = radios.erase(it);
                removed = true;
            } else ++it;
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (removed) state.SetProviderAvailable(false);
        state.SetProviderAvailable(healthy && !radios.empty());
    }

    void DeviceEvent(WPARAM kind, LPARAM parameter)
    {
        if (kind == DBT_DEVNODES_CHANGED) { RefreshRadios(); return; }
        if (!parameter) return;
        const auto* header = reinterpret_cast<const DEV_BROADCAST_HDR*>(parameter);
        if (header->dbch_size < offsetof(DEV_BROADCAST_HANDLE, dbch_data) || header->dbch_devicetype != DBT_DEVTYP_HANDLE) return;
        const auto* data = reinterpret_cast<const DEV_BROADCAST_HANDLE*>(header);
        auto source = std::find_if(radios.begin(), radios.end(), [&](const Radio& radio) { return radio.notification == data->dbch_hdevnotify; });
        if (source == radios.end()) return;
        if (kind == DBT_DEVICEREMOVECOMPLETE || kind == DBT_DEVICEQUERYREMOVE || kind == DBT_DEVICEREMOVEPENDING) {
            std::lock_guard<std::mutex> lock(mutex);
            state.SetProviderAvailable(false);
            return;
        }
        const size_t required = offsetof(BTH_HCI_EVENT_INFO, connected) + sizeof(UCHAR);
        if (kind != DBT_CUSTOMEVENT || !IsEqualGUID(data->dbch_eventguid, GUID_BLUETOOTH_HCI_EVENT) ||
            data->dbch_size - offsetof(DEV_BROADCAST_HANDLE, dbch_data) < required) return;
        BTH_HCI_EVENT_INFO event{};
        memcpy(&event, data->dbch_data, required);
        if (event.connectionType != HCI_CONNECTION_TYPE_SCO) return;
        std::lock_guard<std::mutex> lock(mutex);
        state.ObserveSco(event.bthAddress, event.connected != 0);
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        Impl* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == WM_DEVICECHANGE) { self->DeviceEvent(wparam, lparam); return TRUE; }
        if (self && message == WM_TIMER) { self->RefreshRadios(); return 0; }
        if (message == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
        if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    void Run()
    {
        WNDCLASSW klass{};
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpfnWndProc = WindowProc;
        klass.lpszClassName = L"MicroSIPBluetoothRecoveryObserver";
        bool registered = RegisterClassW(&klass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        HWND created = registered ? CreateWindowW(klass.lpszClassName, L"MicroSIP Bluetooth recovery", 0,
            0, 0, 0, 0, nullptr, nullptr, klass.hInstance, this) : nullptr;
        { std::lock_guard<std::mutex> lock(mutex); window = created; }
        if (created) {
            RefreshRadios();
            // Recheck provider presence only. Elapsed time never authorizes unmute.
            SetTimer(created, 1, 1000, nullptr);
        }
        { std::lock_guard<std::mutex> lock(mutex); started = created != nullptr; }
        SetEvent(ready);
        if (created) {
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        for (const auto& radio : radios) { UnregisterDeviceNotification(radio.notification); CloseHandle(radio.handle); }
        radios.clear();
        std::lock_guard<std::mutex> lock(mutex);
        state.SetProviderAvailable(false);
        window = nullptr;
        started = false;
    }
};

Observer::Observer() : impl_(new Impl) {}
Observer::~Observer() = default;

bool Observer::Start()
{
    if (!impl_->ready) return false;
    if (!impl_->thread.joinable()) impl_->thread = std::thread([this] { impl_->Run(); });
    WaitForSingleObject(impl_->ready, INFINITE);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->started;
}

void Observer::Arm(const std::wstring& captureName, const std::wstring& playbackName)
{
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<DeviceBinding> bindings(2, {DeviceKind::Unknown, 0});
    {
        CComPtr<IMMDeviceEnumerator> enumerator;
        if ((SUCCEEDED(initialized) || initialized == RPC_E_CHANGED_MODE) &&
            SUCCEEDED(enumerator.CoCreateInstance(__uuidof(MMDeviceEnumerator)))) {
            bindings[0] = ResolveDevice(enumerator, true, captureName);
            bindings[1] = ResolveDevice(enumerator, false, playbackName);
        }
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->state.Arm(bindings);
}

void Observer::CaptureClosed() { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->state.CaptureClosed(); }
bool Observer::Ready() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->state.Ready(); }
bool Observer::Unknown() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->state.Unknown(); }
void Observer::Reset() { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->state.Reset(); }

} // namespace BluetoothRecovery
