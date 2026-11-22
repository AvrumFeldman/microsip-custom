// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <windows.h>
#include <string>

namespace StartupRegistration {
// These helpers operate only on the explicitly supplied registry key, so tests
// can exercise startup changes without opening the user's actual Run key.
bool ReadCommand(HKEY key, const wchar_t* valueName, std::wstring& command);
std::wstring Executable(const std::wstring& command);
bool TargetsExecutable(const std::wstring& command, const std::wstring& executable);
bool TargetsProfile(const std::wstring& command, const std::wstring& executable,
    const std::wstring& profileDirectory = {});
bool IsMicroSIP(const std::wstring& command);
LONG Set(HKEY key, const wchar_t* valueName, const std::wstring& executable, bool enable,
    const std::wstring& profileDirectory = {});
}
