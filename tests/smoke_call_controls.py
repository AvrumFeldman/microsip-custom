"""Local SIP/UI regression for single-call conference and attended transfer.

Usage: python tests/smoke_call_controls.py PID
Use an isolated portable copy with a blank local account, singleMode=1,
sourcePort=5096 and callAudioMode=0. No external calls are made.
"""
import ctypes as c
from ctypes import wintypes as w
import itertools
import re
import socket
import sys
import time
import uuid

user = c.WinDLL('user32', use_last_error=True)
callback_type = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
for name in ('EnumWindows', 'EnumChildWindows'):
    getattr(user, name).argtypes = ([callback_type, w.LPARAM] if name == 'EnumWindows'
                                  else [w.HWND, callback_type, w.LPARAM])
user.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
user.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
user.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
user.GetDlgItem.argtypes = [w.HWND, c.c_int]
user.GetDlgItem.restype = w.HWND
user.SendMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
user.SendMessageW.restype = w.LPARAM
user.PostMessageW.argtypes = user.SendMessageW.argtypes
user.GetMenuItemCount.argtypes = [w.HMENU]
user.GetMenuState.argtypes = [w.HMENU, w.UINT, w.UINT]
user.GetMenuStringW.argtypes = [w.HMENU, w.UINT, w.LPWSTR, c.c_int, w.UINT]
user.GetMenuItemRect.argtypes = [w.HWND, w.HMENU, w.UINT, c.POINTER(w.RECT)]
user.ScreenToClient.argtypes = [w.HWND, c.POINTER(w.POINT)]
pid = int(sys.argv[1])
user.SendMessageTimeoutW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM,
                                    w.UINT, w.UINT, c.POINTER(c.c_size_t)]
user.SendMessageTimeoutW.restype = w.LPARAM


def send(hwnd, message, wparam=0, lparam=0):
    result = c.c_size_t()
    if not user.SendMessageTimeoutW(hwnd, message, wparam, lparam, 3, 2000, c.byref(result)):
        raise TimeoutError(f'Test window stopped responding to message {message:#x}')
    return w.LPARAM(result.value).value


class CopyData(c.Structure):
    _fields_ = [('data', c.c_size_t), ('size', w.DWORD), ('pointer', c.c_void_p)]


def windows():
    found = []

    @callback_type
    def visit(hwnd, unused):
        owner = w.DWORD()
        user.GetWindowThreadProcessId(hwnd, c.byref(owner))
        if owner.value == pid:
            found.append(hwnd)
        return True

    user.EnumWindows(visit, 0)
    for hwnd in found.copy():
        user.EnumChildWindows(hwnd, visit, 0)
    return found


def text(hwnd, class_name=False):
    buf = c.create_unicode_buffer(512)
    (user.GetClassNameW if class_name else user.GetWindowTextW)(hwnd, buf, 512)
    return buf.value


def eventually(predicate, message, timeout=5):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        result = predicate()
        if result:
            return result
        time.sleep(.04)
    raise AssertionError(message)


main = next(hwnd for hwnd in windows() if text(hwnd, True) == 'MicroSIP')
messages = next(hwnd for hwnd in windows()
                if text(hwnd, True) == '#32770' and text(user.GetDlgItem(hwnd, 1000), True) == 'SysTabControl32')
dialer = next(hwnd for hwnd in windows() if user.GetDlgItem(hwnd, 1011)
              and user.GetDlgItem(hwnd, 1135))


def command(value):
    buf = c.create_unicode_buffer(value)
    data = CopyData(1, c.sizeof(buf), c.cast(buf, c.c_void_p))
    send(main, 0x4a, 0, c.addressof(data))


def action(value):
    send(messages, 0x111, value, 0)


def find_dialog(title):
    return next((h for h in windows() if text(h, True) == '#32770' and text(h) == title), None)


def destination(action_id, title, number):
    action(action_id)
    dlg = eventually(lambda: find_dialog(title), title)
    buf = c.create_unicode_buffer(number)
    send(user.GetDlgItem(dlg, 1078), 0x0c, 0, c.addressof(buf))
    send(dlg, 0x111, 1, 0)


def popup_menus():
    menus = []
    for hwnd in windows():
        if text(hwnd, True) == '#32768':
            menu = send(hwnd, 0x1e1, 0, 0)  # MN_GETHMENU
            labels = []
            for index in range(user.GetMenuItemCount(menu)):
                buf = c.create_unicode_buffer(512)
                user.GetMenuStringW(menu, index, buf, 512, 0x400)
                labels.append(buf.value)
            menus.append((hwnd, menu, labels))
    return menus


def click_menu(popup, index):
    hwnd, menu, _ = popup
    # Send keys only to the test menu's owning UI thread, without changing
    # the user's global mouse position or injecting input into other apps.
    for _ in range(user.GetMenuItemCount(menu) + 1):
        if user.GetMenuState(menu, index, 0x400) & 0x80:  # MF_HILITE
            break
        user.PostMessageW(hwnd, 0x100, 0x28, 0)  # Down
        time.sleep(.06)
    assert user.GetMenuState(menu, index, 0x400) & 0x80
    user.PostMessageW(hwnd, 0x100, 0x0d, 0)  # Enter


peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
peer.bind(('127.0.0.1', 0))
peer.settimeout(5)
port = peer.getsockname()[1]
rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rtp.bind(('127.0.0.1', 0))
sdp = ('v=0\r\no=test 1 1 IN IP4 127.0.0.1\r\ns=Local test\r\n'
       'c=IN IP4 127.0.0.1\r\nt=0 0\r\n'
       f'm=audio {rtp.getsockname()[1]} RTP/AVP 0 101\r\n'
       'a=rtpmap:0 PCMU/8000\r\na=rtpmap:101 telephone-event/8000\r\na=sendrecv\r\n')


def header(message, name):
    return re.search(r'^' + re.escape(name) + r':\s*(.*)\r?$', message, re.M | re.I).group(1).strip()


def response(request, address, code=200, reason='OK', body=''):
    headers = []
    for name in ('Via', 'From', 'To', 'Call-ID', 'CSeq'):
        value = header(request, name)
        if name == 'To' and ';tag=' not in value:
            value += ';tag=local-test'
        headers.append(f'{name}: {value}')
    headers.append(f'Contact: <sip:peer@127.0.0.1:{port}>')
    if body:
        headers.append('Content-Type: application/sdp')
    headers.append(f'Content-Length: {len(body)}')
    peer.sendto((f'SIP/2.0 {code} {reason}\r\n' + '\r\n'.join(headers) + '\r\n\r\n' + body).encode(), address)


def receive(method, fresh=False):
    while True:
        data, address = peer.recvfrom(65535)
        request = data.decode(errors='replace')
        existing = bool(re.search(r'^To:.*;tag=', request, re.M | re.I))
        if request.startswith(method + ' ') and not (fresh and existing):
            return request, address
        if request.startswith('INVITE ') and existing:
            response(request, address, body=sdp)


def pump(seconds):
    end = time.monotonic() + seconds
    peer.settimeout(.1)
    requests = []
    try:
        while time.monotonic() < end:
            try:
                packet, address = peer.recvfrom(65535)
            except socket.timeout:
                continue
            request = packet.decode(errors='replace')
            requests.append(request)
            if request.startswith('INVITE '):
                response(request, address, body=sdp)
            elif request.startswith('BYE '):
                response(request, address)
    finally:
        peer.settimeout(5)
    return requests


def establish(name):
    command(f'sip:{name}@127.0.0.1:{port}')
    request, address = receive('INVITE', fresh=True)
    response(request, address, body=sdp)
    receive('ACK')
    pump(.5)
    return request, address


notify_sequence = itertools.count(50)


def notify_transfer(refer, address, status):
    body = f'SIP/2.0 {status}\r\n'
    to = header(refer, 'To')
    packet = (f'NOTIFY {header(refer, "Contact").strip("<>")} SIP/2.0\r\n'
              f'Via: SIP/2.0/UDP 127.0.0.1:{port};branch=z9hG4bK{uuid.uuid4().hex}\r\n'
              f'From: {to}\r\nTo: {header(refer, "From")}\r\n'
              f'Call-ID: {header(refer, "Call-ID")}\r\nCSeq: {next(notify_sequence)} NOTIFY\r\n'
              'Max-Forwards: 70\r\nEvent: refer\r\nSubscription-State: terminated;reason=noresource\r\n'
              f'Content-Type: message/sipfrag\r\nContent-Length: {len(body)}\r\n\r\n{body}')
    peer.sendto(packet.encode(), address)


def remote_bye(invite, address):
    to = header(invite, 'To')
    if ';tag=' not in to:
        to += ';tag=local-test'
    packet = (f'BYE {header(invite, "Contact").strip("<>")} SIP/2.0\r\n'
              f'Via: SIP/2.0/UDP 127.0.0.1:{port};branch=z9hG4bK{uuid.uuid4().hex}\r\n'
              f'From: {to}\r\nTo: {header(invite, "From")}\r\n'
              f'Call-ID: {header(invite, "Call-ID")}\r\nCSeq: 90 BYE\r\n'
              'Max-Forwards: 70\r\nContent-Length: 0\r\n\r\n')
    peer.sendto(packet.encode(), address)


def run_tests():
    try:
        original, address = establish('original')
        expected_title = f'original@127.0.0.1:{port} \u2013 original'
        assert text(messages) == expected_title, f'Conversation title Unicode mismatch: {ascii(text(messages))}'
        user.PostMessageW(dialer, 0x111, 1135, 0)
        menu = eventually(lambda: next((m for m in popup_menus() if 'Blind Transfer' in m[2]), None), 'Transfer dropdown')
        assert any('Attended Transfer' in label for label in menu[2])
        click_menu(menu, 1)
        dlg = eventually(lambda: find_dialog('Attended Transfer'), 'Attended dialog')
        buf = c.create_unicode_buffer(f'sip:consult@127.0.0.1:{port}')
        send(user.GetDlgItem(dlg, 1078), 0x0c, 0, c.addressof(buf))
        send(dlg, 0x111, 1, 0)
        hold, address = receive('INVITE')
        assert header(hold, 'Call-ID') == header(original, 'Call-ID')
        # A consultation cannot start while the original caller is still unheld.
        peer.settimeout(.4)
        try:
            while True:
                pending = peer.recvfrom(65535)[0].decode(errors='replace')
                assert not pending.startswith('INVITE sip:consult'), 'consultation started before hold acknowledgment'
        except socket.timeout:
            pass
        finally:
            peer.settimeout(5)
        response(hold, address, body=sdp)
        consult, consult_address = receive('INVITE', fresh=True)
        assert consult.startswith(f'INVITE sip:consult@127.0.0.1:{port} ')
        response(consult, consult_address, body=sdp)
        receive('ACK')
        pump(.4)
        action(32825)  # cancel consultation and resume original
        results = pump(.8)
        assert any(r.startswith('BYE ') and header(r, 'Call-ID') == header(consult, 'Call-ID') for r in results)
        assert any(r.startswith('INVITE ') and header(r, 'Call-ID') == header(original, 'Call-ID') for r in results)
        assert not any(r.startswith('BYE ') and header(r, 'Call-ID') == header(original, 'Call-ID') for r in results)
        print('PASS: transfer dropdown, single-call consultation waits for hold; cancel resumes original', flush=True)

        destination(32793, 'Attended Transfer', f'sip:cancel-pending@127.0.0.1:{port}')
        hold, hold_address = receive('INVITE')
        action(32825)
        response(hold, hold_address, body=sdp)
        results = pump(.8)
        assert not any(r.startswith('INVITE sip:cancel-pending') for r in results)
        assert any(r.startswith('INVITE ') and header(r, 'Call-ID') == header(original, 'Call-ID') for r in results)
        print('PASS: cancel while hold is pending waits for acknowledgment, resumes, and never dials', flush=True)

        destination(32793, 'Attended Transfer', f'sip:busy@127.0.0.1:{port}')
        failed, failed_address = receive('INVITE', fresh=True)
        response(failed, failed_address, 486, 'Busy Here')
        receive('ACK')
        results = pump(.8)
        assert any(r.startswith('INVITE ') and header(r, 'Call-ID') == header(original, 'Call-ID') for r in results)
        assert not any(r.startswith('BYE ') for r in results)
        print('PASS: failed consultation automatically resumes the original caller', flush=True)

        destination(32793, 'Attended Transfer', f'sip:early-bye-failure@127.0.0.1:{port}')
        consult, consult_address = receive('INVITE', fresh=True)
        response(consult, consult_address, body=sdp)
        receive('ACK')
        pump(.4)
        action(32824)
        refer, refer_address = receive('REFER')
        response(refer, refer_address, 202, 'Accepted')
        remote_bye(consult, consult_address)
        results = pump(.5)
        assert not any(r.startswith(('INVITE ', 'BYE ')) and header(r, 'Call-ID') == header(original, 'Call-ID') for r in results), 'early consultation BYE changed source before transfer result'
        notify_transfer(refer, refer_address, '503 Service Unavailable')
        results = pump(.8)
        assert any(r.startswith('INVITE ') and header(r, 'Call-ID') == header(original, 'Call-ID') for r in results), 'failed transfer did not resume source after consultation ended'
        assert not any(r.startswith('BYE ') for r in results)
        print('PASS: consultation BYE before failed REFER result keeps source held, then resumes it', flush=True)

        destination(32793, 'Attended Transfer', f'sip:complete@127.0.0.1:{port}')
        consult, consult_address = receive('INVITE', fresh=True)
        response(consult, consult_address, body=sdp)
        receive('ACK')
        pump(.4)
        action(32824)
        refer, refer_address = receive('REFER')
        assert header(refer, 'Call-ID') == header(original, 'Call-ID')
        assert 'Replaces=' in header(refer, 'Refer-To')
        response(refer, refer_address, 403, 'Forbidden')
        assert not any(r.startswith('BYE ') for r in pump(.7)), 'rejected transfer ended a call'
        action(32824)  # a failed REFER can be retried without losing either call
        refer, refer_address = receive('REFER')
        response(refer, refer_address, 202, 'Accepted')
        remote_bye(consult, consult_address)
        results = pump(.5)
        assert not any(r.startswith(('INVITE ', 'BYE ')) and header(r, 'Call-ID') == header(original, 'Call-ID') for r in results), 'early consultation BYE changed source before transfer result'
        notify_transfer(refer, refer_address, '200 OK')
        results = pump(1.2)
        ended = {header(r, 'Call-ID') for r in results if r.startswith('BYE ')}
        assert header(original, 'Call-ID') in ended, ended
        assert not any(r.startswith('INVITE ') and header(r, 'Call-ID') == header(original, 'Call-ID') for r in results)
        print('PASS: REFER with Replaces succeeds after early consultation BYE and disconnects source without resuming', flush=True)

        original, address = establish('ordinary-complete')
        destination(32793, 'Attended Transfer', f'sip:ordinary-consult@127.0.0.1:{port}')
        consult, consult_address = receive('INVITE', fresh=True)
        response(consult, consult_address, body=sdp)
        receive('ACK')
        pump(.4)
        action(32824)
        refer, refer_address = receive('REFER')
        response(refer, refer_address, 202, 'Accepted')
        notify_transfer(refer, refer_address, '200 OK')
        results = pump(.8)
        ended = {header(r, 'Call-ID') for r in results if r.startswith('BYE ')}
        assert header(original, 'Call-ID') in ended and header(consult, 'Call-ID') in ended, ended
        print('PASS: ordinary successful attended transfer closes both local legs', flush=True)

        original, address = establish('conference-original')
        destination(32794, 'Invite to Conference', f'sip:conference-added@127.0.0.1:{port}')
        added, added_address = receive('INVITE', fresh=True)
        response(added, added_address, body=sdp)
        receive('ACK')
        pump(.5)
        user.PostMessageW(dialer, 0x111, 1011, 0)
        menu = eventually(lambda: next((m for m in popup_menus() if 'Remove participant' in m[2]), None), 'Conference menu')
        click_menu(menu, 1)
        participants = eventually(lambda: next((m for m in popup_menus() if 'conference-original' in m[2]), None), 'Participants submenu')
        assert len(participants[2]) == 2, participants[2]
        click_menu(participants, participants[2].index('conference-original'))
        bye, bye_address = receive('BYE')
        assert header(bye, 'Call-ID') == header(original, 'Call-ID')
        response(bye, bye_address)
        assert not any(r.startswith('BYE ') for r in pump(.5)), 'remaining participant was also disconnected'
        command('/hangupall')
        bye, bye_address = receive('BYE')
        assert header(bye, 'Call-ID') == header(added, 'Call-ID')
        response(bye, bye_address)
        print('PASS: single-call conference menu removes only the selected participant', flush=True)

        pump(.4)
        original, address = establish('blind-original')
        destination(32792, 'Blind Transfer', f'sip:blind-target@127.0.0.1:{port}')
        refer, refer_address = receive('REFER')
        assert 'Replaces=' not in header(refer, 'Refer-To')
        assert f'blind-target@127.0.0.1:{port}' in header(refer, 'Refer-To')
        response(refer, refer_address, 202, 'Accepted')
        notify_transfer(refer, refer_address, '200 OK')
        results = pump(.8)
        assert any(r.startswith('BYE ') and header(r, 'Call-ID') == header(original, 'Call-ID') for r in results)
        print('PASS: blind transfer retains full destination URI and uses plain REFER', flush=True)
    finally:
        command('/hangupall')
        peer.close()
        rtp.close()


if __name__ == '__main__':
    run_tests()
