#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run a portable test copy and UI regression on a private Windows desktop.

Usage: python tests/run_isolated_desktop.py --app build/FIXTURE/microsip.exe \
           --driver tests/smoke_call_waiting.py [--timeout 180]

Use a separate Windows session or VM for UI regression tests. A dedicated
desktop can still affect focus or show UI elsewhere in the same session.
Prepare an account-free portable fixture under build first. The driver receives
the test application's PID as its first argument. It closes its own app on exit, then
removes its disposable executable/helper and matching symbols. Use
--keep-binaries to retain those files for diagnostics; data and logs remain.
"""
import argparse
import configparser
import ctypes as c
from ctypes import wintypes as w
import os
import pathlib
import runpy
import stat
import subprocess
import sys
import time
import uuid


def validated_fixture_path(app, root, require_executable=True):
    app = pathlib.Path(os.path.abspath(app))
    if not app.is_relative_to(root / 'build') or (require_executable and app.suffix.lower() != '.exe'):
        raise ValueError('Test application must be an executable under build/')
    # Inspect the lexical path before resolving it, so a junction cannot hide
    # an escape into an installed copy or another folder during cleanup.
    for component in (app, *app.parents):
        info = component.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
            raise ValueError('Test fixture cannot contain symlinks or reparse points')
    if not app.is_file() or app.resolve() != app:
        raise ValueError('Test application must be an existing regular file')
    return app


def running_fixture_binaries(app):
    """Return executable basenames still running, or unidentifiable, at this path."""
    kernel = c.WinDLL('kernel32', use_last_error=True)

    class ProcessEntry(c.Structure):
        _fields_ = [('size', w.DWORD), ('usage', w.DWORD), ('pid', w.DWORD),
                    ('heap', c.c_size_t), ('module', w.DWORD), ('threads', w.DWORD),
                    ('parent', w.DWORD), ('priority', w.LONG), ('flags', w.DWORD),
                    ('exe', w.WCHAR * 260)]

    kernel.CreateToolhelp32Snapshot.argtypes = [w.DWORD, w.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = w.HANDLE
    kernel.Process32FirstW.argtypes = [w.HANDLE, c.POINTER(ProcessEntry)]
    kernel.Process32NextW.argtypes = [w.HANDLE, c.POINTER(ProcessEntry)]
    kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    kernel.OpenProcess.restype = w.HANDLE
    kernel.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, c.POINTER(w.DWORD)]
    kernel.CloseHandle.argtypes = [w.HANDLE]
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    if snapshot == c.c_void_p(-1).value:
        raise c.WinError(c.get_last_error())
    names = {app.name.lower(), 'microsipaudioguard.exe'}
    blocked = set()
    entry = ProcessEntry()
    entry.size = c.sizeof(entry)
    try:
        valid = kernel.Process32FirstW(snapshot, c.byref(entry))
        while valid:
            name = entry.exe.lower()
            if name in names:
                process = kernel.OpenProcess(0x1000, False, entry.pid)
                if not process:
                    blocked.add(name)
                else:
                    try:
                        image = c.create_unicode_buffer(32768)
                        length = w.DWORD(len(image))
                        if not kernel.QueryFullProcessImageNameW(process, 0, image, c.byref(length)):
                            blocked.add(name)
                        elif pathlib.Path(image.value) == app.parent / entry.exe:
                            blocked.add(name)
                    finally:
                        kernel.CloseHandle(process)
            valid = kernel.Process32NextW(snapshot, c.byref(entry))
        if c.get_last_error() != 18:  # ERROR_NO_MORE_FILES
            raise c.WinError(c.get_last_error())
    finally:
        kernel.CloseHandle(snapshot)
    return blocked


def cleanup_fixture_binaries(app, root):
    removed = 0
    try:
        validated_fixture_path(app, root)
        blocked = running_fixture_binaries(app)
    except (OSError, ValueError) as error:
        print('Fixture binary cleanup skipped:', error, flush=True)
        return
    for executable in dict.fromkeys((app.name, 'MicroSIPAudioGuard.exe')):
        if executable.lower() in blocked:
            print('Fixture binary cleanup retained running/unidentified executable:', executable, flush=True)
            continue
        stem = pathlib.Path(executable).stem
        for name in (executable, *(stem + suffix for suffix in ('.pdb', '.ilk', '.lib', '.exp'))):
            candidate = app.parent / name
            if not candidate.exists():
                continue
            try:
                # Recheck the parent chain before every individual file removal.
                validated_fixture_path(candidate, root, require_executable=False)
                info = candidate.lstat()
                if not stat.S_ISREG(info.st_mode) or getattr(info, 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
                    raise ValueError('Cleanup target is not a regular file')
                candidate.unlink()  # Windows also refuses a locked/running image.
                removed += 1
            except (OSError, ValueError) as error:
                print('Fixture binary cleanup retained', name, ':', error, flush=True)
                if name == executable:
                    break
    print('Removed disposable fixture binary files:', removed, flush=True)


def validate_fixture(app):
    raw = app.with_suffix('.ini').read_bytes()
    config = configparser.ConfigParser(interpolation=None, strict=False)
    config.read_string(raw.decode('utf-16' if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig'))
    settings = config['Settings']
    mode = settings.getint('callAudioMode', fallback=0)
    if mode not in (0, 2) or (mode == 2 and settings.get('callAudioApps', '').lower() != 'audiofocustests.exe'):
        raise ValueError('Fixture must leave audio unchanged or target only AudioFocusTests.exe')
    for key, value in settings.items():
        if key.startswith('cmd') and value.strip('" '):
            raise ValueError('Fixture cannot run configured external call commands')
    accounts = [config[section] for section in config.sections() if section.lower().startswith('account')]
    if not accounts:
        raise ValueError('Fixture needs an explicit synthetic account')
    for account in accounts:
        if account.get('username') not in ('custom-test', 'registration-test'):
            raise ValueError('Fixture needs a synthetic test username')
        if account.get('password', '').strip('" ') or account.get('proxy', '').strip('" '):
            raise ValueError('Fixture cannot contain credentials or a proxy')
        if account.get('domain', '') not in ('127.0.0.1', 'localhost'):
            raise ValueError('Fixture account must use a loopback domain')
        server = account.get('server', '').strip('" ')
        if server and server.split(':', 1)[0] not in ('127.0.0.1', 'localhost'):
            raise ValueError('Fixture registrar must use loopback')
    source_port = settings.getint('sourcePort', fallback=5096)
    if not 1024 <= source_port <= 65535:
        raise ValueError('Fixture sourcePort must be a nonprivileged UDP port')
    return source_port


def user_api():
    user = c.WinDLL('user32', use_last_error=True)
    user.CreateDesktopW.argtypes = [w.LPCWSTR, w.LPCWSTR, c.c_void_p, w.DWORD,
                                   w.DWORD, c.c_void_p]
    user.CreateDesktopW.restype = w.HANDLE
    user.CloseDesktop.argtypes = [w.HANDLE]
    user.GetForegroundWindow.restype = w.HWND
    user.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
    user.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
    user.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
    user.GetCursorPos.argtypes = [c.POINTER(w.POINT)]
    return user


def app_windows(user, pid):
    callback = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    found = []

    @callback
    def visit(hwnd, unused):
        owner = w.DWORD()
        name = c.create_unicode_buffer(256)
        user.GetWindowThreadProcessId(hwnd, c.byref(owner))
        user.GetClassNameW(hwnd, name, 256)
        if owner.value == pid and name.value == 'MicroSIP':
            found.append(hwnd)
        return True

    user.EnumWindows.argtypes = [callback, w.LPARAM]
    user.EnumWindows(visit, 0)
    return found


def driver_mode():
    # CreateProcess lpDesktop has already assigned this process's first thread
    # to the private desktop, before loading any test that imports user32.
    operation, pid, *arguments = sys.argv[2:]
    user = user_api()
    pid = int(pid)
    if operation == 'close':
        for hwnd in app_windows(user, pid):
            user.PostMessageW(hwnd, 0x111, 32784, 0)  # WM_COMMAND / ID_EXIT
        return
    deadline = time.monotonic() + 20
    while not app_windows(user, pid):
        if time.monotonic() >= deadline:
            raise TimeoutError('Private desktop app did not create its window')
        time.sleep(.1)
    time.sleep(1)  # allow OnCreated to initialize local SIP transports
    driver = pathlib.Path(arguments[0]).resolve()
    sys.path.insert(0, str(driver.parent))
    sys.argv = [str(driver), str(pid), *arguments[1:]]
    runpy.run_path(str(driver), run_name='__main__')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=pathlib.Path, required=True)
    parser.add_argument('--driver', type=pathlib.Path, required=True)
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--keep-binaries', action='store_true',
                        help='retain disposable executables and symbols for diagnostics')
    parser.add_argument('driver_args', nargs='*')
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parent.parent
    try:
        app = validated_fixture_path(args.app, root)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    # Require explicit disposable fixtures; out/microsip.exe may share a user's
    # development profile, so it must be copied under build before testing.
    if not app.is_relative_to(root / 'build') or not app.is_file():
        parser.error('--app must be an existing disposable copy under build/')
    if not app.with_suffix('.ini').is_file():
        parser.error('portable fixture must have an account-free .ini beside the executable')
    driver_env = os.environ.copy()
    driver_env['MICROSIP_TEST_SIP_PORT'] = str(validate_fixture(app))
    # New custom builds share a per-user profile by default. Tests must opt in
    # to isolation explicitly and never initialize or migrate the real profile.
    (app.parent / 'microsip.portable').touch(exist_ok=True)
    kernel = c.WinDLL('kernel32', use_last_error=True)
    kernel.WritePrivateProfileStringW.argtypes = [w.LPCWSTR, w.LPCWSTR, w.LPCWSTR, w.LPCWSTR]
    if not kernel.WritePrivateProfileStringW('Global', 'customUpdateChannel', 'github', str(app.with_suffix('.ini'))):
        raise c.WinError(c.get_last_error())
    driver = args.driver.resolve()
    if not driver.is_file():
        parser.error('--driver does not exist')
    user = user_api()
    desktop_name = 'MicroSIP-Test-' + uuid.uuid4().hex
    # READOBJECTS | CREATEWINDOW | ENUMERATE | WRITEOBJECTS. Deliberately omit
    # SWITCHDESKTOP, JOURNALRECORD and JOURNALPLAYBACK.
    desktop = user.CreateDesktopW(desktop_name, None, None, 0, 0xC3, None)
    if not desktop:
        raise c.WinError(c.get_last_error())
    startup = subprocess.STARTUPINFO()
    startup.lpDesktop = 'WinSta0\\' + desktop_name
    before = user.GetForegroundWindow()
    cursor_before = w.POINT()
    user.GetCursorPos(c.byref(cursor_before))
    app_process = None
    driver_process = None
    result = 1
    try:
        app_process = subprocess.Popen([str(app)], cwd=app.parent, startupinfo=startup)
        driver_process = subprocess.Popen([
            sys.executable, str(pathlib.Path(__file__).resolve()), '--_driver',
            'run', str(app_process.pid), str(driver), *args.driver_args], startupinfo=startup, env=driver_env)
        result = driver_process.wait(timeout=args.timeout)
    finally:
        try:
            if driver_process and driver_process.poll() is None:
                driver_process.terminate()
                driver_process.wait(timeout=5)
            if app_process and app_process.poll() is None:
                subprocess.run([
                    sys.executable, str(pathlib.Path(__file__).resolve()), '--_driver',
                    'close', str(app_process.pid)], startupinfo=startup, timeout=10, check=False)
                try:
                    app_process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    app_process.terminate()
                    app_process.wait(timeout=5)
        finally:
            user.CloseDesktop(desktop)
            if not args.keep_binaries and (app_process is None or app_process.poll() is not None):
                cleanup_fixture_binaries(app, root)
    after = user.GetForegroundWindow()
    cursor_after = w.POINT()
    user.GetCursorPos(c.byref(cursor_after))
    # User activity can naturally change these during longer tests. Report
    # observations, without treating user input as a test failure.
    print('Private desktop closed; visible foreground unchanged:', before == after,
          '; cursor unchanged:', (cursor_before.x, cursor_before.y) ==
          (cursor_after.x, cursor_after.y), flush=True)
    return result


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--_driver':
        driver_mode()
    else:
        raise SystemExit(main())
