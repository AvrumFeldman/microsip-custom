# SPDX-License-Identifier: GPL-2.0-or-later
"""Single Call Mode regression against an isolated portable instance.

Usage: python tests/smoke_call_waiting.py PID [--failure-cases]
Requires synthetic local account custom-test, sourcePort=5096, singleMode=1,
callWaiting=1, callAudioMode=0, and the default holdOnNewCallEntry=0. The test
changes the fixture's hold timing in Settings and restores it on success.
Never use a configured production instance.
The harness overrides the default port through MICROSIP_TEST_SIP_PORT.
"""
import ctypes as c
from ctypes import wintypes as w
import socket
import sys
import time
import uuid

import smoke_call_controls as sip

HOLD = 1057
END = 1055
NUMBER = 1078
STARTUP = 1041
NEW_CALL_HOLD = 1303
SETTINGS = 32789
WM_COMMAND = 0x111

sip.user.GetWindowLongW.argtypes = [w.HWND, c.c_int]
sip.user.GetWindowLongW.restype = w.LONG


def incoming():
    call_id = uuid.uuid4().hex
    request = (f'INVITE sip:custom-test@127.0.0.1:{sip.local_port} SIP/2.0\r\n'
               f'Via: SIP/2.0/UDP 127.0.0.1:{sip.port};branch=z9hG4bK{uuid.uuid4().hex}\r\n'
               f'From: "waiting" <sip:waiting@127.0.0.1:{sip.port}>;tag=waiting\r\n'
               'To: <sip:custom-test@127.0.0.1>\r\n'
               f'Call-ID: {call_id}\r\nCSeq: 1 INVITE\r\nMax-Forwards: 70\r\n'
               f'Contact: <sip:waiting@127.0.0.1:{sip.port}>\r\n'
               f'Content-Type: application/sdp\r\nContent-Length: {len(sip.sdp)}\r\n\r\n{sip.sdp}')
    sip.peer.sendto(request.encode(), ('127.0.0.1', sip.local_port))
    return request


def status(call_id, code):
    while True:
        raw, address = sip.peer.recvfrom(65535)
        message = raw.decode()
        assert not message.startswith('BYE '), 'Answer/switch hung up an existing call'
        if message.startswith(f'SIP/2.0 {code}') and sip.header(message, 'Call-ID') == call_id:
            return message, address


def in_dialog(invite, reply, method, cseq):
    request = (f'{method} sip:custom-test@127.0.0.1:{sip.local_port} SIP/2.0\r\n'
               f'Via: SIP/2.0/UDP 127.0.0.1:{sip.port};branch=z9hG4bK{uuid.uuid4().hex}\r\n'
               f'From: {sip.header(invite, "From")}\r\nTo: {sip.header(reply, "To")}\r\n'
               f'Call-ID: {sip.header(invite, "Call-ID")}\r\nCSeq: {cseq} {method}\r\n'
               'Max-Forwards: 70\r\nContent-Length: 0\r\n\r\n')
    sip.peer.sendto(request.encode(), ('127.0.0.1', sip.local_port))


def cancel_waiting(invite):
    target = f'sip:custom-test@127.0.0.1:{sip.local_port}'
    request = (f'CANCEL {target} SIP/2.0\r\n' +
               '\r\n'.join(f'{name}: {sip.header(invite, name)}'
                             for name in ('Via', 'From', 'To', 'Call-ID')) +
               '\r\nCSeq: 1 CANCEL\r\nMax-Forwards: 70\r\nContent-Length: 0\r\n\r\n')
    sip.peer.sendto(request.encode(), ('127.0.0.1', sip.local_port))
    reply, _ = status(sip.header(invite, 'Call-ID'), 487)
    ack = (f'ACK {target} SIP/2.0\r\n' +
           '\r\n'.join(f'{name}: {sip.header(reply, name)}'
                         for name in ('Via', 'From', 'To', 'Call-ID')) +
           '\r\nCSeq: 1 ACK\r\nMax-Forwards: 70\r\nContent-Length: 0\r\n\r\n')
    sip.peer.sendto(ack.encode(), ('127.0.0.1', sip.local_port))


def quiet_before_hold_ack(target_id):
    end = time.monotonic() + .4
    sip.peer.settimeout(.05)
    try:
        while time.monotonic() < end:
            try:
                raw, _ = sip.peer.recvfrom(65535)
            except socket.timeout:
                continue
            message = raw.decode()
            assert not message.startswith('BYE '), 'Existing call was disconnected'
            assert not (message.startswith('SIP/2.0 200') and sip.header(message, 'Call-ID') == target_id), 'Answered before hold acknowledgment'
            assert not (message.startswith('INVITE ') and sip.header(message, 'Call-ID') == target_id), 'Resumed before hold acknowledgment'
    finally:
        sip.peer.settimeout(5)


def hold_menu():
    sip.user.PostMessageW(sip.dialer, WM_COMMAND, HOLD, 0)
    return sip.eventually(lambda: next((menu for menu in sip.popup_menus()
                           if 'Hold current call' in menu[2] or 'Resume current call' in menu[2]), None),
                          'Hold dropdown')


def choose_hold_action(label):
    popup = hold_menu()
    matches = [i for i, item in enumerate(popup[2]) if item == label or item.startswith(label + '@')]
    assert len(matches) == 1, popup[2]
    sip.click_menu(popup, matches[0])


def choose_call(name):
    choose_hold_action(f'Switch to {name}')


def switch(held_id, resumed_id, name):
    choose_call(name)
    hold, address = sip.receive('INVITE')
    assert sip.header(hold, 'Call-ID') == held_id
    assert 'a=sendonly' in hold or 'a=inactive' in hold
    quiet_before_hold_ack(resumed_id)
    sip.response(hold, address, body=sip.sdp)
    resume, address = sip.receive('INVITE')
    assert sip.header(resume, 'Call-ID') == resumed_id
    assert 'a=sendrecv' in resume
    sip.response(resume, address, body=sip.sdp)
    records = sip.pump(.5)
    assert not any(r.startswith('BYE ') for r in records)


def new_call_dialog():
    choose_hold_action('New call...')
    dlg = sip.eventually(lambda: sip.find_dialog('New call'), 'New call number dialog')
    label = c.create_unicode_buffer(128)
    sip.send(sip.user.GetDlgItem(dlg, 1), 0x0d, len(label), c.addressof(label))  # WM_GETTEXT
    assert label.value == 'Call', label.value
    return dlg


def dial(dlg, number):
    destination = c.create_unicode_buffer(f'sip:{number}@127.0.0.1:{sip.port}')
    sip.send(sip.user.GetDlgItem(dlg, NUMBER), 0x0c, 0, c.addressof(destination))
    sip.send(dlg, WM_COMMAND, 1, 0)


def assert_no_dial(hold=None):
    end = time.monotonic() + .4
    sip.peer.settimeout(.05)
    try:
        while time.monotonic() < end:
            try:
                message = sip.peer.recvfrom(65535)[0].decode()
            except socket.timeout:
                continue
            assert not message.startswith(('BYE ', 'REFER ')), message.splitlines()[0]
            if message.startswith('INVITE '):
                assert hold is not None, 'Unexpected call or re-INVITE before pressing Call'
                assert (sip.header(message, 'Call-ID'), sip.header(message, 'CSeq')) == (
                    sip.header(hold, 'Call-ID'), sip.header(hold, 'CSeq')
                ), 'Unexpected call or re-INVITE before hold acknowledgment'
    finally:
        sip.peer.settimeout(5)


def receive_hold(call_id):
    request, address = sip.receive('INVITE')
    assert sip.header(request, 'Call-ID') == call_id
    assert 'a=sendonly' in request or 'a=inactive' in request
    return request, address


def acknowledge_resume(call_id):
    request, address = sip.receive('INVITE')
    assert sip.header(request, 'Call-ID') == call_id
    assert 'a=sendrecv' in request
    sip.response(request, address, body=sip.sdp)
    assert not any(message.startswith(('BYE ', 'REFER ')) for message in sip.pump(.4))


def end_original(call_id):
    sip.command('/hangupall')
    bye, address = sip.receive('BYE')
    assert sip.header(bye, 'Call-ID') == call_id
    sip.response(bye, address)
    sip.pump(.4)


def configure_hold_timing(expected, selection=None):
    sip.send(sip.main, WM_COMMAND, SETTINGS)
    dlg = sip.eventually(lambda: sip.find_dialog('Settings'), 'Settings')
    combo = sip.user.GetDlgItem(dlg, NEW_CALL_HOLD)
    assert combo and sip.send(combo, 0x146) == 2  # CB_GETCOUNT
    for index, label in enumerate(('When pressing Call', 'When choosing New call')):
        text = c.create_unicode_buffer(128)
        sip.send(combo, 0x148, index, c.addressof(text))  # CB_GETLBTEXT
        assert text.value == label, text.value
    assert sip.send(combo, 0x147) == expected  # CB_GETCURSEL
    if selection is not None:
        # Only save settings with startup disabled for this disposable fixture.
        startup = sip.user.GetDlgItem(dlg, STARTUP)
        assert startup and sip.send(startup, 0xf0) == 0  # BM_GETCHECK
        assert sip.send(combo, 0x14e, selection) == selection  # CB_SETCURSEL
    sip.user.PostMessageW(dlg, WM_COMMAND, 2 if selection is None else 1, 0)
    sip.eventually(lambda: not sip.find_dialog('Settings'), 'closing Settings', timeout=15)


def outgoing_calls():
    configure_hold_timing(0)
    original, _ = sip.establish('outgoing-original')
    original_id = sip.header(original, 'Call-ID')
    hold_control = sip.user.GetDlgItem(sip.dialer, HOLD)
    style = sip.user.GetWindowLongW(hold_control, -16)
    assert style & 0x0f in (0x0c, 0x0d), 'Hold is missing its split-button style'
    popup = hold_menu()
    assert 'New call...' in popup[2]
    assert not any(label.startswith('Switch to ') for label in popup[2])
    sip.click_menu(popup, popup[2].index('Hold current call'))
    hold, address = receive_hold(original_id)
    sip.response(hold, address, body=sip.sdp)
    sip.pump(.4)
    choose_hold_action('Resume current call')
    acknowledge_resume(original_id)
    print('PASS: a single connected call shows a split Hold button with Hold, Resume and New call', flush=True)

    dlg = new_call_dialog()
    assert_no_dial()
    sip.send(dlg, WM_COMMAND, 2)
    assert_no_dial()
    dlg = new_call_dialog()
    assert_no_dial()
    dial(dlg, 'outgoing-second')
    hold, address = receive_hold(original_id)
    assert_no_dial(hold)
    sip.response(hold, address, body=sip.sdp)
    second, address = sip.receive('INVITE', fresh=True)
    assert second.startswith(f'INVITE sip:outgoing-second@127.0.0.1:{sip.port} ')
    second_id = sip.header(second, 'Call-ID')
    assert second_id != original_id
    sip.response(second, address, body=sip.sdp)
    assert not any(message.startswith(('BYE ', 'REFER ')) for message in sip.pump(.5))
    switch(second_id, original_id, 'outgoing-original')
    switch(original_id, second_id, 'sip:outgoing-second')
    sip.remote_bye(second, address)
    acknowledge_resume(original_id)
    end_original(original_id)
    print('PASS: default New call holds only after Call and resumes the first call when the second ends', flush=True)

    configure_hold_timing(0, 1)
    configure_hold_timing(1)
    original, _ = sip.establish('immediate-original')
    original_id = sip.header(original, 'Call-ID')
    dlg = new_call_dialog()
    hold, address = receive_hold(original_id)
    sip.send(dlg, WM_COMMAND, 2)
    assert_no_dial(hold)
    sip.response(hold, address, body=sip.sdp)
    acknowledge_resume(original_id)

    dlg = new_call_dialog()
    hold, address = receive_hold(original_id)
    dial(dlg, 'busy-second')
    assert_no_dial(hold)
    sip.response(hold, address, body=sip.sdp)
    second, address = sip.receive('INVITE', fresh=True)
    assert second.startswith(f'INVITE sip:busy-second@127.0.0.1:{sip.port} ')
    sip.response(second, address, 486, 'Busy Here')
    acknowledge_resume(original_id)

    dlg = new_call_dialog()
    hold, address = receive_hold(original_id)
    sip.response(hold, address, body=sip.sdp)
    dial(dlg, 'end-locally')
    second, address = sip.receive('INVITE', fresh=True)
    sip.response(second, address, body=sip.sdp)
    sip.pump(.5)
    sip.user.PostMessageW(sip.dialer, WM_COMMAND, END, 0)
    bye, bye_address = sip.receive('BYE')
    assert sip.header(bye, 'Call-ID') == sip.header(second, 'Call-ID')
    sip.response(bye, bye_address)
    acknowledge_resume(original_id)
    end_original(original_id)
    print('PASS: immediate hold waits for acknowledgment and resumes after cancel, busy, or End', flush=True)
    configure_hold_timing(1, 0)


try:
    original, original_address = sip.establish('first')
    original_id = sip.header(original, 'Call-ID')
    waiting = incoming()
    waiting_id = sip.header(waiting, 'Call-ID')
    status(waiting_id, 180)
    sip.command('/answer')
    hold, address = sip.receive('INVITE')
    assert sip.header(hold, 'Call-ID') == original_id
    assert 'a=sendonly' in hold or 'a=inactive' in hold
    quiet_before_hold_ack(waiting_id)
    sip.response(hold, address, body=sip.sdp)
    answered, _ = status(waiting_id, 200)
    in_dialog(waiting, answered, 'ACK', 1)
    assert not any(r.startswith('BYE ') for r in sip.pump(.5))
    print('PASS: answering call waiting waits for hold and preserves the first call', flush=True)

    switch(waiting_id, original_id, 'first')
    switch(original_id, waiting_id, 'waiting')
    print('PASS: Hold menu switches both ways, acknowledging hold before resume', flush=True)

    if '--failure-cases' in sys.argv:
        choose_call('first')
        hold, address = sip.receive('INVITE')
        assert sip.header(hold, 'Call-ID') == waiting_id
        sip.response(hold, address, body=sip.sdp)
        resume, address = sip.receive('INVITE')
        assert sip.header(resume, 'Call-ID') == original_id
        sip.response(resume, address, 488, 'Not Acceptable Here')
        sip.peer.settimeout(38)
        try:
            rollback, address = sip.receive('INVITE')
        finally:
            sip.peer.settimeout(5)
        assert sip.header(rollback, 'Call-ID') == waiting_id
        sip.response(rollback, address, body=sip.sdp)
        assert not any(r.startswith('BYE ') for r in sip.pump(.5))
        print('PASS: rejected resume restores the previously active party', flush=True)

    in_dialog(waiting, answered, 'BYE', 2)
    sip.pump(.5)
    # The remaining held call still has a working normal resume control.
    choose_hold_action('Resume current call')
    resume, address = sip.receive('INVITE')
    assert sip.header(resume, 'Call-ID') == original_id
    sip.response(resume, address, body=sip.sdp)
    sip.pump(.4)
    sip.command('/hangupall')
    ended, address = sip.receive('BYE')
    assert sip.header(ended, 'Call-ID') == original_id
    sip.response(ended, address)
    sip.pump(.4)
    print('PASS: ending the second call leaves the first resumable', flush=True)

    original, _ = sip.establish('cancel-before-answer')
    original_id = sip.header(original, 'Call-ID')
    waiting = incoming()
    waiting_id = sip.header(waiting, 'Call-ID')
    status(waiting_id, 180)
    sip.command('/answer')
    hold, address = sip.receive('INVITE')
    assert sip.header(hold, 'Call-ID') == original_id
    cancel_waiting(waiting)
    sip.response(hold, address, body=sip.sdp)
    resume, address = sip.receive('INVITE')
    assert sip.header(resume, 'Call-ID') == original_id and 'a=sendrecv' in resume
    sip.response(resume, address, body=sip.sdp)
    assert not any(r.startswith('BYE ') for r in sip.pump(.4))
    print('PASS: caller canceling during pending hold restores the first call', flush=True)
    end_original(original_id)
    outgoing_calls()
finally:
    sip.send(sip.messages, 0x1f)  # cancel only the test window's popup, if open
    if dialog := sip.find_dialog('New call'):
        sip.user.PostMessageW(dialog, WM_COMMAND, 2, 0)
    sip.command('/hangupall')
    sip.command('/hangupincoming')
    sip.pump(.5)
    sip.peer.close()
    sip.rtp.close()
