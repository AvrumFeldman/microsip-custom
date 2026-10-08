// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AUDIO_FOCUS_STANDALONE
#include "stdafx.h"
#endif
#include "AudioFocus.h"
#include <vector>

static std::wstring QuoteArgument(const std::wstring& value)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += ch;
    }
    result.append(slashes * 2, L'\\');
    return result + L"\"";
}

AudioFocus::~AudioFocus()
{
    Stop();
    if (process_) CloseHandle(process_);
}

void AudioFocus::Stop()
{
    if (pipe_) { CloseHandle(pipe_); pipe_ = nullptr; }
    // Never terminate the guard: it must finish restoring its sessions.
    if (process_ && WaitForSingleObject(process_, 1000) == WAIT_OBJECT_0) {
        CloseHandle(process_);
        process_ = nullptr;
    }
}

bool AudioFocus::Update(bool active, int mode, const std::wstring& apps)
{
    if (!active || mode < 1 || mode > 2) { Stop(); return true; }
    if (process_ && WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) {
        CloseHandle(process_);
        process_ = nullptr;
        if (pipe_) { CloseHandle(pipe_); pipe_ = nullptr; }
    }
    if (pipe_ && (mode_ != mode || apps_ != apps)) Stop();
    if (process_) return true; // including a guard still finishing restoration
    return Start(mode, apps);
}

bool AudioFocus::Start(int mode, const std::wstring& apps)
{
    wchar_t module[32768];
    const DWORD length = GetModuleFileNameW(nullptr, module, _countof(module));
    if (!length || length >= _countof(module)) return false;
    std::wstring exe(module, length);
    exe.resize(exe.find_last_of(L"\\/") + 1);
    exe += L"MicroSIPAudioGuard.exe";

    SECURITY_ATTRIBUTES security = { sizeof(security), nullptr, TRUE };
    HANDLE reader = nullptr;
    if (!CreatePipe(&reader, &pipe_, &security, 0)) return false;
    if (!SetHandleInformation(pipe_, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(reader); Stop(); return false;
    }

    // Only the read end is inherited; no SIP/network/file handles leak.
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<BYTE> storage(size);
    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size)) {
        CloseHandle(reader); Stop(); return false;
    }
    BOOL created = FALSE;
    if (UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
        &reader, sizeof(reader), nullptr, nullptr)) {
        std::wstring command = QuoteArgument(exe) + L" " +
            std::to_wstring(reinterpret_cast<UINT_PTR>(reader)) + L" " +
            std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(mode) + L" " + QuoteArgument(apps);
        PROCESS_INFORMATION process = {};
        created = CreateProcessW(exe.c_str(), &command[0], nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &process);
        if (created) {
            process_ = process.hProcess;
            CloseHandle(process.hThread);
            mode_ = mode;
            apps_ = apps;
        }
    }
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    CloseHandle(reader);
    if (!created) Stop();
    return created != FALSE;
}
