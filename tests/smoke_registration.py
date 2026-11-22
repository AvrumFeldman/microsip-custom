# SPDX-License-Identifier: GPL-2.0-or-later
"""Loopback-only SIP registration recovery regression (about two minutes).

Usage: python tests/smoke_registration.py PID REGISTRAR_PORT [TRANSCRIPT.json]
PID must be a separate portable test copy with this account-free test setup:
  [Settings] accountId=1, enableLocalAccount=0, sourcePort=5096,
             enableSTUN=0, enableLog=1, callAudioMode=0
  [Account1] server=127.0.0.1:REGISTRAR_PORT, domain=127.0.0.1,
             username=registration-test, password=, transport=udp,
             registerRefresh=300, allowRewrite=0, publish=0
With --switch-unregistered also provide:
  [Account2] server=, domain=127.0.0.1, username=registration-disabled,
             password=, transport=udp
Bind/start this script immediately after launching the test copy. It verifies
the portable INI beside that PID's executable before accepting any SIP traffic.
No real account settings, startup entries, audio devices, or outside calls are
used. The test leaves the test account registered until its instance is closed.
"""
import argparse
import configparser
import ctypes as c
from ctypes import wintypes as w
import json
from pathlib import Path
import re
import socket
import time


def header(message, name):
    match = re.search(r'^' + re.escape(name) + r':\s*(.*)$', message, re.M | re.I)
    assert match, f'Missing {name} in SIP message'
    return match.group(1).strip()


def check_test_instance(pid, port, switch_unregistered):
    kernel = c.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    kernel.OpenProcess.restype = w.HANDLE
    kernel.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, c.POINTER(w.DWORD)]
    kernel.CloseHandle.argtypes = [w.HANDLE]
    process = kernel.OpenProcess(0x1000, False, pid)
    assert process, f'Cannot inspect test PID {pid}'
    try:
        image = c.create_unicode_buffer(32768)
        size = w.DWORD(len(image))
        assert kernel.QueryFullProcessImageNameW(process, 0, image, c.byref(size))
    finally:
        kernel.CloseHandle(process)
    ini = Path(image.value).with_suffix('.ini')
    raw = ini.read_bytes()
    config = configparser.ConfigParser(interpolation=None, strict=False)
    config.read_string(raw.decode('utf-16' if raw.startswith((b'\xff\xfe', b'\xfe\xff')) else 'utf-8-sig'))
    assert config.getint('Settings', 'accountId') == 1, 'Requires isolated test account 1'
    assert config.get('Account1', 'server') == f'127.0.0.1:{port}', 'Requires loopback registrar'
    assert config.get('Account1', 'domain') == '127.0.0.1'
    assert config.get('Account1', 'username') == 'registration-test', 'Requires synthetic account'
    assert not config.get('Account1', 'password', fallback=''), 'Test password must be blank'
    assert not config.get('Account1', 'proxy', fallback=''), 'Test proxy must be blank'
    assert config.get('Account1', 'transport', fallback='udp') == 'udp'
    if switch_unregistered:
        assert not config.get('Account2', 'server', fallback='')
        assert not config.get('Account2', 'proxy', fallback='')
        assert not config.get('Account2', 'password', fallback='')
        assert config.get('Account2', 'domain') == '127.0.0.1'
        assert config.get('Account2', 'username') == 'registration-disabled'
    print(f'Validated synthetic portable account in {ini}', flush=True)


def switch_to_unregistered(pid):
    user = c.WinDLL('user32', use_last_error=True)
    callback_type = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    user.EnumWindows.argtypes = [callback_type, w.LPARAM]
    user.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
    user.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
    user.SendMessageTimeoutW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM,
                                       w.UINT, w.UINT, c.POINTER(c.c_size_t)]
    user.SendMessageTimeoutW.restype = w.LPARAM
    windows = []

    @callback_type
    def visit(hwnd, unused):
        owner = w.DWORD()
        name = c.create_unicode_buffer(256)
        user.GetWindowThreadProcessId(hwnd, c.byref(owner))
        user.GetClassNameW(hwnd, name, len(name))
        if owner.value == pid and name.value == 'MicroSIP':
            windows.append(hwnd)
        return True

    user.EnumWindows(visit, 0)
    assert len(windows) == 1, 'Expected one isolated test main window'
    result = c.c_size_t()
    # WM_COMMAND / ID_ACCOUNT_CHANGE_RANGE + 1 selects synthetic account 2.
    assert user.SendMessageTimeoutW(windows[0], 0x111, 40001, 0, 3, 5000, c.byref(result))


def run(options):
    check_test_instance(options.pid, options.port, options.switch_unregistered)
    peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    peer.bind(('127.0.0.1', options.port))
    peer.settimeout(.2)
    replies = {}
    transcript = []
    last_attempt = None

    def receive(timeout=31):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                raw, address = peer.recvfrom(65535)
            except socket.timeout:
                continue
            assert address[0] == '127.0.0.1', 'Received non-loopback traffic'
            request = raw.decode('utf-8', errors='replace')
            if not request.startswith('REGISTER '):
                continue  # UDP keep-alives contain no SIP request.
            assert 'registration-test@' in header(request, 'From')
            key = (header(request, 'Call-ID'), header(request, 'CSeq'))
            if key in replies:
                peer.sendto(replies[key], address)
                continue
            if re.search(r'^Expires:\s*0\s*$', request, re.M | re.I):
                respond(request, address, key, 200, 0)
                continue
            return request, address, key, time.monotonic()
        raise AssertionError(f'No new registration attempt within {timeout}s')

    def respond(request, address, key, code, expiration=0):
        reason = {200: 'OK', 403: 'Forbidden', 503: 'Service Unavailable'}[code]
        fields = []
        for name in ('Via', 'From', 'To', 'Call-ID', 'CSeq'):
            value = header(request, name)
            if name == 'To' and ';tag=' not in value:
                value += ';tag=synthetic-registrar'
            fields.append(f'{name}: {value}')
        if code == 200:
            fields.append(f'Contact: {header(request, "Contact")};expires={expiration}')
            fields.append(f'Expires: {expiration}')
        else:
            # Avoid opening an account editor for the deliberate 403 failure.
            fields.append('P-Registrar-Error: Synthetic registrar unavailable')
        fields.append('Content-Length: 0')
        packet = (f'SIP/2.0 {code} {reason}\r\n' + '\r\n'.join(fields) + '\r\n\r\n').encode()
        replies[key] = packet
        peer.sendto(packet, address)

    try:
        # Repeated temporary errors exercise both first and subsequent native
        # retry intervals. 403 is intentionally outside PJSIP's retry whitelist.
        # A short successful lease then forces a refresh outage and recovery.
        phases = [(503, 0), (503, 0), (503, 0), (403, 0), (403, 0),
                  (200, 15), (503, 0), (503, 0), (200, 120)]
        for code, expiration in phases:
            request, address, key, now = receive(35 if last_attempt is None else 31)
            delay = None if last_attempt is None else now - last_attempt
            if delay is not None:
                assert delay <= 30.5, f'Registration attempt delayed {delay:.2f}s'
            last_attempt = now
            respond(request, address, key, code, expiration)
            transcript.append({'response': code, 'expires': expiration, 'delay': delay,
                               'call_id': key[0], 'cseq': key[1]})
            print(f'Registration {len(transcript)}: reply {code}, gap {delay}', flush=True)
        try:
            receive(26)
        except AssertionError as error:
            assert str(error).startswith('No new registration attempt'), error
        else:
            raise AssertionError('Healthy registration was retried before its scheduled refresh')
        print('PASS: repeated transient errors, 403 recovery, refresh outage, and healthy idle', flush=True)
        if options.switch_unregistered:
            switch_to_unregistered(options.pid)
            try:
                receive(31)
            except AssertionError as error:
                assert str(error).startswith('No new registration attempt'), error
            else:
                raise AssertionError('Registration continued after selecting a no-registrar account')
            print('PASS: account switch disables registration retries', flush=True)
    finally:
        peer.close()
        if options.transcript:
            Path(options.transcript).write_text(json.dumps(transcript, indent=2), encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pid', type=int)
    parser.add_argument('port', type=int)
    parser.add_argument('transcript', nargs='?')
    parser.add_argument('--switch-unregistered', action='store_true')
    run(parser.parse_args())
