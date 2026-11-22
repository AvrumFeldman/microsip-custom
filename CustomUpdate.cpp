// SPDX-License-Identifier: GPL-2.0-or-later
// Only this fork's stable GitHub releases are an update source.
#include "CustomUpdate.h"
#include "lib/jsoncpp/json/json.h"
#include <winhttp.h>
#include <wincrypt.h>
#include <objbase.h>
#include <algorithm>
#include <vector>
#include <stdexcept>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")

namespace CustomUpdate {
namespace {
const wchar_t* kRepository = L"https://github.com/AvrumFeldman/microsip-custom";
const wchar_t* kLatest = L"https://api.github.com/repos/AvrumFeldman/microsip-custom/releases/latest";
const DWORD kHelperResource = 16001;
const unsigned long long kMaxArchive = 128ULL * 1024 * 1024;
const size_t kMaxManifest = 256 * 1024;

struct InternetHandle {
    HINTERNET value;
    explicit InternetHandle(HINTERNET handle) : value(handle) {}
    ~InternetHandle() { if (value) WinHttpCloseHandle(value); }
    operator HINTERNET() const { return value; }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
};
struct FileHandle {
    HANDLE value;
    explicit FileHandle(HANDLE handle) : value(handle) {}
    ~FileHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

void Fail(const wchar_t* message) { throw std::wstring(message); }
void Require(bool condition, const wchar_t* message) { if (!condition) Fail(message); }
void HttpRequire(bool condition, const wchar_t* message)
{
    if (!condition) {
        const DWORD code = GetLastError();
        throw std::wstring(message) + L" (WinHTTP " + std::to_wstring(code) + L").";
    }
}

std::wstring Wide(const std::string& value)
{
    if (value.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0);
    Require(size > 0, L"GitHub returned invalid UTF-8.");
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), &result[0], size);
    return result;
}

bool HexHash(const std::wstring& value)
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
    });
}

std::wstring Lower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return c >= L'A' && c <= L'Z' ? wchar_t(c + (L'a' - L'A')) : c;
    });
    return value;
}

bool UrlParts(const std::wstring& url, std::wstring& host, std::wstring& path, std::wstring& extra)
{
    if (url.size() > 16384 || url.find_first_of(L"\r\n\t#") != std::wstring::npos) return false;
    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
    parts.dwUserNameLength = parts.dwPasswordLength = DWORD(-1);
    if (!WinHttpCrackUrl(url.c_str(), DWORD(url.size()), 0, &parts) ||
        parts.nScheme != INTERNET_SCHEME_HTTPS || parts.nPort != INTERNET_DEFAULT_HTTPS_PORT ||
        parts.dwUserNameLength || parts.dwPasswordLength) return false;
    host = Lower(std::wstring(parts.lpszHostName, parts.dwHostNameLength));
    path.assign(parts.lpszUrlPath ? parts.lpszUrlPath : L"", parts.dwUrlPathLength);
    extra.assign(parts.lpszExtraInfo ? parts.lpszExtraInfo : L"", parts.dwExtraInfoLength);
    return !host.empty();
}

std::wstring AssetName(const Release& release)
{
    return L"MicroSIP-Custom-" + release.version + L".zip";
}

std::wstring ChecksumName(const Release& release)
{
    return L"MicroSIP-Custom-" + release.version + L"-SHA256SUMS.txt";
}

// Redirects are followed manually, with certificate verification left enabled.
// Signed release-asset URLs are only accepted from GitHub's dedicated hosts.
void Fetch(const std::wstring& initialUrl, size_t limit, std::string* body,
    const std::wstring& filename = L"")
{
    InternetHandle session(WinHttpOpen(L"MicroSIP-Custom-Updater/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    HttpRequire(session.value != nullptr, L"Could not initialize the secure GitHub connection");
    HttpRequire(WinHttpSetTimeouts(session, 10000, 10000, 15000, 15000) != FALSE, L"Could not bound GitHub connection timeouts");
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    Require(WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols)) != FALSE,
        L"TLS 1.2 is unavailable. The update could not be checked securely.");
    const bool api = initialUrl == kLatest;
    std::wstring url = initialUrl;
    const ULONGLONG started = GetTickCount64();
    for (int redirects = 0; redirects <= 5; ++redirects) {
        std::wstring host, path, extra;
        Require(UrlParts(url, host, path, extra), L"The release download URL is invalid.");
        Require(api ? url == kLatest : IsAllowedDownloadUrl(url), L"The release redirected outside trusted GitHub hosts.");
        InternetHandle connection(WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
        HttpRequire(connection.value != nullptr, L"Could not connect to GitHub");
        const std::wstring target = path + extra;
        InternetHandle request(WinHttpOpenRequest(connection, L"GET", target.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        HttpRequire(request.value != nullptr, L"Could not create the secure release request");
        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        DWORD authPolicy = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
        DWORD disabled = WINHTTP_DISABLE_COOKIES;
        Require(WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy)) != FALSE,
            L"Could not enforce update redirect policy.");
        WinHttpSetOption(request, WINHTTP_OPTION_AUTOLOGON_POLICY, &authPolicy, sizeof(authPolicy));
        WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled));
        // Public GitHub assets never require a personal client certificate.
        // Explicitly opt out; server certificate validation remains unchanged.
        HttpRequire(WinHttpSetOption(request, WINHTTP_OPTION_CLIENT_CERT_CONTEXT,
            WINHTTP_NO_CLIENT_CERT_CONTEXT, 0) != FALSE, L"Could not disable personal client certificates for GitHub");
        const wchar_t* headers = api ?
            L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n" :
            L"Accept: application/octet-stream\r\n";
        HttpRequire(WinHttpSendRequest(request, headers, DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE,
            L"GitHub could not be reached securely; check your connection and try again");
        HttpRequire(WinHttpReceiveResponse(request, nullptr) != FALSE,
            L"The secure GitHub response could not be received");
        DWORD status = 0, size = sizeof(status);
        Require(WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) != FALSE,
            L"GitHub returned an unreadable HTTP response.");
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            Require(!api && redirects < 5, L"Unexpected GitHub release redirect.");
            size = 0;
            WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                nullptr, &size, WINHTTP_NO_HEADER_INDEX);
            Require(size >= sizeof(wchar_t) && size <= 32768, L"Invalid GitHub release redirect.");
            std::vector<wchar_t> location(size / sizeof(wchar_t) + 1, 0);
            Require(WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                location.data(), &size, WINHTTP_NO_HEADER_INDEX) != FALSE, L"Invalid GitHub release redirect.");
            url = location.data();
            continue;
        }
        Require(status != 403 && status != 429, L"GitHub has limited update requests. Try again later.");
        Require(status == 200, L"GitHub did not return the requested release asset.");
        FileHandle output(filename.empty() ? INVALID_HANDLE_VALUE : CreateFileW(filename.c_str(), GENERIC_WRITE,
            0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        Require(filename.empty() || output.value != INVALID_HANDLE_VALUE, L"Could not create the staged download.");
        if (body) body->clear();
        size_t total = 0;
        char buffer[32768];
        while (true) {
            Require(GetTickCount64() - started < 180000, L"The GitHub download timed out.");
            DWORD received = 0;
            HttpRequire(WinHttpReadData(request, buffer, sizeof(buffer), &received) != FALSE,
                L"The GitHub download was interrupted");
            if (!received) break;
            Require(received <= limit - total, L"The release response exceeds its allowed size.");
            total += received;
            if (body) body->append(buffer, received);
            if (!filename.empty()) {
                DWORD written = 0;
                Require(WriteFile(output.value, buffer, received, &written, nullptr) && written == received,
                    L"Could not save the downloaded release.");
            }
        }
        Require(total > 0, L"GitHub returned an empty release response.");
        return;
    }
    Fail(L"Too many release download redirects.");
}

std::wstring FileHash(const std::wstring& path)
{
    FileHandle input(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    Require(input.value != INVALID_HANDLE_VALUE, L"Could not read the downloaded archive.");
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    Require(CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) != FALSE,
        L"SHA256 verification is unavailable.");
    try {
        Require(CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) != FALSE, L"SHA256 verification is unavailable.");
        BYTE buffer[32768];
        DWORD received = 0;
        do {
            Require(ReadFile(input.value, buffer, sizeof(buffer), &received, nullptr) != FALSE,
                L"Could not read the downloaded archive.");
            if (received) Require(CryptHashData(hash, buffer, received, 0) != FALSE, L"SHA256 verification failed.");
        } while (received);
        BYTE digest[32];
        DWORD size = sizeof(digest);
        Require(CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0) != FALSE && size == 32,
            L"SHA256 verification failed.");
        std::wstring result;
        const wchar_t* digits = L"0123456789abcdef";
        for (BYTE b : digest) { result += digits[b >> 4]; result += digits[b & 15]; }
        CryptDestroyHash(hash);
        CryptReleaseContext(provider, 0);
        return result;
    } catch (...) {
        if (hash) CryptDestroyHash(hash);
        CryptReleaseContext(provider, 0);
        throw;
    }
}

std::wstring Literal(const std::wstring& text)
{
    std::wstring result = L"'";
    for (wchar_t c : text) { result += c; if (c == L'\'') result += c; }
    return result + L"'";
}

std::wstring Powershell()
{
    wchar_t directory[MAX_PATH];
    Require(GetSystemDirectoryW(directory, MAX_PATH) != 0, L"Could not locate Windows PowerShell.");
    std::wstring path = std::wstring(directory) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    Require(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES, L"Windows PowerShell is unavailable.");
    return path;
}

DWORD StartHelper(const std::wstring& stage, const std::wstring& arguments, bool wait)
{
    const std::wstring executable = Powershell();
    const std::wstring script = L"& " + Literal(stage + L"\\Apply-Update.ps1") + arguments;
    DWORD length = 0;
    Require(CryptBinaryToStringW(reinterpret_cast<const BYTE*>(script.data()), DWORD(script.size() * sizeof(wchar_t)),
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &length) != FALSE, L"Could not encode the update command.");
    std::wstring encoded(length, L'\0');
    Require(CryptBinaryToStringW(reinterpret_cast<const BYTE*>(script.data()), DWORD(script.size() * sizeof(wchar_t)),
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &encoded[0], &length) != FALSE, L"Could not encode the update command.");
    encoded.resize(wcslen(encoded.c_str()));
    std::wstring command = L"\"" + executable + L"\" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -EncodedCommand " + encoded;
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(0);
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process = {};
    Require(CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, stage.c_str(), &startup, &process) != FALSE,
        L"Could not start the MicroSIP Custom update worker.");
    FileHandle processHandle(process.hProcess), threadHandle(process.hThread);
    if (!wait) return 0;
    Require(WaitForSingleObject(processHandle.value, 180000) == WAIT_OBJECT_0,
        L"Package validation did not finish. The running application was left unchanged.");
    DWORD code = 1;
    GetExitCodeProcess(processHandle.value, &code);
    return code;
}

std::wstring ReadWorkerError(const std::wstring& stage)
{
    FileHandle file(CreateFileW((stage + L"\\update-error.txt").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) return L"The release package could not be validated. See " + stage;
    char data[8192];
    DWORD read = 0;
    if (!ReadFile(file.value, data, sizeof(data), &read, nullptr)) return L"The release package could not be validated.";
    return Wide(std::string(data, read));
}
}

bool ParseVersion(const std::wstring& value, std::array<unsigned, 4>& version)
{
    size_t position = 0;
    for (size_t i = 0; i < version.size(); ++i) {
        unsigned number = 0;
        size_t start = position;
        while (position < value.size() && value[position] >= L'0' && value[position] <= L'9') {
            number = number * 10 + unsigned(value[position++] - L'0');
            if (number > 65535 || position - start > 5) return false;
        }
        if (position == start) return false;
        version[i] = number;
        if (i < 3 && (position >= value.size() || value[position++] != L'.')) return false;
    }
    return position == value.size();
}

bool IsAssetUrl(const std::wstring& url, const std::wstring& tag, const std::wstring& assetName)
{
    std::array<unsigned, 4> version;
    return tag.size() > 1 && tag[0] == L'v' && ParseVersion(tag.substr(1), version) &&
        url == std::wstring(kRepository) + L"/releases/download/" + tag + L"/" + assetName;
}

bool IsAllowedDownloadUrl(const std::wstring& url)
{
    std::wstring host, path, extra;
    if (!UrlParts(url, host, path, extra)) return false;
    if (host == L"github.com") {
        const std::wstring prefix = L"/AvrumFeldman/microsip-custom/releases/download/";
        if (path.compare(0, prefix.size(), prefix) != 0 || !extra.empty()) return false;
        const std::wstring rest = path.substr(prefix.size());
        const size_t slash = rest.find(L'/');
        if (slash == std::wstring::npos) return false;
        const std::wstring tag = rest.substr(0, slash), name = rest.substr(slash + 1);
        std::array<unsigned, 4> version;
        if (tag.size() < 2 || tag[0] != L'v' || !ParseVersion(tag.substr(1), version)) return false;
        return name == L"MicroSIP-Custom-" + tag.substr(1) + L".zip" ||
            name == L"MicroSIP-Custom-" + tag.substr(1) + L"-SHA256SUMS.txt";
    }
    if (host == L"release-assets.githubusercontent.com")
        return path.compare(0, 33, L"/github-production-release-asset/") == 0;
    if (host == L"objects.githubusercontent.com")
        return path.compare(0, 40, L"/github-production-release-asset-2e65be/") == 0;
    return false;
}

bool ParseRelease(const std::string& json, Release& release, std::wstring& error)
{
    release = Release();
    error.clear();
    try {
        Require(json.size() <= 2 * 1024 * 1024, L"The release metadata exceeds its allowed size.");
        Json::Value root;
        Json::Reader reader;
        Require(reader.parse(json, root, false) && root.isObject(), L"GitHub returned invalid release metadata.");
        Require(root["draft"].isBool() && !root["draft"].asBool() &&
            root["prerelease"].isBool() && !root["prerelease"].asBool(), L"Only stable published releases can be installed.");
        Require(root["tag_name"].isString() && root["html_url"].isString(), L"The release identity is missing.");
        release.tag = Wide(root["tag_name"].asString());
        std::array<unsigned, 4> version;
        Require(release.tag.size() > 1 && release.tag[0] == L'v' && ParseVersion(release.tag.substr(1), version),
            L"The release tag is not a four-component MicroSIP Custom version.");
        release.version = release.tag.substr(1);
        release.pageUrl = Wide(root["html_url"].asString());
        Require(release.pageUrl == std::wstring(kRepository) + L"/releases/tag/" + release.tag,
            L"The release does not belong to the MicroSIP Custom repository.");
        Require(root["assets"].isArray() && root["assets"].size() <= 100,
            L"The release asset list is invalid.");
        for (Json::Value::ArrayIndex i = 0; i < root["assets"].size(); ++i) {
            const Json::Value& asset = root["assets"][i];
            if (!asset.isObject() || !asset["name"].isString()) continue;
            const std::wstring name = Wide(asset["name"].asString());
            if (name != AssetName(release) && name != ChecksumName(release)) continue;
            Require(asset["browser_download_url"].isString() && asset["state"].isString() &&
                asset["state"].asString() == "uploaded" && (asset["size"].isUInt() ||
                    (asset["size"].isInt() && asset["size"].asInt() >= 0)), L"A required release asset is not ready.");
            const std::wstring url = Wide(asset["browser_download_url"].asString());
            Require(IsAssetUrl(url, release.tag, name), L"A release asset does not belong to the expected repository and tag.");
            if (name == AssetName(release)) {
                Require(release.packageUrl.empty(), L"The release has duplicate binary assets.");
                release.packageSize = asset["size"].asUInt();
                Require(release.packageSize > 0 && release.packageSize <= kMaxArchive, L"The release archive exceeds its allowed size.");
                release.packageUrl = url;
                if (asset["digest"].isString()) {
                    std::wstring digest = Wide(asset["digest"].asString());
                    Require(digest.compare(0, 7, L"sha256:") == 0 && HexHash(digest.substr(7)), L"The release archive digest is invalid.");
                    release.packageSha256 = Lower(digest.substr(7));
                }
            } else {
                Require(release.checksumsUrl.empty(), L"The release has duplicate checksum assets.");
                Require(asset["size"].asUInt() > 0 && asset["size"].asUInt() <= kMaxManifest, L"The release checksum file exceeds its allowed size.");
                release.checksumsUrl = url;
            }
        }
        Require(!release.packageUrl.empty() && !release.checksumsUrl.empty(), L"This release has no matching binary archive and checksum file.");
        return true;
    } catch (const std::wstring& message) { error = message; }
    catch (...) { error = L"Could not parse the GitHub release metadata."; }
    release = Release();
    return false;
}

bool ParseArchiveChecksum(const std::string& manifest, const std::wstring& name,
    std::wstring& checksum, std::wstring& error)
{
    checksum.clear();
    error.clear();
    try {
        Require(!manifest.empty() && manifest.size() <= kMaxManifest, L"The release checksum file is invalid.");
        size_t begin = 0;
        while (begin < manifest.size()) {
            size_t end = manifest.find('\n', begin);
            if (end == std::string::npos) end = manifest.size();
            std::string line = manifest.substr(begin, end - begin);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            begin = end + 1;
            if (line.empty()) continue;
            Require(line.size() > 66 && line[64] == ' ' && (line[65] == ' ' || line[65] == '*'), L"The release checksum file has an invalid entry.");
            const std::wstring digest = Wide(line.substr(0, 64));
            Require(HexHash(digest), L"The release checksum file has an invalid digest.");
            if (Wide(line.substr(66)) == name) {
                Require(checksum.empty(), L"The release checksum file has duplicate archive entries.");
                checksum = Lower(digest);
            }
        }
        Require(!checksum.empty(), L"The release checksum file does not name the selected binary archive.");
        return true;
    } catch (const std::wstring& message) { error = message; checksum.clear(); return false; }
}

CheckResult CheckLatest(const std::wstring& currentVersion)
{
    CheckResult result;
    try {
        std::array<unsigned, 4> current, latest;
        Require(ParseVersion(currentVersion, current), L"The running application's version is invalid.");
        std::string json;
        Fetch(kLatest, 2 * 1024 * 1024, &json);
        if (!ParseRelease(json, result.release, result.error)) return result;
        Require(ParseVersion(result.release.version, latest), L"The release version is invalid.");
        result.updateAvailable = latest > current;
    } catch (const std::wstring& message) { result.error = message; }
    catch (...) { result.error = L"Could not check for MicroSIP Custom updates."; }
    return result;
}

StageResult Stage(const Release& release)
{
    StageResult result;
    try {
        std::array<unsigned, 4> version;
        Require(ParseVersion(release.version, version) && release.tag == L"v" + release.version &&
            IsAssetUrl(release.packageUrl, release.tag, AssetName(release)) &&
            IsAssetUrl(release.checksumsUrl, release.tag, ChecksumName(release)) &&
            release.packageSize > 0 && release.packageSize <= kMaxArchive, L"The selected release is invalid.");
        std::string manifest;
        Fetch(release.checksumsUrl, kMaxManifest, &manifest);
        std::wstring error;
        if (!ParseArchiveChecksum(manifest, AssetName(release), result.archiveSha256, error)) throw error;
        Require(release.packageSha256.empty() || release.packageSha256 == result.archiveSha256,
            L"GitHub's archive digest differs from the release checksum file.");
        wchar_t temporary[MAX_PATH];
        DWORD tempLength = GetTempPathW(MAX_PATH, temporary);
        Require(tempLength > 0 && tempLength < MAX_PATH, L"Could not locate the local update staging directory.");
        GUID id;
        Require(SUCCEEDED(CoCreateGuid(&id)), L"Could not create an isolated update staging directory.");
        wchar_t identifier[40];
        StringFromGUID2(id, identifier, 40);
        result.stagingPath = std::wstring(temporary) + L"MicroSIPCustomUpdate-" + identifier;
        Require(CreateDirectoryW(result.stagingPath.c_str(), nullptr) != FALSE, L"Could not create the isolated update staging directory.");
        result.version = release.version;
        Fetch(release.packageUrl, size_t(release.packageSize), nullptr, result.stagingPath + L"\\release.zip");
        Require(FileHash(result.stagingPath + L"\\release.zip") == result.archiveSha256,
            L"The downloaded archive failed SHA256 verification.");
        HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(kHelperResource), RT_RCDATA);
        Require(resource != nullptr, L"This application is missing its embedded update helper.");
        HGLOBAL data = LoadResource(nullptr, resource);
        DWORD size = SizeofResource(nullptr, resource);
        const void* script = LockResource(data);
        Require(script && size > 0 && size <= 256 * 1024, L"The embedded update helper is invalid.");
        FileHandle helper(CreateFileW((result.stagingPath + L"\\Apply-Update.ps1").c_str(), GENERIC_WRITE, 0,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        Require(helper.value != INVALID_HANDLE_VALUE, L"Could not stage the embedded update helper.");
        DWORD written = 0;
        Require(WriteFile(helper.value, script, size, &written, nullptr) && written == size, L"Could not stage the embedded update helper.");
        CloseHandle(helper.value);
        helper.value = INVALID_HANDLE_VALUE;
        const std::wstring arguments = L" -Mode Prepare -StagePath " + Literal(result.stagingPath) +
            L" -ExpectedVersion " + Literal(result.version) + L" -ArchiveSha256 " + Literal(result.archiveSha256);
        if (StartHelper(result.stagingPath, arguments, true) != 0) throw ReadWorkerError(result.stagingPath);
    } catch (const std::wstring& message) { result.error = message; }
    catch (...) { result.error = L"Could not download and prepare the MicroSIP Custom update."; }
    return result;
}

bool LaunchApply(const StageResult& stage, const std::wstring& targetExe, DWORD parentPID,
    std::wstring& error, const std::wstring& profileDirectory)
{
    error.clear();
    try {
        std::array<unsigned, 4> version;
        Require(stage.error.empty() && !stage.stagingPath.empty() && ParseVersion(stage.version, version) &&
            HexHash(stage.archiveSha256), L"The staged update is invalid.");
        if (!profileDirectory.empty()) {
            const bool absolute = (profileDirectory.size() >= 3 && profileDirectory[1] == L':' &&
                (profileDirectory[2] == L'\\' || profileDirectory[2] == L'/')) ||
                (profileDirectory.size() >= 2 && profileDirectory[0] == L'\\' && profileDirectory[1] == L'\\');
            Require(absolute && std::none_of(profileDirectory.begin(), profileDirectory.end(), [](wchar_t c) {
                return c < 32 || c == L'"';
            }), L"The selected profile directory must be an absolute Windows path.");
        }
        Require(parentPID == GetCurrentProcessId(), L"The update worker must belong to the running application.");
        wchar_t executable[MAX_PATH];
        Require(GetModuleFileNameW(nullptr, executable, MAX_PATH) > 0 && _wcsicmp(executable, targetExe.c_str()) == 0,
            L"The update target is not the running application.");
        size_t slash = targetExe.find_last_of(L"\\/");
        Require(slash != std::wstring::npos && _wcsicmp(targetExe.substr(slash + 1).c_str(), L"microsip.exe") == 0,
            L"Rename this application to microsip.exe before using automatic updates.");
        FILETIME created, exited, kernel, user;
        Require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != FALSE,
            L"Could not identify the application being updated.");
        ULARGE_INTEGER started;
        started.HighPart = created.dwHighDateTime;
        started.LowPart = created.dwLowDateTime;
        std::wstring arguments = L" -Mode Apply -StagePath " + Literal(stage.stagingPath) +
            L" -ExpectedVersion " + Literal(stage.version) + L" -ArchiveSha256 " + Literal(stage.archiveSha256) +
            L" -InstallPath " + Literal(targetExe.substr(0, slash)) + L" -ParentProcessId " + std::to_wstring(parentPID) +
            L" -ParentStartedUtcFileTime " + std::to_wstring(started.QuadPart);
        if (!profileDirectory.empty()) arguments += L" -ProfileDirectory " + Literal(profileDirectory);
        StartHelper(stage.stagingPath, arguments, false);
        return true;
    } catch (const std::wstring& message) { error = message; }
    catch (...) { error = L"Could not launch the MicroSIP Custom update worker."; }
    return false;
}
}
