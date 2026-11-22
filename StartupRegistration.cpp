// SPDX-License-Identifier: GPL-2.0-or-later
#include "StartupRegistration.h"
#include <shellapi.h>
#include <vector>

namespace StartupRegistration {
namespace {
std::wstring QuoteArgument(const std::wstring& argument)
{
    // CommandLineToArgvW treats backslashes immediately before a closing quote
    // as escapes. A directory can end in a slash, including a drive root.
    const size_t last = argument.find_last_not_of(L'\\');
    const size_t trailing = last == std::wstring::npos ? argument.size() : argument.size() - last - 1;
    return L"\"" + argument + std::wstring(trailing, L'\\') + L"\"";
}

std::wstring NormalizePath(const std::wstring& path)
{
    if (path.empty()) {
        return {};
    }
    DWORD length = ExpandEnvironmentStringsW(path.c_str(), nullptr, 0);
    if (!length) {
        return {};
    }
    std::vector<wchar_t> expanded(length);
    if (!ExpandEnvironmentStringsW(path.c_str(), expanded.data(), length)) {
        return {};
    }
    length = GetFullPathNameW(expanded.data(), 0, nullptr, nullptr);
    if (!length) {
        return {};
    }
    std::vector<wchar_t> full(length);
    if (!GetFullPathNameW(expanded.data(), length, full.data(), nullptr)) {
        return {};
    }
    // Match an existing short (8.3) path to its long spelling as well.
    length = GetLongPathNameW(full.data(), nullptr, 0);
    if (length) {
        std::vector<wchar_t> longPath(length);
        if (GetLongPathNameW(full.data(), longPath.data(), length)) {
            return longPath.data();
        }
    }
    return full.data();
}

std::wstring NormalizeDirectory(const std::wstring& directory)
{
    std::wstring path = NormalizePath(directory);
    while (path.size() > 1 && path.back() == L'\\' && path[path.size() - 2] != L':') {
        path.pop_back();
    }
    return path;
}
}

bool ReadCommand(HKEY key, const wchar_t* valueName, std::wstring& command)
{
    command.clear();
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes % sizeof(wchar_t) ||
        bytes > 32768 * sizeof(wchar_t)) {
        return false;
    }
    std::vector<wchar_t> text(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, valueName, nullptr, &type,
        reinterpret_cast<BYTE*>(text.data()), &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return false;
    }
    command = text.data();
    return true;
}

std::wstring Executable(const std::wstring& command)
{
    const size_t first = command.find_first_not_of(L" \t");
    if (first == std::wstring::npos) {
        return {};
    }
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(command.c_str() + first, &count);
    if (!arguments) {
        return {};
    }
    const std::wstring executable = count ? arguments[0] : L"";
    LocalFree(arguments);
    return executable;
}

bool TargetsExecutable(const std::wstring& command, const std::wstring& executable)
{
    const std::wstring target = NormalizePath(Executable(command));
    const std::wstring current = NormalizePath(executable);
    return !target.empty() && !current.empty() &&
        CompareStringOrdinal(target.c_str(), -1, current.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool TargetsProfile(const std::wstring& command, const std::wstring& executable,
    const std::wstring& profileDirectory)
{
    if (!TargetsExecutable(command, executable)) {
        return false;
    }
    const size_t first = command.find_first_not_of(L" \t");
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(command.c_str() + first, &count);
    if (!arguments) {
        return false;
    }
    bool hasProfile = false;
    for (int i = 1; i < count; ++i) {
        if (_wcsicmp(arguments[i], L"/profile-directory") == 0) {
            hasProfile = true;
        }
    }
    bool matches = profileDirectory.empty() && !hasProfile;
    if (!profileDirectory.empty() && (count == 3 || count == 4) &&
        _wcsicmp(arguments[1], L"/profile-directory") == 0 &&
        (count == 3 || _wcsicmp(arguments[3], L"/minimized") == 0)) {
        const std::wstring target = NormalizeDirectory(arguments[2]);
        const std::wstring expected = NormalizeDirectory(profileDirectory);
        matches = !target.empty() && !expected.empty() &&
            CompareStringOrdinal(target.c_str(), -1, expected.c_str(), -1, TRUE) == CSTR_EQUAL;
    }
    LocalFree(arguments);
    return matches;
}

bool IsMicroSIP(const std::wstring& command)
{
    const std::wstring executable = NormalizePath(Executable(command));
    const size_t slash = executable.find_last_of(L"\\/");
    const wchar_t* name = executable.c_str() + (slash == std::wstring::npos ? 0 : slash + 1);
    return _wcsicmp(name, L"microsip.exe") == 0 || _wcsicmp(name, L"microsip-lite.exe") == 0;
}

LONG Set(HKEY key, const wchar_t* valueName, const std::wstring& executable, bool enable,
    const std::wstring& profileDirectory)
{
    if (enable) {
        if (executable.empty() || executable.find(L'"') != std::wstring::npos ||
            profileDirectory.find(L'"') != std::wstring::npos) {
            return ERROR_INVALID_PARAMETER;
        }
        std::wstring command = QuoteArgument(executable);
        if (!profileDirectory.empty()) {
            command += L" /profile-directory " + QuoteArgument(profileDirectory);
        }
        command += L" /minimized";
        return RegSetValueExW(key, valueName, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    }
    // An unchecked custom startup setting must not delete another installation's
    // entry when the user merely saves unrelated preferences.
    std::wstring command;
    if (!ReadCommand(key, valueName, command) || !TargetsProfile(command, executable, profileDirectory)) {
        return ERROR_SUCCESS;
    }
    const LONG status = RegDeleteValueW(key, valueName);
    return status == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : status;
}
}
