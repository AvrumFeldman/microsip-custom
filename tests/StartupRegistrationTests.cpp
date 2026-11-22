// SPDX-License-Identifier: GPL-2.0-or-later
#include "../StartupRegistration.h"
#include <iostream>
#include <stdexcept>

namespace {
void Expect(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct TestKey {
    HKEY key = nullptr;
    std::wstring path;
    TestKey()
    {
        path = L"Software\\MicroSIPStartupTests-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64());
        DWORD disposition = 0;
        Expect(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
            KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, &disposition) == ERROR_SUCCESS &&
            disposition == REG_CREATED_NEW_KEY, "create isolated registry test key");
    }
    ~TestKey()
    {
        if (key) {
            RegCloseKey(key);
            RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str());
        }
    }
};
}

int main()
{
    try {
        const std::wstring custom = L"C:\\Tools\\MicroSIP Custom\\microsip.exe";
        const std::wstring original = L"\"C:\\Program Files\\MicroSIP\\microsip.exe\" /minimized";
        Expect(StartupRegistration::TargetsExecutable(L"\"C:\\TOOLS\\MicroSIP Custom\\microsip.exe\" /minimized", custom),
            "case-insensitive quoted target");
        Expect(StartupRegistration::TargetsExecutable(L"  \"C:\\Tools\\MicroSIP Custom\\microsip.exe\" /minimized", custom),
            "leading whitespace");
        Expect(!StartupRegistration::TargetsExecutable(original, custom), "different installation");
        Expect(!StartupRegistration::TargetsExecutable(L"\"C:\\Tools\\MicroSIP Custom\\microsip.exe.backup\"", custom),
            "filename suffix must not match");
        Expect(!StartupRegistration::TargetsExecutable(L"other.exe \"C:\\Tools\\MicroSIP Custom\\microsip.exe\"", custom),
            "argument mention must not match");
        Expect(!StartupRegistration::TargetsExecutable(L"", custom), "empty command must not match");
        Expect(StartupRegistration::IsMicroSIP(original), "recognize official executable");
        Expect(!StartupRegistration::IsMicroSIP(L"\"C:\\Other\\other.exe\" /minimized"), "ignore unrelated executable");
        const DWORD envLength = GetEnvironmentVariableW(L"SystemRoot", nullptr, 0);
        Expect(envLength != 0, "SystemRoot available");
        std::wstring windows(envLength, L'\0');
        GetEnvironmentVariableW(L"SystemRoot", &windows[0], envLength);
        windows.resize(envLength - 1);
        Expect(StartupRegistration::TargetsExecutable(L"\"%SystemRoot%\\microsip.exe\" /minimized", windows + L"\\microsip.exe"),
            "expand registry environment path");

        const std::wstring profile = L"C:\\Custom Profiles\\Sales";
        const std::wstring profiled = L"\"" + custom + L"\" /profile-directory \"" + profile + L"\" /minimized";
        Expect(StartupRegistration::TargetsProfile(L"\"" + custom + L"\" /minimized", custom),
            "default startup targets default profile");
        Expect(StartupRegistration::TargetsProfile(profiled, custom, L"c:\\CUSTOM PROFILES\\Sales\\"),
            "profile paths match case-insensitively with a trailing slash");
        Expect(!StartupRegistration::TargetsProfile(profiled, custom), "explicit profile is not default profile");
        Expect(!StartupRegistration::TargetsProfile(profiled, custom, L"C:\\Other Profile"),
            "different profiles of the same executable remain distinct");
        Expect(!StartupRegistration::TargetsProfile(L"\"" + custom + L"\" /profile-directory", custom),
            "missing profile argument does not match default startup");
        Expect(!StartupRegistration::TargetsProfile(profiled + L" /profile-directory \"C:\\Other\"", custom, profile),
            "ambiguous duplicate profile option does not match");

        TestKey test;
        const wchar_t* name = L"MicroSIP";
        Expect(RegSetValueExW(test.key, name, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(original.c_str()),
            static_cast<DWORD>((original.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS, "seed original startup");
        Expect(StartupRegistration::Set(test.key, name, custom, false) == ERROR_SUCCESS,
            "unchecked custom setting succeeds without deleting original");
        std::wstring command;
        Expect(StartupRegistration::ReadCommand(test.key, name, command) && command == original,
            "saving unrelated custom settings preserves original startup");
        Expect(StartupRegistration::Set(test.key, name, custom, true) == ERROR_SUCCESS, "switch startup target");
        Expect(StartupRegistration::ReadCommand(test.key, name, command) &&
            command == L"\"" + custom + L"\" /minimized", "quoted custom startup runs minimized");
        Expect(StartupRegistration::TargetsExecutable(command, custom), "current custom startup detected");
        Expect(StartupRegistration::Set(test.key, name, custom, false) == ERROR_SUCCESS, "disable own startup");
        Expect(!StartupRegistration::ReadCommand(test.key, name, command), "own entry removed");
        Expect(StartupRegistration::Set(test.key, name, custom, false) == ERROR_SUCCESS, "disable missing startup is harmless");
        Expect(StartupRegistration::Set(test.key, name, custom, true, profile) == ERROR_SUCCESS,
            "enable startup for an explicit profile");
        Expect(StartupRegistration::ReadCommand(test.key, name, command) && command == profiled,
            "startup preserves quoted profile option before minimized");
        Expect(StartupRegistration::Set(test.key, name, custom, false) == ERROR_SUCCESS &&
            StartupRegistration::ReadCommand(test.key, name, command) && command == profiled,
            "default profile must not disable explicit profile startup");
        Expect(StartupRegistration::Set(test.key, name, custom, false, L"C:\\Other Profile") == ERROR_SUCCESS &&
            StartupRegistration::ReadCommand(test.key, name, command) && command == profiled,
            "another profile must not disable the current startup entry");
        Expect(StartupRegistration::Set(test.key, name, custom, false, profile) == ERROR_SUCCESS &&
            !StartupRegistration::ReadCommand(test.key, name, command), "disable matching explicit profile");
        Expect(StartupRegistration::Set(test.key, name, custom, true, L"C:\\") == ERROR_SUCCESS &&
            StartupRegistration::ReadCommand(test.key, name, command) &&
            StartupRegistration::TargetsProfile(command, custom, L"C:\\"),
            "quoted drive root survives command-line backslash escaping");
        Expect(StartupRegistration::Set(test.key, name, custom, true, L"C:\\bad\"profile") == ERROR_INVALID_PARAMETER,
            "reject quote injection in a profile path");
        std::cout << "PASS: exact startup target parsing, explicit profiles, migration and ownership in an isolated registry key\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
