// SPDX-License-Identifier: GPL-2.0-or-later
#include "../CustomUpdate.h"
#include "../lib/jsoncpp/json/json.h"
#include <iostream>
#include <stdexcept>

namespace {
void Expect(bool condition, const char* description)
{
    if (!condition) throw std::runtime_error(description);
}

Json::Value Fixture()
{
    Json::Value release(Json::objectValue);
    release["draft"] = false;
    release["prerelease"] = false;
    release["tag_name"] = "v3.22.16.10";
    release["html_url"] = "https://github.com/AvrumFeldman/microsip-custom/releases/tag/v3.22.16.10";
    for (const char* name : {"MicroSIP-Custom-3.22.16.10.zip", "MicroSIP-Custom-3.22.16.10-SHA256SUMS.txt"}) {
        Json::Value asset(Json::objectValue);
        asset["name"] = name;
        asset["state"] = "uploaded";
        asset["size"] = 6252373u;
        if (std::string(name).find("SHA256") != std::string::npos) asset["size"] = 500u;
        asset["browser_download_url"] = std::string("https://github.com/AvrumFeldman/microsip-custom/releases/download/v3.22.16.10/") + name;
        release["assets"].append(asset);
    }
    return release;
}

bool Parse(const Json::Value& json)
{
    CustomUpdate::Release release;
    std::wstring error;
    bool result = CustomUpdate::ParseRelease(Json::FastWriter().write(json), release, error);
    Expect(result || !error.empty(), "a rejected release explains its error");
    return result;
}
}

int main(int argc, char** argv)
{
    try {
        std::array<unsigned, 4> old, newer;
        Expect(CustomUpdate::ParseVersion(L"3.22.16.9", old) && CustomUpdate::ParseVersion(L"3.22.16.10", newer) && newer > old,
            "versions compare numerically rather than lexically");
        for (const wchar_t* bad : {L"3.22.16", L"v3.22.16.5", L"3.22.16.5-beta", L"3.22.16.65536", L"3.22.16.-1", L"3.22.16.5 ", L"3.22.16.99999999999999999"})
            Expect(!CustomUpdate::ParseVersion(bad, newer), "invalid versions rejected");
        const std::wstring canonical = L"https://github.com/AvrumFeldman/microsip-custom/releases/download/v3.22.16.10/MicroSIP-Custom-3.22.16.10.zip";
        Expect(CustomUpdate::IsAssetUrl(canonical, L"v3.22.16.10", L"MicroSIP-Custom-3.22.16.10.zip"), "exact repository/tag/asset URL");
        for (const wchar_t* bad : {
            L"http://github.com/AvrumFeldman/microsip-custom/releases/download/v3.22.16.10/file.zip",
            L"https://github.com.evil.test/AvrumFeldman/microsip-custom/releases/download/v3.22.16.10/file.zip",
            L"https://github.com@evil.test/AvrumFeldman/microsip-custom/releases/download/v3.22.16.10/file.zip",
            L"https://github.com/another/repository/releases/download/v3.22.16.10/file.zip",
            L"https://release-assets.githubusercontent.com.evil.test/github-production-release-asset/file",
            L"https://release-assets.githubusercontent.com:444/github-production-release-asset/file",
            L"https://evil@release-assets.githubusercontent.com/github-production-release-asset/file",
            L"https://release-assets.githubusercontent.com/github-production-release-asset/file#fragment",
            L"https://release-assets.githubusercontent.com/other/file",
            L"file:///C:/malicious.zip"})
            Expect(!CustomUpdate::IsAllowedDownloadUrl(bad), "untrusted release hosts, schemes, credentials and ports rejected");
        Expect(CustomUpdate::IsAllowedDownloadUrl(L"https://release-assets.githubusercontent.com/github-production-release-asset/123/file?sig=test%2Fvalue"), "signed GitHub asset redirect");
        Expect(CustomUpdate::IsAllowedDownloadUrl(L"https://objects.githubusercontent.com/github-production-release-asset-2e65be/123/file?sig=test"), "older signed GitHub asset redirect");
        auto valid = Fixture();
        Expect(Parse(valid), "stable complete GitHub release accepted");
        auto bad = valid; bad["prerelease"] = true; Expect(!Parse(bad), "prerelease rejected");
        bad = valid; bad["draft"] = true; Expect(!Parse(bad), "draft rejected");
        bad = valid; bad["html_url"] = "https://github.com/elsewhere/microsip/releases/tag/v3.22.16.10"; Expect(!Parse(bad), "foreign repository rejected");
        bad = valid; bad["tag_name"] = "v3.22.16.10-rc1"; Expect(!Parse(bad), "non-numeric tag rejected");
        bad = valid; bad["assets"][0u]["browser_download_url"] = "https://example.test/evil.zip"; Expect(!Parse(bad), "foreign binary asset rejected");
        bad = valid; bad["assets"][0u]["size"] = 0u; Expect(!Parse(bad), "empty asset rejected");
        bad = valid; bad["assets"][0u]["size"] = 134217729u; Expect(!Parse(bad), "oversized archive rejected");
        bad = valid; bad["assets"][0u]["digest"] = "md5:1234"; Expect(!Parse(bad), "invalid asset digest rejected");
        bad = valid; bad["assets"].append(valid["assets"][0u]); Expect(!Parse(bad), "ambiguous duplicate binary rejected");
        bad = valid; bad["assets"].resize(1); Expect(!Parse(bad), "missing checksum asset rejected");
        std::wstring digest, error;
        const std::string hash(64, 'a');
        const std::wstring archive = L"MicroSIP-Custom-3.22.16.10.zip";
        Expect(CustomUpdate::ParseArchiveChecksum(hash + "  MicroSIP-Custom-3.22.16.10.zip\r\n", archive, digest, error) && digest == std::wstring(64, L'a'), "exact binary checksum selected");
        Expect(!CustomUpdate::ParseArchiveChecksum(hash + "  other.zip\n", archive, digest, error), "wrong archive checksum rejected");
        Expect(!CustomUpdate::ParseArchiveChecksum(hash + "  MicroSIP-Custom-3.22.16.10.zip\n" + hash + "  MicroSIP-Custom-3.22.16.10.zip\n", archive, digest, error), "duplicate archive checksum rejected");
        Expect(!CustomUpdate::ParseArchiveChecksum("garbage", archive, digest, error), "malformed checksum manifest rejected");
        std::cout << "PASS: numeric versions, stable release identity, exact assets, HTTPS redirect policy, bounds and SHA256 manifests\n";
        if (argc > 1 && std::string(argv[1]) == "--live") {
            const auto latest = CustomUpdate::CheckLatest(L"3.22.16.6");
            if (!latest.error.empty()) {
                std::wcerr << L"LIVE CHECK UNAVAILABLE: " << latest.error << L'\n';
                return 2;
            }
            std::wcout << L"PASS: live stable release " << latest.release.version << L" from " << latest.release.pageUrl
                << L"; update available=" << latest.updateAvailable << L'\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
