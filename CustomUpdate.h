// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <windows.h>
#include <array>
#include <string>

namespace CustomUpdate {
// These operations perform blocking network/disk work. The UI owns its worker
// thread and must recheck that no calls are present before invoking LaunchApply.
struct Release {
    std::wstring version, tag, pageUrl, packageUrl, checksumsUrl;
    unsigned long long packageSize = 0;
    std::wstring packageSha256;
};
struct CheckResult {
    bool updateAvailable = false;
    Release release;
    std::wstring error;
};
struct StageResult {
    std::wstring stagingPath, version, archiveSha256, error;
};

CheckResult CheckLatest(const std::wstring& currentVersion);
StageResult Stage(const Release& release);
bool LaunchApply(const StageResult& stage, const std::wstring& targetExe,
    DWORD parentPID, std::wstring& error, const std::wstring& profileDirectory = L"");

// Pure parsing/security functions are public so the release format, URL trust
// boundaries and version ordering can be regression tested without networking.
bool ParseVersion(const std::wstring& value, std::array<unsigned, 4>& version);
bool ParseRelease(const std::string& json, Release& release, std::wstring& error);
bool IsAssetUrl(const std::wstring& url, const std::wstring& tag,
    const std::wstring& assetName);
bool IsAllowedDownloadUrl(const std::wstring& url);
bool ParseArchiveChecksum(const std::string& manifest, const std::wstring& name,
    std::wstring& checksum, std::wstring& error);
}
