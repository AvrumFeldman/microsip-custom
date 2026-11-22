// SPDX-License-Identifier: GPL-2.0-or-later
#include "CustomProfile.h"

#include <algorithm>
#include <cwctype>
#include <stdexcept>
#include <vector>

namespace CustomProfile {
namespace {
const wchar_t* const kMigrationDirectory = L".custom-profile-migration";
const wchar_t* const kDataFiles[] = {
    L"Contacts.xml", L"contacts.db", L"contacts.db-wal", L"contacts.db-journal",
    L"call_log.db", L"call_log.db-wal", L"call_log.db-journal"
};

std::wstring Join(const std::wstring& directory, const std::wstring& name)
{
    return directory + (directory.empty() || directory.back() == L'\\' ? L"" : L"\\") + name;
}

bool IsAbsolute(const std::wstring& path)
{
    return (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) ||
        (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\');
}

std::wstring FullPath(const std::wstring& path)
{
    if (!IsAbsolute(path)) throw std::runtime_error("MicroSIP Custom requires an absolute profile path.");
    const DWORD size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Cannot resolve the MicroSIP Custom profile path.");
    std::wstring result(size, L'\0');
    const DWORD length = GetFullPathNameW(path.c_str(), size, &result[0], nullptr);
    if (!length || length >= size) throw std::runtime_error("Cannot resolve the MicroSIP Custom profile path.");
    result.resize(length);
    while (result.size() > 3 && result.back() == L'\\') result.pop_back();
    return result;
}

bool Exists(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

void EnsureDirectory(const std::wstring& directory)
{
    const DWORD attributes = GetFileAttributesW(directory.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) return;
    if (!CreateDirectoryW(directory.c_str(), nullptr)) {
        throw std::runtime_error("Cannot create the MicroSIP Custom profile directory. Check folder permissions.");
    }
}

std::wstring ProfileIdentity(std::wstring ini)
{
    std::transform(ini.begin(), ini.end(), ini.begin(), [](wchar_t c) { return std::towlower(c); });
    unsigned long long hash = 14695981039346656037ull;
    for (wchar_t c : ini) {
        hash ^= static_cast<unsigned short>(c);
        hash *= 1099511628211ull;
    }
    return L"Local\\MicroSIPCustom.Profile." + std::to_wstring(hash);
}

struct SourceLocks {
    std::vector<HANDLE> handles;
    ~SourceLocks() { for (HANDLE handle : handles) CloseHandle(handle); }
    void Add(const std::wstring& file)
    {
        // SQLite keeps its databases open for writing. Denying write/delete
        // sharing refuses a live old installation and keeps DB/WAL/journal
        // snapshots coherent without changing/checkpointing the original data.
        HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("Cannot migrate the existing MicroSIP Custom profile. Close the old custom copy "
                "and try again from its existing folder; its original settings have not been changed.");
        }
        handles.push_back(handle);
    }
};

void Flush(const std::wstring& file)
{
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot flush migrated MicroSIP Custom data.");
    const BOOL ok = FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!ok) throw std::runtime_error("Cannot flush migrated MicroSIP Custom data.");
}

void MakeStagedFileWritable(const std::wstring& file)
{
    const DWORD attributes = GetFileAttributesW(file.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        throw std::runtime_error("Cannot inspect staged MicroSIP Custom data.");
    }
    if (attributes & FILE_ATTRIBUTE_READONLY) {
        const DWORD writable = attributes & ~FILE_ATTRIBUTE_READONLY;
        if (!SetFileAttributesW(file.c_str(), writable ? writable : FILE_ATTRIBUTE_NORMAL)) {
            throw std::runtime_error("Cannot make the migrated MicroSIP Custom profile writable.");
        }
    }
}

void RemoveStagedFile(const std::wstring& file)
{
    if (Exists(file)) MakeStagedFileWritable(file);
    if (!DeleteFileW(file.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        throw std::runtime_error("Cannot clean up an interrupted MicroSIP Custom profile migration.");
    }
}

void CleanStaging(const std::wstring& stage)
{
    for (const auto* name : kDataFiles) RemoveStagedFile(Join(stage, name));
    RemoveStagedFile(Join(stage, L"microsip.ini"));
    RemoveStagedFile(Join(stage, L"ready"));
    // Only application-owned files are removed. Unknown files are preserved.
    RemoveDirectoryW(stage.c_str());
}

bool Publish(const std::wstring& stage, const std::wstring& destination, const wchar_t* name)
{
    const std::wstring source = Join(stage, name);
    if (!Exists(source)) return false;
    if (!MoveFileExW(source.c_str(), Join(destination, name).c_str(), MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) {
            throw std::runtime_error("Cannot finish the MicroSIP Custom profile migration. Original data is preserved; try again.");
        }
        RemoveStagedFile(source); // an existing shared file always wins
        return false;
    }
    return true;
}

void PreserveRecordingPath(const std::wstring& stagedIni, const std::wstring& oldDirectory)
{
    std::vector<wchar_t> buffer(32768);
    GetPrivateProfileStringW(L"Settings", L"recordingPath", L"Recordings", buffer.data(),
        static_cast<DWORD>(buffer.size()), stagedIni.c_str());
    const std::wstring recording(buffer.data());
    // Keep recordings in place. In particular, a new working/executable folder
    // must not redirect a legacy relative path to an empty recordings folder.
    if (!IsAbsolute(recording)) {
        const std::wstring absolute = FullPath(Join(oldDirectory, recording));
        if (!WritePrivateProfileStringW(L"Settings", L"recordingPath", absolute.c_str(), stagedIni.c_str())) {
            throw std::runtime_error("Cannot preserve the existing MicroSIP Custom recording directory.");
        }
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, stagedIni.c_str());
    }
}

bool Migrate(const std::wstring& oldDirectory, const std::wstring& oldIni, const Paths& paths)
{
    const std::wstring stage = Join(paths.roaming, kMigrationDirectory);
    const std::wstring ready = Join(stage, L"ready");
    // A completed stage is sufficient to recover after a crash, even when the
    // next executable lives elsewhere and the old portable folder is absent.
    if (!Exists(ready)) {
        if (!Exists(oldIni)) return false;
        EnsureDirectory(stage);
        CleanStaging(stage);
        EnsureDirectory(stage);
        std::vector<std::pair<std::wstring, std::wstring>> files;
        files.emplace_back(oldIni, L"microsip.ini");
        for (const auto* name : kDataFiles) {
            const std::wstring source = Join(oldDirectory, name);
            if (!Exists(source) || Exists(Join(paths.roaming, name))) continue;
            // Never attach a source WAL/journal to an existing shared database.
            const std::wstring fileName(name);
            const size_t sidecar = fileName.find(L".db-");
            if (sidecar != std::wstring::npos) {
                const std::wstring databaseName = fileName.substr(0, sidecar + 3);
                if (!Exists(Join(oldDirectory, databaseName)) || Exists(Join(paths.roaming, databaseName))) continue;
            }
            files.emplace_back(source, name);
        }
        SourceLocks locks;
        // Acquire every source lock before copying the first byte.
        for (const auto& file : files) locks.Add(file.first);
        for (const auto& file : files) {
            const std::wstring destination = Join(stage, file.second);
            if (!CopyFileW(file.first.c_str(), destination.c_str(), TRUE)) {
                throw std::runtime_error("Cannot copy the existing MicroSIP Custom data. Original data is preserved; check free space and folder permissions.");
            }
            // CopyFile preserves source attributes. Old exported/read-only
            // files must not prevent adjusting the staged recording path or
            // leave the new profile unable to save settings after migration.
            MakeStagedFileWritable(destination);
            if (file.second == L"microsip.ini") PreserveRecordingPath(destination, oldDirectory);
            Flush(destination);
        }
        HANDLE marker = CreateFileW(ready.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (marker == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot stage the MicroSIP Custom profile migration.");
        const BOOL flushed = FlushFileBuffers(marker);
        CloseHandle(marker);
        if (!flushed) throw std::runtime_error("Cannot flush the MicroSIP Custom migration marker.");
    }
    for (const auto* name : kDataFiles) Publish(stage, paths.roaming, name);
    // Only this final rename makes the shared profile initialized. All data is
    // staged first, so interruption can be resumed instead of creating a blank
    // or truncated INI that hides the original accounts.
    const bool migrated = Publish(stage, paths.roaming, L"microsip.ini");
    CleanStaging(stage);
    return migrated;
}
}

Lease::~Lease()
{
    ReleaseMutex(mutex_);
    CloseHandle(mutex_);
}

Paths Resolve(const std::wstring& executable, const std::wstring& roamingRoot,
    const std::wstring& localRoot, const std::wstring& explicitDirectory)
{
    const std::wstring exe = FullPath(executable);
    const size_t separator = exe.find_last_of(L"\\/");
    const std::wstring directory = exe.substr(0, separator);
    std::wstring stem = exe.substr(separator + 1);
    const size_t extension = stem.find_last_of(L'.');
    if (extension != std::wstring::npos) stem.resize(extension);
    Paths paths;
    paths.portable = explicitDirectory.empty() && Exists(Join(directory, L"microsip.portable"));
    if (!explicitDirectory.empty()) {
        paths.roaming = paths.local = FullPath(explicitDirectory);
        paths.ini = Join(paths.roaming, L"microsip.ini");
    }
    else if (paths.portable) {
        paths.roaming = paths.local = directory;
        paths.ini = Join(directory, stem + L".ini");
    }
    else {
        // A failed shell-folder lookup must never silently fall back to the
        // executable folder or to the official application's profile.
        paths.roaming = Join(FullPath(roamingRoot), L"MicroSIP Custom");
        paths.local = Join(FullPath(localRoot), L"MicroSIP Custom");
        paths.ini = Join(paths.roaming, L"microsip.ini");
    }
    paths.log = Join(paths.local, L"microsip_log.txt");
    paths.identity = ProfileIdentity(paths.ini);
    HANDLE mutex = CreateMutexW(nullptr, FALSE, paths.identity.c_str());
    if (!mutex) throw std::runtime_error("Cannot protect the MicroSIP Custom profile from another running copy.");
    const DWORD acquired = WaitForSingleObject(mutex, 0);
    if (acquired == WAIT_TIMEOUT) {
        CloseHandle(mutex);
        paths.inUse = true;
        return paths;
    }
    if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED) {
        CloseHandle(mutex);
        throw std::runtime_error("Cannot acquire the MicroSIP Custom profile lock.");
    }
    paths.lease = std::make_shared<Lease>(mutex);
    EnsureDirectory(paths.roaming);
    EnsureDirectory(paths.local);
    if (!paths.portable && !Exists(paths.ini)) {
        paths.migrated = Migrate(directory, Join(directory, stem + L".ini"), paths);
    }
    paths.roaming += L"\\";
    paths.local += L"\\";
    return paths;
}

void InitializeUpdateChannel(const std::wstring& ini)
{
    wchar_t channel[64] = {};
    GetPrivateProfileStringW(L"Global", L"customUpdateChannel", L"", channel, 64, ini.c_str());
    if (_wcsicmp(channel, L"github") == 0) return;
    wchar_t interval[64] = {};
    GetPrivateProfileStringW(L"Settings", L"updatesInterval", L"", interval, 64, ini.c_str());
    if (!*interval || _wcsicmp(interval, L"never") == 0) {
        if (!WritePrivateProfileStringW(L"Settings", L"updatesInterval", L"daily", ini.c_str())) {
            throw std::runtime_error("Cannot initialize MicroSIP Custom update preferences.");
        }
    }
    if (!WritePrivateProfileStringW(L"Global", L"checkUpdatesTime", L"0", ini.c_str()) ||
        !WritePrivateProfileStringW(L"Global", L"customUpdateChannel", L"github", ini.c_str())) {
        throw std::runtime_error("Cannot initialize MicroSIP Custom update preferences.");
    }
}
}
