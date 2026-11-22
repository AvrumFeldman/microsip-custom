# SPDX-License-Identifier: GPL-2.0-or-later
"""Full nonportable startup on a private desktop and disposable explicit profile.

Usage: python tests/smoke_default_startup.py --app build/CANDIDATE/microsip.exe
Fresh startup begins without any INI, portable marker or GitHub channel seed.
The migrated fixture contains only loopback identities and synthetic databases.
The supported /profile-directory option keeps all application data below build.
No installed profile, actual SIP credentials, outside call or visible desktop
is used. Automatic GitHub checking remains enabled to exercise normal startup.
Disposable executables/debug binaries are removed after success/failure; pass
--keep-binaries only when needed for diagnosis. Profile data/reports are kept.
"""
import argparse
import configparser
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
from pathlib import Path
import shutil
import sqlite3
import stat
import subprocess
import sys
import time
import uuid
from smoke_custom_update import cleanup_binaries

REPO = Path(__file__).resolve().parents[1]


def api():
    user = c.WinDLL('user32', use_last_error=True)
    user.CreateDesktopW.argtypes = [w.LPCWSTR, w.LPCWSTR, c.c_void_p, w.DWORD, w.DWORD, c.c_void_p]
    user.CreateDesktopW.restype = w.HANDLE
    user.CloseDesktop.argtypes = [w.HANDLE]
    user.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
    user.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
    user.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
    user.GetForegroundWindow.restype = w.HWND
    user.GetCursorPos.argtypes = [c.POINTER(w.POINT)]
    user.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
    user.SendMessageTimeoutW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM, w.UINT, w.UINT, c.POINTER(c.c_size_t)]
    user.SendMessageTimeoutW.restype = w.LPARAM
    return user


def windows(user, pid):
    callback = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    found = []

    @callback
    def visit(hwnd, unused):
        owner = w.DWORD()
        user.GetWindowThreadProcessId(hwnd, c.byref(owner))
        if owner.value == pid:
            kind, title = c.create_unicode_buffer(256), c.create_unicode_buffer(2048)
            user.GetClassNameW(hwnd, kind, len(kind))
            user.GetWindowTextW(hwnd, title, len(title))
            found.append({'hwnd': int(hwnd), 'class': kind.value, 'text': title.value})
        return True

    user.EnumWindows.argtypes = [callback, w.LPARAM]
    user.EnumChildWindows.argtypes = [w.HWND, callback, w.LPARAM]
    user.EnumWindows(visit, 0)
    for item in found.copy():
        user.EnumChildWindows(item['hwnd'], visit, 0)
    return found


def probe(pid, output):
    user = api()
    started = time.monotonic()
    main = None
    snapshots = []
    try:
        while time.monotonic() - started < 20:
            scene = windows(user, pid)
            snapshots = scene
            main = next((item['hwnd'] for item in scene if item['class'] == 'MicroSIP'), None)
            # Decline only the known startup replacement/update prompts in this
            # synthetic process. A refusal never changes the actual Run entry.
            text = '\n'.join(item['text'] for item in scene)
            if 'Windows is set to start another MicroSIP copy' in text or ('is available from GitHub.' in text and 'Download, verify' in text):
                for item in scene:
                    if item['class'] == '#32770':
                        user.PostMessageW(item['hwnd'], 0x111, 7, 0)  # IDNO
            if main:
                result = c.c_size_t()
                if not user.SendMessageTimeoutW(main, 0, 0, 0, 3, 2000, c.byref(result)):
                    raise AssertionError('Main window stopped responding during startup')
                if time.monotonic() - started >= 4:
                    break
            time.sleep(.1)
        if not main:
            raise AssertionError('No main window; test-process windows: ' + json.dumps(snapshots))
        # Keep pumping responsiveness checks while the default automatic update
        # worker runs or fails, then close only this isolated process's window.
        for _ in range(15):
            result = c.c_size_t()
            if not user.SendMessageTimeoutW(main, 0, 0, 0, 3, 2000, c.byref(result)):
                raise AssertionError('Automatic update checking blocked the UI')
            time.sleep(.1)
        Path(output).write_text(json.dumps({'success': True, 'seconds': time.monotonic() - started,
                                           'windows': windows(user, pid)}, indent=2), encoding='utf-8')
        user.PostMessageW(main, 0x111, 32784, 0)  # WM_COMMAND / ID_EXIT
        return 0
    except Exception as error:
        Path(output).write_text(json.dumps({'success': False, 'error': str(error), 'windows': windows(user, pid)}, indent=2), encoding='utf-8')
        return 1


def read_ini(path):
    raw = path.read_bytes()
    config = configparser.ConfigParser(interpolation=None, strict=False)
    config.read_string(raw.decode('utf-16' if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig'))
    return config


def scenario(candidate, root, name, migrated=False):
    directory = root / name
    application = directory / 'application'
    profile = directory / "selected profile's data"
    application.mkdir(parents=True)
    binary = application / 'microsip.exe'
    shutil.copyfile(candidate, binary)
    shutil.copyfile(candidate.parent / 'MicroSIPAudioGuard.exe', application / 'MicroSIPAudioGuard.exe')
    assert not (application / 'microsip.portable').exists()
    original = {}
    if migrated:
        legacy = ('[Global]\n[Settings]\naccountId=1\nsourcePort=5598\nupdatesInterval=never\n'
                  'enableSTUN=0\nheadsetSupport=0\ncallAudioMode=0\nnetworkChanges=0\nenableLog=1\n'
                  '[Account1]\nusername=custom-test\ndomain=127.0.0.1\nserver=\npassword=\ntransport=udp\n')
        (application / 'microsip.ini').write_text(legacy, encoding='utf-16')
        for name in ('call_log.db', 'contacts.db'):
            db = sqlite3.connect(application / name)
            try:
                db.execute('CREATE TABLE custom_fixture(value TEXT)')
                db.execute("INSERT INTO custom_fixture VALUES ('synthetic retained data')")
                db.commit()
            finally:
                db.close()
        (application / 'Contacts.xml').write_text('<contacts></contacts>', encoding='utf-8')
        for name in ('microsip.ini', 'call_log.db', 'contacts.db', 'Contacts.xml'):
            source = application / name
            original[name] = hashlib.sha256(source.read_bytes()).hexdigest()
            source.chmod(stat.S_IREAD)
    user = api()
    desktop_name = 'MicroSIP-Default-Startup-' + uuid.uuid4().hex
    desktop = user.CreateDesktopW(desktop_name, None, None, 0, 0xC3, 0)
    if not desktop:
        raise c.WinError(c.get_last_error())
    startup = subprocess.STARTUPINFO()
    startup.lpDesktop = 'WinSta0\\' + desktop_name
    output = directory / 'startup-result.json'
    process = None
    driver = None
    try:
        process = subprocess.Popen([str(binary), '/profile-directory', str(profile)], cwd=application, startupinfo=startup)
        driver = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), '--_probe', str(process.pid), str(output)], startupinfo=startup)
        code = driver.wait(timeout=35)
        result = json.loads(output.read_text(encoding='utf-8'))
        assert code == 0 and result['success'], result
        process.wait(timeout=15)
        assert process.returncode == 0, f'Application exited with {process.returncode}'
        config = read_ini(profile / 'microsip.ini')
        assert config.get('Global', 'customUpdateChannel') == 'github'
        assert config.get('Settings', 'updatesInterval') == 'daily'
        if migrated:
            assert config.get('Account1', 'username') == 'custom-test'
            assert not config.get('Account1', 'server', fallback='')
            for name in ('call_log.db', 'contacts.db'):
                db = sqlite3.connect(profile / name)
                try:
                    assert db.execute('SELECT value FROM custom_fixture').fetchone()[0] == 'synthetic retained data'
                finally:
                    db.close()
            for name, digest in original.items():
                assert hashlib.sha256((application / name).read_bytes()).hexdigest() == digest
                assert not ((application / name).stat().st_mode & stat.S_IWRITE)
        else:
            assert not config.has_option('Account1', 'server')
        print('PASS:', directory.name, '(shared mode, daily GitHub checks, responsive UI, graceful exit)', flush=True)
    finally:
        if driver and driver.poll() is None:
            driver.terminate()
            driver.wait(timeout=5)
        if process and process.poll() is None:
            # Failure cleanup is limited to the synthetic process created above.
            process.terminate()
            process.wait(timeout=5)
        user.CloseDesktop(desktop)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--keep-binaries', action='store_true', help='Retain disposable executables/debug files for diagnosis')
    options = parser.parse_args()
    candidate = options.app.resolve()
    assert candidate.is_relative_to(REPO / 'build') and candidate.is_file(), 'Use a disposable candidate under build'
    root = REPO / 'build' / ('default-startup-' + uuid.uuid4().hex)
    root.mkdir()
    try:
        scenario(candidate, root, 'fresh-shared-profile')
        scenario(candidate, root, 'migrated-shared-profile', migrated=True)
        print('PASS: full default/shared startup regressions; artifacts:', root, flush=True)
    finally:
        if not options.keep_binaries:
            cleanup_binaries(root)


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--_probe':
        raise SystemExit(probe(int(sys.argv[2]), sys.argv[3]))
    main()
