# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify durable call history using only synthetic localhost SIP dialogs.

Usage: python tests/smoke_call_history.py PID PATH_TO_TEST_CALL_LOG_DB [--blocked-only]
Use a fresh portable instance under this checkout's build/ directory with
sourcePort=5096, accountId=0, enableLocalAccount=1, singleMode=1,
callWaiting=1, denyIncoming=button, DND=0, callAudioMode=0 and local username
custom-test. --blocked-only instead requires DND=1 and checks policy rejection.
The database must be beside that test process's executable. The installed
softphone and its configuration are never read or changed.
The harness overrides the default port through MICROSIP_TEST_SIP_PORT.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
from pathlib import Path
import socket
import sqlite3
import time
import uuid


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('pid', type=int)
parser.add_argument('database', type=Path)
parser.add_argument('--blocked-only', action='store_true')
options = parser.parse_args()
database = options.database.resolve(strict=True)
build_root = Path(__file__).resolve().parents[1] / 'build'
assert database.is_relative_to(build_root), 'Use an isolated portable copy under build/'
kernel = c.WinDLL('kernel32', use_last_error=True)
kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
kernel.OpenProcess.restype = w.HANDLE
kernel.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, c.POINTER(w.DWORD)]
kernel.CloseHandle.argtypes = [w.HANDLE]
process = kernel.OpenProcess(0x1000, False, options.pid)
assert process, 'Cannot identify the test process'
try:
    image = c.create_unicode_buffer(32768)
    length = w.DWORD(len(image))
    assert kernel.QueryFullProcessImageNameW(process, 0, image, c.byref(length))
    assert Path(image.value).resolve().parent == database.parent, 'DB is not beside the test executable'
finally:
    kernel.CloseHandle(process)

import smoke_call_controls as sip

sip.user.IsWindowVisible.argtypes = [w.HWND]
sip.user.IsWindowEnabled.argtypes = [w.HWND]
sip.peer.settimeout(.05)
target = ('127.0.0.1', sip.local_port)


def history(call_id):
    with sqlite3.connect(database.as_uri() + '?mode=ro', uri=True) as db:
        db.row_factory = sqlite3.Row
        rows = db.execute('SELECT * FROM call_log WHERE callId=?', (call_id,)).fetchall()
        assert len(rows) <= 1, 'Repeated state notifications duplicated a call record'
        return dict(rows[0]) if rows else None


def wait_packet(predicate, message, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        sip.send(sip.main, 0)
        try:
            data, address = sip.peer.recvfrom(65535)
        except socket.timeout:
            continue
        packet = data.decode(errors='replace')
        if predicate(packet):
            return packet, address
        if packet.startswith('INVITE '):
            sip.response(packet, address, body=sip.sdp)
        elif packet.startswith(('BYE ', 'OPTIONS ')):
            sip.response(packet, address)
    raise AssertionError(message)


def reply_for(call_id, code, method='INVITE'):
    return wait_packet(lambda p: p.startswith(f'SIP/2.0 {code} ') and
                       sip.header(p, 'Call-ID') == call_id and
                       sip.header(p, 'CSeq').endswith(' ' + method),
                       f'No {code} response to {method} for {call_id}')


def incoming(username, display=None):
    call_id = 'history-' + uuid.uuid4().hex
    caller = f'<sip:{username}@127.0.0.1:{sip.port}>;tag=history-test'
    if display is not None:
        caller = f'"{display}" ' + caller
    invite = (f'INVITE sip:custom-test@127.0.0.1:{sip.local_port} SIP/2.0\r\n'
              f'Via: SIP/2.0/UDP 127.0.0.1:{sip.port};branch=z9hG4bK{uuid.uuid4().hex}\r\n'
              f'From: {caller}\r\nTo: <sip:custom-test@127.0.0.1:{sip.local_port}>\r\n'
              f'Call-ID: {call_id}\r\nCSeq: 1 INVITE\r\nMax-Forwards: 70\r\n'
              f'Contact: <sip:{username}@127.0.0.1:{sip.port}>\r\n'
              f'Content-Type: application/sdp\r\nContent-Length: {len(sip.sdp)}\r\n\r\n{sip.sdp}')
    sip.peer.sendto(invite.encode(), target)
    return invite, call_id


def in_dialog(method, invite, response, sequence=1, transaction=False):
    via = (sip.header(invite, 'Via') if transaction else
           f'SIP/2.0/UDP 127.0.0.1:{sip.port};branch=z9hG4bK{uuid.uuid4().hex}')
    to = sip.header(invite if method == 'CANCEL' else response, 'To')
    packet = (f'{method} sip:custom-test@127.0.0.1:{sip.local_port} SIP/2.0\r\n'
              f'Via: {via}\r\nFrom: {sip.header(invite, "From")}\r\nTo: {to}\r\n'
              f'Call-ID: {sip.header(invite, "Call-ID")}\r\nCSeq: {sequence} {method}\r\n'
              'Max-Forwards: 70\r\nContent-Length: 0\r\n\r\n')
    sip.peer.sendto(packet.encode(), target)


def assert_record(call_id, call_type, username, info=None, duration=0):
    def persisted():
        row = history(call_id)
        return (row if row and row['type'] == call_type and row['duration'] >= duration
                and (info is None or row['info'] == info) else None)
    row = sip.eventually(persisted, f'Missing/wrong call history: {call_id}: {history(call_id)}')
    assert row['number'].startswith(username), row
    assert row['time'] > 0, row
    return row


def ring_dialog():
    return next((hwnd for hwnd in sip.windows()
                 if sip.user.GetDlgItem(hwnd, 1000) and sip.user.GetDlgItem(hwnd, 1032)
                 and sip.user.IsWindowVisible(hwnd)
                 and sip.user.IsWindowEnabled(sip.user.GetDlgItem(hwnd, 1032))), None)


def check_blocked():
    invite, call_id = incoming('blocked-history', 'Blocked caller')
    rejected, _ = reply_for(call_id, 486)
    in_dialog('ACK', invite, rejected, transaction=True)
    row = assert_record(call_id, 2, 'blocked-history', 'Do Not Disturb')
    assert row['duration'] == 0 and row['name'] == 'Blocked caller', row
    assert not ring_dialog(), 'DND rejection unexpectedly opened a ringing dialog'
    print('PASS: DND-rejected incoming call persists once with caller metadata and rejection reason', flush=True)


try:
    dnd = sip.user.GetDlgItem(sip.dialer, 1002)
    assert dnd, 'The test instance needs denyIncoming=button'
    assert bool(sip.send(dnd, 0xf0)) == options.blocked_only, 'Unexpected initial DND state'
    if options.blocked_only:
        check_blocked()
    else:
        # A display name is optional. The caller number must still be recorded.
        invite, call_id = incoming('unnamed-history')
        reply_for(call_id, 180)
        sip.command('/answer')
        answered, _ = reply_for(call_id, 200)
        in_dialog('ACK', invite, answered)
        time.sleep(1.2)
        in_dialog('BYE', invite, answered, sequence=2)
        reply_for(call_id, 200, 'BYE')
        row = assert_record(call_id, 1, 'unnamed-history', 'Call Ended', duration=1)
        assert row['name'] == 'unnamed-history', row
        print('PASS: answered incoming caller without display name retains duration and result', flush=True)

        invite, call_id = incoming('named-history', 'Named caller')
        reply_for(call_id, 180)
        sip.command('/answer')
        answered, _ = reply_for(call_id, 200)
        in_dialog('ACK', invite, answered)
        time.sleep(.2)
        sip.command('/hangupall')
        bye, address = wait_packet(lambda p: p.startswith('BYE ') and
                                  sip.header(p, 'Call-ID') == call_id, 'No local hangup')
        sip.response(bye, address)
        row = assert_record(call_id, 1, 'named-history', 'Call Ended')
        assert row['name'] == 'Named caller', row
        print('PASS: answered incoming caller persists after local hangup', flush=True)

        invite, call_id = incoming('missed-history')
        ringing, _ = reply_for(call_id, 180)
        in_dialog('CANCEL', invite, ringing, transaction=True)
        rejected, _ = reply_for(call_id, 487)
        in_dialog('ACK', invite, rejected, transaction=True)
        assert_record(call_id, 2, 'missed-history', 'Canceled')
        print('PASS: unanswered canceled call remains in missed history', flush=True)

        invite, call_id = incoming('declined-history')
        reply_for(call_id, 180)
        dialog = sip.eventually(ring_dialog, 'No incoming decline controls')
        sip.send(dialog, 0x111, 1032)
        rejected, _ = reply_for(call_id, 486)
        in_dialog('ACK', invite, rejected, transaction=True)
        assert_record(call_id, 2, 'declined-history', 'Declined')
        print('PASS: declined call remains in missed history with Declined result', flush=True)

        sip.send(sip.dialer, 0x111, 1002)
        try:
            check_blocked()
        finally:
            sip.send(sip.dialer, 0x111, 1002)

        sip.command(f'sip:outgoing-history@127.0.0.1:{sip.port}')
        outgoing, address = wait_packet(lambda p: p.startswith('INVITE ') and
                                       ';tag=' not in sip.header(p, 'To'), 'No outgoing INVITE')
        sip.response(outgoing, address, 486, 'Busy Here')
        wait_packet(lambda p: p.startswith('ACK '), 'No rejection ACK')
        assert_record(sip.header(outgoing, 'Call-ID'), 0, 'outgoing-history', 'Busy Here')
        print('PASS: outgoing rejection retains outgoing type and Busy Here result', flush=True)
finally:
    sip.command('/hangupall')
    sip.command('/hangupincoming')
    sip.peer.close()
    sip.rtp.close()
