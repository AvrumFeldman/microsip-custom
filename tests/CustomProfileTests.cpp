// SPDX-License-Identifier: GPL-2.0-or-later
#include "../CustomProfile.h"

#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void Expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::wstring Join(const std::wstring& directory, const std::wstring& name)
{
    return directory + (directory.empty() || directory.back() == L'\\' ? L"" : L"\\") + name;
}

void Directory(const std::wstring& path)
{
    Expect(CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS, "create fixture directory");
}

bool Exists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void Bytes(const std::wstring& file, const std::string& data)
{
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Expect(handle != INVALID_HANDLE_VALUE, "create synthetic fixture file");
    DWORD written = 0;
    const BOOL ok = WriteFile(handle, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(handle);
    Expect(ok && written == data.size(), "write synthetic fixture file");
}

std::string ReadBytes(const std::wstring& file)
{
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Expect(handle != INVALID_HANDLE_VALUE, "open synthetic fixture file");
    const DWORD length = GetFileSize(handle, nullptr);
    std::string result(length, '\0');
    DWORD read = 0;
    const BOOL ok = ReadFile(handle, result.empty() ? nullptr : &result[0], length, &read, nullptr);
    CloseHandle(handle);
    Expect(ok && read == length, "read synthetic fixture file");
    return result;
}

void Ini(const std::wstring& file, const wchar_t* section, const wchar_t* key, const wchar_t* value)
{
    if (!Exists(file)) Bytes(file, std::string("\xff\xfe", 2));
    Expect(WritePrivateProfileStringW(section, key, value, file.c_str()) != FALSE, "write synthetic INI value");
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, file.c_str());
}

std::wstring Value(const std::wstring& file, const wchar_t* section, const wchar_t* key)
{
    wchar_t value[32768] = {};
    GetPrivateProfileStringW(section, key, L"", value, 32768, file.c_str());
    return value;
}

void CleanTree(const std::wstring& directory, const std::wstring& allowedRoot)
{
    Expect(directory == allowedRoot || (directory.size() > allowedRoot.size() &&
        directory.compare(0, allowedRoot.size(), allowedRoot) == 0 && directory[allowedRoot.size()] == L'\\'),
        "cleanup must stay within the unique temporary test directory");
    WIN32_FIND_DATAW data;
    HANDLE search = FindFirstFileW(Join(directory, L"*").c_str(), &data);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
            const std::wstring file = Join(directory, data.cFileName);
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) RemoveDirectoryW(file.c_str());
                else CleanTree(file, allowedRoot);
            }
            else {
                SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(file.c_str());
            }
        } while (FindNextFileW(search, &data));
        FindClose(search);
    }
    RemoveDirectoryW(directory.c_str());
}

struct Fixture {
    std::wstring root, roaming, local, old, executable, ini;
    Fixture()
    {
        wchar_t temp[MAX_PATH] = {};
        Expect(GetTempPathW(MAX_PATH, temp) != 0, "temporary test root available");
        static unsigned sequence = 0;
        root = Join(temp, L"MicroSIPCustomProfileTests-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence));
        Directory(root);
        roaming = Join(root, L"Roaming");
        local = Join(root, L"Local");
        old = Join(root, L"Old custom build");
        Directory(roaming);
        Directory(local);
        Directory(old);
        executable = Join(old, L"microsip.exe");
        ini = Join(old, L"microsip.ini");
    }
    ~Fixture() { try { CleanTree(root, root); } catch (...) {} }
    void Seed()
    {
        Ini(ini, L"Settings", L"accountId", L"1");
        Ini(ini, L"Settings", L"callAudioMode", L"2");
        Ini(ini, L"Settings", L"startupMigrationDeclined", L"synthetic startup entry");
        Ini(ini, L"Account1", L"username", L"synthetic-profile-user");
        Ini(ini, L"Account1", L"password", L"opaque-custom-encrypted-sentinel");
    }
};

void DefaultsAndPortable()
{
    Fixture f;
    Directory(Join(f.roaming, L"MicroSIP"));
    const std::wstring official = Join(Join(f.roaming, L"MicroSIP"), L"microsip.ini");
    Ini(official, L"Account1", L"username", L"must-never-be-imported");
    const std::string before = ReadBytes(official);
    {
        auto p = CustomProfile::Resolve(f.executable, f.roaming, f.local);
        Expect(!p.portable && !p.migrated && !p.inUse && p.lease, "new builds default to shared profile");
        Expect(p.ini == Join(Join(f.roaming, L"MicroSIP Custom"), L"microsip.ini"), "stable custom roaming INI");
        Expect(p.log == Join(Join(f.local, L"MicroSIP Custom"), L"microsip_log.txt"), "stable custom local logs");
        Expect(!Exists(p.ini) && !Exists(f.ini), "path selection does not create an empty INI");
        Expect(ReadBytes(official) == before, "official MicroSIP profile untouched");
    }
    Bytes(Join(f.old, L"microsip.portable"), "");
    f.Seed();
    {
        auto p = CustomProfile::Resolve(f.executable, L"", L"");
        Expect(p.portable && p.ini == f.ini && !p.migrated && p.lease, "explicit marker keeps portable account beside executable");
        Expect(Value(f.ini, L"Account1", L"username") == L"synthetic-profile-user", "portable account unchanged");
    }
    bool rejected = false;
    DeleteFileW(Join(f.old, L"microsip.portable").c_str());
    try { CustomProfile::Resolve(f.executable, L"", f.local); }
    catch (const std::exception&) { rejected = true; }
    Expect(rejected, "missing AppData root must fail instead of silently using official or portable data");
    std::cout << "PASS: stable custom profile by default; explicit portable marker; official data untouched\n";
}

void MigrationAndLaterBuilds()
{
    Fixture f;
    f.Seed();
    Ini(f.ini, L"Settings", L"recordingPath", L"My recordings");
    const std::vector<std::wstring> files = { L"Contacts.xml", L"contacts.db", L"contacts.db-wal",
        L"contacts.db-journal", L"call_log.db", L"call_log.db-wal", L"call_log.db-journal" };
    for (size_t i = 0; i < files.size(); ++i) Bytes(Join(f.old, files[i]), "synthetic-data-" + std::to_string(i));
    Bytes(Join(f.old, L"contacts.db-shm"), "transient shared memory is not a portable database snapshot");
    const std::string originalIni = ReadBytes(f.ini);
    std::wstring sharedIni, sharedDirectory, identity;
    {
        auto p = CustomProfile::Resolve(f.executable, f.roaming, f.local);
        Expect(p.migrated && !p.portable, "first shared launch migrates adjacent custom profile");
        sharedIni = p.ini;
        sharedDirectory = p.roaming;
        identity = p.identity;
        Expect(Value(p.ini, L"Account1", L"password") == L"opaque-custom-encrypted-sentinel", "password ciphertext preserved without conversion");
        Expect(Value(p.ini, L"Settings", L"callAudioMode") == L"2", "custom audio settings preserved");
        Expect(Value(p.ini, L"Settings", L"startupMigrationDeclined") == L"synthetic startup entry", "startup choices preserved");
        Expect(Value(p.ini, L"Settings", L"recordingPath") == Join(f.old, L"My recordings"), "relative recordings remain in original directory");
        for (const auto& name : files) Expect(ReadBytes(Join(p.roaming, name)) == ReadBytes(Join(f.old, name)), "contacts/history and recovery sidecars copied intact");
        Expect(!Exists(Join(p.roaming, L"contacts.db-shm")), "transient SQLite SHM is rebuilt");
        Expect(ReadBytes(f.ini) == originalIni, "migration never changes the original custom INI");
    }
    const std::string sharedBefore = ReadBytes(sharedIni);
    const std::wstring later = Join(f.root, L"Later release");
    Directory(later);
    Ini(Join(later, L"microsip.ini"), L"Settings", L"accountId", L"0");
    {
        auto p = CustomProfile::Resolve(Join(later, L"microsip.exe"), f.roaming, f.local);
        Expect(!p.migrated && p.ini == sharedIni && p.identity == identity, "a later release reuses the same profile and lock");
        Expect(ReadBytes(p.ini) == sharedBefore, "release defaults never replace existing shared accounts");
        Expect(ReadBytes(Join(p.roaming, L"call_log.db")) == ReadBytes(Join(f.old, L"call_log.db")), "history persists across executable locations");
    }
    std::cout << "PASS: one-time migration preserves settings, encrypted passwords, contacts, history and recording paths across releases\n";
}

void AlternatePersistentDirectory()
{
    Fixture f;
    // A supplied alternate profile exercises shared-mode behavior without
    // consulting or writing the caller's actual Windows known folders.
    const std::wstring chosen = Join(f.root, L"Chosen persistent profile");
    Bytes(Join(f.old, L"microsip.portable"), "");
    std::wstring identity;
    {
        auto p = CustomProfile::Resolve(f.executable, L"", L"", chosen);
        Expect(!p.portable && !p.migrated && p.lease, "alternate directory explicitly selects persistent mode");
        Expect(p.ini == Join(chosen, L"microsip.ini") && p.roaming == chosen + L"\\" && p.local == p.roaming,
            "alternate profile uses only its chosen directory");
        CustomProfile::InitializeUpdateChannel(p.ini);
        Expect(Value(p.ini, L"Settings", L"updatesInterval") == L"daily", "fresh alternate profile gets shared-mode update defaults");
        identity = p.identity;
    }
    const std::wstring later = Join(f.root, L"Other build");
    Directory(later);
    {
        auto p = CustomProfile::Resolve(Join(later, L"microsip.exe"), L"", L"", chosen);
        Expect(!p.portable && p.identity == identity && !p.migrated, "alternate persistent profile survives executable changes");
    }
    bool rejected = false;
    try { CustomProfile::Resolve(f.executable, f.roaming, f.local, L"relative-profile"); }
    catch (const std::exception&) { rejected = true; }
    Expect(rejected, "explicit directory must be absolute even when portable marker exists");
    f.Seed();
    const std::wstring migrated = Join(f.root, L"Migrated alternate profile");
    auto p = CustomProfile::Resolve(f.executable, L"", L"", migrated);
    Expect(p.migrated && Value(p.ini, L"Account1", L"username") == L"synthetic-profile-user",
        "alternate profile uses normal adjacent custom-data migration");
    std::cout << "PASS: explicit alternate persistent profile ignores portable marker, stays isolated and migrates normally\n";
}

void BusyProfilesAndSourceLocks()
{
    Fixture f;
    f.Seed();
    Bytes(Join(f.old, L"call_log.db"), "synthetic history");
    HANDLE writer = CreateFileW(Join(f.old, L"call_log.db").c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Expect(writer != INVALID_HANDLE_VALUE, "open synthetic old app database handle");
    bool refused = false;
    try { CustomProfile::Resolve(f.executable, f.roaming, f.local); }
    catch (const std::exception&) { refused = true; }
    CloseHandle(writer);
    Expect(refused, "live old custom database must block migration");
    Expect(!Exists(Join(Join(f.roaming, L"MicroSIP Custom"), L"microsip.ini")), "failed migration never publishes a blank or partial shared INI");
    auto p = CustomProfile::Resolve(f.executable, f.roaming, f.local);
    Expect(p.migrated, "migration succeeds once old app database is closed");
    const std::string before = ReadBytes(p.ini);
    bool inUse = false;
    std::thread duplicate([&] {
        auto other = CustomProfile::Resolve(f.executable, f.roaming, f.local);
        inUse = other.inUse && !other.lease && other.identity == p.identity;
    });
    duplicate.join();
    Expect(inUse && ReadBytes(p.ini) == before, "second process/thread cannot write an in-use profile");
    std::cout << "PASS: profile lease prevents concurrent owners and live-source migration fails without publishing data\n";
}

void ReadOnlyLegacyFiles()
{
    Fixture f;
    f.Seed();
    const std::wstring contacts = Join(f.old, L"Contacts.xml");
    Bytes(contacts, "synthetic readonly legacy contacts");
    Expect(SetFileAttributesW(f.ini.c_str(), FILE_ATTRIBUTE_READONLY) &&
        SetFileAttributesW(contacts.c_str(), FILE_ATTRIBUTE_READONLY), "make synthetic legacy files read-only");
    const std::string originalIni = ReadBytes(f.ini);
    auto p = CustomProfile::Resolve(f.executable, f.roaming, f.local);
    Expect(p.migrated, "read-only legacy source files should migrate into a writable shared profile");
    Expect(ReadBytes(Join(p.roaming, L"Contacts.xml")) == "synthetic readonly legacy contacts", "readonly contacts copied intact");
    Expect(ReadBytes(f.ini) == originalIni &&
        (GetFileAttributesW(f.ini.c_str()) & FILE_ATTRIBUTE_READONLY), "readonly source INI content and attributes preserved");
    Expect(!(GetFileAttributesW(p.ini.c_str()) & FILE_ATTRIBUTE_READONLY), "new shared INI remains writable");
    std::cout << "PASS: read-only legacy sources migrate without changing originals or making the new profile read-only\n";
}

void InterruptedMigrationAndExistingData()
{
    Fixture f;
    Directory(Join(f.roaming, L"MicroSIP Custom"));
    const std::wstring destination = Join(f.roaming, L"MicroSIP Custom");
    const std::wstring stage = Join(destination, L".custom-profile-migration");
    Directory(stage);
    Ini(Join(stage, L"microsip.ini"), L"Account1", L"username", L"staged-recoverable-user");
    Bytes(Join(destination, L"call_log.db"), "already-published-database");
    Bytes(Join(stage, L"call_log.db-wal"), "not-yet-published-recovery-data");
    Bytes(Join(stage, L"contacts.db"), "staged-contacts");
    Bytes(Join(stage, L"ready"), "");
    {
        auto p = CustomProfile::Resolve(f.executable, f.roaming, f.local);
        Expect(p.migrated && Value(p.ini, L"Account1", L"username") == L"staged-recoverable-user", "completed stage resumes without original portable folder");
        Expect(ReadBytes(Join(destination, L"call_log.db-wal")) == "not-yet-published-recovery-data", "interrupted database/WAL publication completes before INI");
        Expect(ReadBytes(Join(destination, L"contacts.db")) == "staged-contacts", "interrupted contacts migration completes");
        Expect(!Exists(stage), "completed migration stage cleaned up");
    }
    Fixture partial;
    partial.Seed();
    const std::wstring partialDestination = Join(partial.roaming, L"MicroSIP Custom");
    Directory(partialDestination);
    const std::wstring partialStage = Join(partialDestination, L".custom-profile-migration");
    Directory(partialStage);
    Bytes(Join(partialStage, L"microsip.ini"), "truncated incomplete copy");
    Bytes(Join(partialDestination, L"contacts.db"), "pre-existing-shared-contacts");
    Bytes(Join(partial.old, L"contacts.db"), "old-portable-contacts");
    Bytes(Join(partial.old, L"contacts.db-wal"), "unrelated-to-existing-shared-database");
    auto p = CustomProfile::Resolve(partial.executable, partial.roaming, partial.local);
    Expect(p.migrated && Value(p.ini, L"Account1", L"username") == L"synthetic-profile-user", "unready incomplete stage is recreated from intact source");
    Expect(ReadBytes(Join(p.roaming, L"contacts.db")) == "pre-existing-shared-contacts", "existing shared contacts never overwritten");
    Expect(!Exists(Join(p.roaming, L"contacts.db-wal")), "source WAL never attaches to an unrelated shared database");
    std::cout << "PASS: interrupted migration resumes safely and existing shared data always wins\n";
}

void UpdateChannelDefaults()
{
    Fixture fresh;
    Bytes(fresh.ini, std::string("\xff\xfe", 2));
    CustomProfile::InitializeUpdateChannel(fresh.ini);
    Expect(Value(fresh.ini, L"Settings", L"updatesInterval") == L"daily" &&
        Value(fresh.ini, L"Global", L"checkUpdatesTime") == L"0" &&
        Value(fresh.ini, L"Global", L"customUpdateChannel") == L"github",
        "a fresh account-free profile initializes daily GitHub checks without a packaged INI seed");
    Expect(Value(fresh.ini, L"Settings", L"accountId").empty(), "fresh channel defaults never create a SIP account");
    Fixture f;
    f.Seed();
    Ini(f.ini, L"Settings", L"updatesInterval", L"never");
    Ini(f.ini, L"Global", L"checkUpdatesTime", L"12345");
    CustomProfile::InitializeUpdateChannel(f.ini);
    Expect(Value(f.ini, L"Settings", L"updatesInterval") == L"daily", "legacy official-update suppression converted once");
    Expect(Value(f.ini, L"Global", L"customUpdateChannel") == L"github" &&
        Value(f.ini, L"Global", L"checkUpdatesTime") == L"0", "GitHub channel marker and fresh check timestamp");
    Ini(f.ini, L"Settings", L"updatesInterval", L"never");
    Ini(f.ini, L"Global", L"checkUpdatesTime", L"67890");
    CustomProfile::InitializeUpdateChannel(f.ini);
    Expect(Value(f.ini, L"Settings", L"updatesInterval") == L"never" &&
        Value(f.ini, L"Global", L"checkUpdatesTime") == L"67890", "later explicit opt-out and timestamp persist across builds");
    Ini(f.ini, L"Global", L"customUpdateChannel", nullptr);
    Ini(f.ini, L"Settings", L"updatesInterval", L"weekly");
    CustomProfile::InitializeUpdateChannel(f.ini);
    Expect(Value(f.ini, L"Settings", L"updatesInterval") == L"weekly", "existing non-never update interval preserved");
    std::cout << "PASS: GitHub update channel initialized once while later user choices persist\n";
}
}

int main()
{
    try {
        DefaultsAndPortable();
        MigrationAndLaterBuilds();
        AlternatePersistentDirectory();
        BusyProfilesAndSourceLocks();
        ReadOnlyLegacyFiles();
        InterruptedMigrationAndExistingData();
        UpdateChannelDefaults();
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
