# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline updater integration/security regression using disposable fake apps.

Usage: python tests/smoke_custom_update.py
Creates synthetic versioned executables, archives and installations below build;
never reads an installed MicroSIP profile, calls a SIP peer or accesses GitHub.
Requires Windows PowerShell 5.1 and the Windows .NET Framework C# compiler.
Disposable executables/debug binaries are removed even on failure; use
--keep-binaries only when those files are needed for diagnosis.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import os
from pathlib import Path
import struct
import stat
import subprocess
import time
import uuid
import zipfile

REPO = Path(__file__).resolve().parents[1]
ROOT = None
HELPER = REPO / 'updater' / 'Apply-Update.ps1'
POWERSHELL = Path(os.environ['SystemRoot']) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
VERSION = '9.9.9.9'
FLAGS = subprocess.CREATE_NO_WINDOW
OLD = NEW = None
OWNED_WORKERS = []


def cleanup_binaries(root):
    """Remove only binary files below one verified, application-owned fixture.

    Leave all directories, data and reports intact. Never traverse a Windows
    junction/symlink, and leave locked files for a later explicit cleanup.
    This helper is also used by smoke_default_startup.py, without import-time
    fixture creation or compilation.
    """
    build = REPO / 'build'
    root = Path(root).absolute()
    prefixes = ('updater-integration-', 'updater-public-format-', 'default-startup-')
    if root.parent != build or not root.name.startswith(prefixes):
        raise ValueError('Binary cleanup requires an owned fixture directly under repo/build')
    reparse = getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0x400)
    for directory in (build, root):
        info = directory.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & reparse:
            raise ValueError('Binary cleanup refuses a symlink/junction fixture root')
    resolved_root = root.resolve(strict=True)
    if not resolved_root.is_relative_to(build.resolve(strict=True)):
        raise ValueError('Binary cleanup escaped repo/build')
    removed = skipped = 0
    pending = [root]
    while pending:
        directory = pending.pop()
        try:
            info = directory.lstat()
            if (stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & reparse or
                    not directory.resolve(strict=True).is_relative_to(resolved_root)):
                continue
            entries = list(directory.iterdir())
        except OSError:
            continue
        for file in entries:
            try:
                info = file.lstat()
                if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & reparse:
                    continue
                if stat.S_ISDIR(info.st_mode):
                    pending.append(file)
                elif stat.S_ISREG(info.st_mode) and file.suffix.lower() in ('.exe', '.pdb', '.ilk'):
                    if not file.resolve(strict=True).is_relative_to(resolved_root):
                        raise ValueError('Binary cleanup target escaped its fixture')
                    # A just-restarted synthetic app can take a moment to exit.
                    # Never terminate another process to make a file deletable.
                    for attempt in range(3):
                        try:
                            file.unlink()
                            removed += 1
                            break
                        except FileNotFoundError:
                            break
                        except PermissionError:
                            if attempt == 2:
                                skipped += 1
                            else:
                                time.sleep(.1)
            except FileNotFoundError:
                continue
            except OSError:
                skipped += 1
    print(f'Fixture binary cleanup: removed {removed}; locked/unavailable {skipped}; data/reports preserved: {root}', flush=True)
    return removed, skipped


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def eventually(predicate, message, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.05)
    raise AssertionError(message)


def compile_app(version, marker):
    directory = ROOT / ('compiled-' + marker)
    directory.mkdir()
    source = directory / 'fake.cs'
    source.write_text('''using System; using System.IO; using System.Threading;
using System.Reflection;
[assembly:AssemblyFileVersion("VERSION")]
class FakeApp {
  static void Main(string[] args) {
    string root = AppDomain.CurrentDomain.BaseDirectory;
    if (args.Length > 0 && args[0] == "--wait") {
      File.WriteAllText(Path.Combine(root, "wait.ready"), "ready");
      while (!File.Exists(Path.Combine(root, "allow-exit"))) Thread.Sleep(25);
    } else {
      File.WriteAllLines(Path.Combine(root, "restart.args"), args);
      File.WriteAllText(Path.Combine(root, "restart.marker"), "MARKER");
    }
  }
}'''.replace('VERSION', version).replace('MARKER', marker), encoding='utf-8')
    compiler = Path(os.environ['SystemRoot']) / 'Microsoft.NET/Framework/v4.0.30319/csc.exe'
    binary = directory / 'microsip.exe'
    subprocess.run([str(compiler), '/nologo', '/target:winexe', '/out:' + str(binary), str(source)],
                   check=True, creationflags=FLAGS, capture_output=True)
    return binary


def archive(stage, with_ini=True, extra=None, corrupt_hash=False):
    entries = {
        'microsip.exe': NEW.read_bytes(),
        'MicroSIPAudioGuard.exe': b'synthetic updated guard',
        'LICENSE': b'synthetic GPL license fixture',
        'COPYING': b'synthetic GPL combined fixture',
        'THIRD-PARTY-NOTICES.md': b'synthetic notices fixture',
        'BUILD-REVISION.txt': ('a' * 40 + '\n' + VERSION + '\n').encode(),
        'README.md': b'z' * 50000,
        'licenses/PJSIP.txt': b'synthetic third-party notice',
        'ringtone.wav': b'stock sound from package',
        'call_log.db': b'package must never replace history',
        'Apply-Update.ps1': b'throw "A downloaded script must never execute"',
    }
    if with_ini:
        entries['microsip.ini'] = b'[Settings]\naccountId=0\n'
    lines = [hashlib.sha256(data).hexdigest() + '  ' + name for name, data in entries.items()]
    entries['SHA256SUMS.txt'] = ('\n'.join(lines) + '\n').encode()
    if corrupt_hash:
        entries['README.md'] = b'tampered after internal manifest'
    path = stage / 'release.zip'
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as output:
        for name, data in entries.items():
            output.writestr('MicroSIP-Custom/' + name, data)
        if extra:
            name, data, attributes = extra
            item = zipfile.ZipInfo(name)
            item.external_attr = attributes
            output.writestr(item, data)
    return path


def args(mode, stage, checksum=None, install=None, parent=None, profile_directory=None):
    result = [str(POWERSHELL), '-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
              '-File', str(HELPER), '-Mode', mode, '-StagePath', str(stage), '-ExpectedVersion', VERSION,
              '-ArchiveSha256', checksum or sha(stage / 'release.zip'), '-NonInteractive']
    if install:
        kernel = c.WinDLL('kernel32', use_last_error=True)
        kernel.GetProcessTimes.argtypes = [w.HANDLE] + [c.POINTER(w.FILETIME)] * 4
        created, exited, kernel_time, user = [w.FILETIME() for _ in range(4)]
        assert kernel.GetProcessTimes(w.HANDLE(int(parent._handle)), c.byref(created), c.byref(exited),
                                      c.byref(kernel_time), c.byref(user))
        filetime = (created.dwHighDateTime << 32) | created.dwLowDateTime
        result += ['-InstallPath', str(install), '-ParentProcessId', str(parent.pid),
                   '-ParentStartedUtcFileTime', str(filetime)]
    if profile_directory:
        result += ['-ProfileDirectory', str(profile_directory)]
    return result


def prepare_case(name, *, with_ini=True, extra=None, corrupt_hash=False, wrong_hash=False, patch_size=False):
    stage = ROOT / name
    stage.mkdir()
    package = archive(stage, with_ini=with_ini, extra=extra, corrupt_hash=corrupt_hash)
    if patch_size:
        raw = bytearray(package.read_bytes())
        position = 0
        while True:
            position = raw.find(b'PK\x01\x02', position)
            assert position >= 0
            length, extra_length, comment_length = struct.unpack_from('<HHH', raw, position + 28)
            filename = bytes(raw[position + 46:position + 46 + length])
            if filename == b'MicroSIP-Custom/README.md':
                local = struct.unpack_from('<I', raw, position + 42)[0]
                struct.pack_into('<I', raw, position + 24, 1)
                struct.pack_into('<I', raw, local + 22, 1)
                break
            position += 46 + length + extra_length + comment_length
        package.write_bytes(raw)
    result = subprocess.run(args('Prepare', stage, '0' * 64 if wrong_hash else None), capture_output=True,
                            text=True, creationflags=FLAGS, timeout=45)
    valid = not (extra or corrupt_hash or wrong_hash or patch_size)
    assert (result.returncode == 0) == valid, (name, result.stdout, result.stderr)
    print('PASS:', name, flush=True)
    return stage


def install_fixture(name):
    install = ROOT / name
    install.mkdir()
    (install / 'microsip.exe').write_bytes(OLD.read_bytes())
    (install / 'MicroSIPAudioGuard.exe').write_bytes(b'synthetic original guard')
    protected = {
        'microsip.ini': b'[Account1]\nprivate-account=preserve\n',
        'microsip.portable': b'preserve portable choice',
        'call_log.db': b'private call history',
        'contacts.db': b'private contacts',
        'ringtone.wav': b'custom user ringtone',
        'recordings/call.wav': b'private recording',
        'custom-data.txt': b'user data',
    }
    for name, data in protected.items():
        file = install / name
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(data)
    return install, protected


def apply_case(name, stage, rollback=False, damaged=False, junction=False, explicit_profile=False):
    install, protected = install_fixture(name)
    original = sha(install / 'microsip.exe')
    expected_archive = sha(stage / 'release.zip')
    failed = rollback or damaged or junction
    profile_directory = ROOT / "profile's shared directory" if explicit_profile else None
    if profile_directory:
        profile_directory.mkdir(exist_ok=True)
    if damaged:
        archive_bytes = bytearray((stage / 'release.zip').read_bytes())
        archive_bytes[0] ^= 1  # The app already approved the original hash.
        (stage / 'release.zip').write_bytes(archive_bytes)
    if junction:
        outside = ROOT / (name + '-outside')
        outside.mkdir()
        (outside / 'PJSIP.txt').write_bytes(b'outside protected fixture')
        subprocess.run(['cmd.exe', '/c', 'mklink', '/J', str(install / 'licenses'), str(outside)],
                       check=True, capture_output=True, creationflags=FLAGS)
    parent = subprocess.Popen([str(install / 'microsip.exe'), '--wait'], creationflags=FLAGS)
    worker = None
    lock = None
    if rollback:
        kernel = c.WinDLL('kernel32', use_last_error=True)
        kernel.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p, w.DWORD, w.DWORD, w.HANDLE]
        kernel.CreateFileW.restype = w.HANDLE
        kernel.CloseHandle.argtypes = [w.HANDLE]
        lock = kernel.CreateFileW(str(install / 'MicroSIPAudioGuard.exe'), 0x80000000, 1, None, 3, 0x80, None)
        assert lock and lock != w.HANDLE(-1).value
    try:
        eventually(lambda: (install / 'wait.ready').exists(), 'Synthetic parent did not start')
        worker = subprocess.Popen(args('Apply', stage, checksum=expected_archive, install=install, parent=parent,
                                       profile_directory=profile_directory), creationflags=FLAGS,
                                  stdout=(stage / (name + '.stdout')).open('wb'), stderr=(stage / (name + '.stderr')).open('wb'))
        OWNED_WORKERS.append(worker)
        time.sleep(1)
        assert worker.poll() is None and sha(install / 'microsip.exe') == original, 'Update replaced an application before it exited'
        (install / 'allow-exit').write_text('exit')
        parent.wait(timeout=10)
        code = worker.wait(timeout=90)
        assert (code == 0) != failed, (name, code, (stage / (name + '.stderr')).read_text(errors='replace'))
        expected = OLD if failed else NEW
        assert sha(install / 'microsip.exe') == sha(expected), 'Updated/rolled-back executable hash differs'
        for relative, data in protected.items():
            assert (install / relative).read_bytes() == data, 'Overwrote protected data: ' + relative
        assert not (install / 'Apply-Update.ps1').exists(), 'Installed a downloaded script'
        eventually(lambda: (install / 'restart.marker').exists(), 'Updater did not restart the same application')
        assert (install / 'restart.marker').read_text() == ('old' if failed else 'new')
        expected_args = ['/profile-directory', str(profile_directory)] if profile_directory else []
        assert (install / 'restart.args').read_text().splitlines() == expected_args, 'Restart lost or changed the explicit profile directory'
        if failed:
            assert (install / 'MicroSIPAudioGuard.exe').read_bytes() == b'synthetic original guard'
        else:
            assert (install / 'MicroSIPAudioGuard.exe').read_bytes() == b'synthetic updated guard'
        if junction:
            assert (outside / 'PJSIP.txt').read_bytes() == b'outside protected fixture'
        print('PASS:', name, '(config/history/recordings/sounds preserved; same executable restarted)', flush=True)
    finally:
        if parent.poll() is None:
            (install / 'allow-exit').write_text('exit')
            parent.wait(timeout=10)
        if lock:
            kernel.CloseHandle(lock)
        if worker and worker.poll() is None:
            worker.wait(timeout=95)


def run():
    with_ini = prepare_case('valid-old-format-with-ini')
    without_ini = prepare_case('valid-shared-profile-without-ini', with_ini=False)
    prepare_case('reject-outer-sha256', wrong_hash=True)
    prepare_case('reject-internal-sha256', corrupt_hash=True)
    prepare_case('reject-understated-zip-size', patch_size=True)
    for name, path, attrs in [
        ('traversal', 'MicroSIP-Custom/../escape.txt', 0),
        ('absolute', '/MicroSIP-Custom/escape.txt', 0),
        ('backslash', 'MicroSIP-Custom/..\\escape.txt', 0),
        ('ads', 'MicroSIP-Custom/file.txt:stream', 0),
        ('case-collision', 'MicroSIP-Custom/readme.md', 0),
        ('symlink', 'MicroSIP-Custom/link', 0o120777 << 16),
    ]:
        prepare_case('reject-' + name, extra=(path, b'unsafe fixture', attrs))
    apply_case('apply-old-format', with_ini)
    apply_case("apply shared's format", without_ini, explicit_profile=True)
    damaged = prepare_case('valid-before-postprepare-corruption')
    apply_case('restart-after-postprepare-corruption', damaged, damaged=True)
    reparse = prepare_case('valid-before-junction-check')
    apply_case('reject-install-junction', reparse, junction=True)
    rollback = prepare_case('valid-for-rollback')
    apply_case('rollback-on-locked-guard', rollback, rollback=True)
    print('PASS: all offline update integration/security tests; artifacts:', ROOT, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--keep-binaries', action='store_true', help='Retain disposable executables/debug files for diagnosis')
    options = parser.parse_args()
    global ROOT, OLD, NEW
    ROOT = REPO / 'build' / ('updater-integration-' + uuid.uuid4().hex)
    ROOT.mkdir(parents=True)
    try:
        OLD = compile_app('9.9.9.8', 'old')
        NEW = compile_app(VERSION, 'new')
        run()
    finally:
        if not options.keep_binaries:
            if any(worker.poll() is None for worker in OWNED_WORKERS):
                print('Binary cleanup deferred because an owned test worker is still running:', ROOT, flush=True)
            else:
                cleanup_binaries(ROOT)


if __name__ == '__main__':
    main()
