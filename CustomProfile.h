// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <windows.h>
#include <memory>
#include <string>

namespace CustomProfile {
// The owning UI thread keeps this lease until process exit. Separate executable
// locations share a lease when they use the same profile.
class Lease {
public:
    explicit Lease(HANDLE mutex) : mutex_(mutex) {}
    ~Lease();
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
private:
    HANDLE mutex_;
};

struct Paths {
    std::wstring roaming;
    std::wstring local;
    std::wstring ini;
    std::wstring log;
    std::wstring identity;
    bool portable = false;
    bool migrated = false;
    bool inUse = false;
    std::shared_ptr<Lease> lease;
};

// All filesystem roots are explicit so tests never access a user's profile.
// Default: shared per-user MicroSIP Custom data. An adjacent microsip.portable
// marker explicitly selects a self-contained profile. Acquire the profile
// lease before creating/migrating files; inUse returns without modifying data.
// An explicit directory selects an alternate persistent profile and takes
// precedence over a portable marker; roaming/local data both live there.
Paths Resolve(const std::wstring& executable, const std::wstring& roamingRoot,
    const std::wstring& localRoot, const std::wstring& explicitDirectory = L"");

// Switch the legacy official-update opt-out to the custom GitHub channel once.
// Later user choices (including never) survive subsequent application builds.
void InitializeUpdateChannel(const std::wstring& ini);
}
