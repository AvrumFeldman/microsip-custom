"""Exercise an isolated MicroSIP instance with a synthetic localhost SIP peer.

Usage: python tests/smoke_calls.py PID PATH_TO_TEST_LOG
The instance must use a blank local account and callAudioMode=2 restricted to
AudioFocusTests.exe. No public phone numbers, accounts, or recordings are used.
"""
import ctypes as c
from ctypes import wintypes as w
import pathlib
import re
import socket
import sys
import time
import uuid

user = c.WinDLL('user32', use_last_error=True)
kernel = c.WinDLL('kernel32', use_last_error=True)
callback_type = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
user.EnumWindows.argtypes = [callback_type, w.LPARAM]
user.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
user.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
user.SendMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
user.SendMessageW.restype = w.LPARAM

class CopyData(c.Structure):
    _fields_ = [('data', c.c_size_t), ('size', w.DWORD), ('pointer', c.c_void_p)]

class ProcessEntry(c.Structure):
    _fields_ = [('size', w.DWORD), ('usage', w.DWORD), ('pid', w.DWORD),
                ('heap', c.c_size_t), ('module', w.DWORD), ('threads', w.DWORD),
                ('parent', w.DWORD), ('priority', w.LONG), ('flags', w.DWORD),
                ('exe', w.WCHAR * 260)]

class AudioStatus(c.Structure):
    # Read-only diagnostics from audio/AudioGuardProtocol.h. The helper stays
    # alive while idle, so its process lifetime is not evidence of muting.
    _fields_ = [('version', w.LONG), ('state', w.LONG),
                ('capture_released', w.LONG), ('observer_available', w.LONG),
                ('parent', w.LONG)]

kernel.CreateToolhelp32Snapshot.argtypes = [w.DWORD, w.DWORD]
kernel.CreateToolhelp32Snapshot.restype = w.HANDLE
kernel.Process32FirstW.argtypes = [w.HANDLE, c.POINTER(ProcessEntry)]
kernel.Process32NextW.argtypes = [w.HANDLE, c.POINTER(ProcessEntry)]
kernel.CloseHandle.argtypes = [w.HANDLE]
kernel.OpenFileMappingW.argtypes = [w.DWORD, w.BOOL, w.LPCWSTR]
kernel.OpenFileMappingW.restype = w.HANDLE
kernel.MapViewOfFile.argtypes = [w.HANDLE, w.DWORD, w.DWORD, w.DWORD, c.c_size_t]
kernel.MapViewOfFile.restype = c.c_void_p
kernel.UnmapViewOfFile.argtypes = [c.c_void_p]
pid = int(sys.argv[1])
log_path = pathlib.Path(sys.argv[2])
windows = []

@callback_type
def visit(hwnd, unused):
    owner = w.DWORD()
    user.GetWindowThreadProcessId(hwnd, c.byref(owner))
    name = c.create_unicode_buffer(256)
    user.GetClassNameW(hwnd, name, 256)
    if owner.value == pid and name.value == 'MicroSIP':
        windows.append(hwnd)
    return True

user.EnumWindows(visit, 0)
assert len(windows) == 1, f'Expected one test window, found {windows}'
window = windows[0]

def command(text):
    buffer = c.create_unicode_buffer(text)
    data = CopyData(1, c.sizeof(buffer), c.cast(buffer, c.c_void_p))
    user.SendMessageW(window, 0x4a, 0, c.addressof(data))

def audio_states():
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    entry = ProcessEntry()
    entry.size = c.sizeof(entry)
    try:
        states = []
        valid = kernel.Process32FirstW(snapshot, c.byref(entry))
        while valid:
            if entry.parent == pid and entry.exe.lower() == 'microsipaudioguard.exe':
                mapping = kernel.OpenFileMappingW(4, False, f'Local\\MicroSIPAudioGuard.Status.{entry.pid}')
                if not mapping:
                    states.append(-1)  # helper is starting; do not claim restoration
                else:
                    try:
                        view = kernel.MapViewOfFile(mapping, 4, 0, 0, c.sizeof(AudioStatus))
                        if not view:
                            states.append(-1)
                        else:
                            try:
                                status = AudioStatus.from_buffer_copy(c.string_at(view, c.sizeof(AudioStatus)))
                                assert status.version == 1 and status.parent == pid, 'unexpected audio status protocol'
                                states.append(status.state)
                            finally:
                                kernel.UnmapViewOfFile(view)
                    finally:
                        kernel.CloseHandle(mapping)
            valid = kernel.Process32NextW(snapshot, c.byref(entry))
        return states
    finally:
        kernel.CloseHandle(snapshot)

def audio_muted():
    return any(state in (1, 2, 3) for state in audio_states())

def audio_restored():
    return all(state == 0 for state in audio_states())

def eventually(predicate, message, timeout=5):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate():
            return
        time.sleep(.05)
    raise AssertionError(message)

def log():
    return log_path.read_text(errors='replace')

def microphone_closed():
    text = log()
    return text.rfind('WMME capture stream started') < text.rfind('Stopped WMME capture stream') or 'WMME capture stream started' not in text

peer = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
peer.bind(('127.0.0.1', 0))
peer.settimeout(5)
port = peer.getsockname()[1]
rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rtp.bind(('127.0.0.1', 0))
sdp = ('v=0\r\no=test 1 1 IN IP4 127.0.0.1\r\ns=Local test\r\n'
       'c=IN IP4 127.0.0.1\r\nt=0 0\r\n'
       f'm=audio {rtp.getsockname()[1]} RTP/AVP 0 8 101\r\n'
       'a=rtpmap:0 PCMU/8000\r\na=rtpmap:8 PCMA/8000\r\n'
       'a=rtpmap:101 telephone-event/8000\r\na=sendrecv\r\n')

def receive(method, new_dialog=False):
    while True:
        data, address = peer.recvfrom(65535)
        message = data.decode(errors='replace')
        existing_dialog = bool(re.search(r'^To:.*;tag=', message, re.M | re.I))
        if message.startswith(method + ' ') and not (new_dialog and existing_dialog):
            return message, address
        if message.startswith('INVITE ') and existing_dialog:
            response(message, address, 200, 'OK', sdp)

def response(request, address, code, reason, body=''):
    headers = []
    for line in request.split('\r\n')[1:]:
        if ':' not in line:
            continue
        key = line.split(':', 1)[0].lower()
        if key in ('via', 'from', 'to', 'call-id', 'cseq'):
            if key == 'to' and ';tag=' not in line:
                line += ';tag=local-test'
            headers.append(line)
    headers.extend([f'Contact: <sip:peer@127.0.0.1:{port}>'])
    if body:
        headers.append('Content-Type: application/sdp')
    headers.append(f'Content-Length: {len(body)}')
    packet = f'SIP/2.0 {code} {reason}\r\n' + '\r\n'.join(headers) + '\r\n\r\n' + body
    peer.sendto(packet.encode(), address)

def pump(seconds):
    # PJSIP can also send an automatic contact-update re-INVITE. Service all
    # dialog transactions, rather than mistaking one for a new outgoing call.
    end = time.monotonic() + seconds
    peer.settimeout(.1)
    try:
        while time.monotonic() < end:
            try:
                data, address = peer.recvfrom(65535)
            except socket.timeout:
                continue
            message = data.decode(errors='replace')
            if message.startswith('INVITE '):
                response(message, address, 200, 'OK', sdp)
            elif message.startswith('BYE '):
                response(message, address, 200, 'OK')
    finally:
        peer.settimeout(5)

def header(message, name):
    return re.search(r'^' + re.escape(name) + r':\s*(.*)\r?$', message, re.M | re.I).group(1).strip()

def remote_bye(invite, address):
    to = header(invite, 'To')
    if ';tag=' not in to:
        to += ';tag=local-test'
    contact = header(invite, 'Contact').split('<', 1)[1].split('>', 1)[0]
    request = (f'BYE {contact} SIP/2.0\r\n'
               f'Via: SIP/2.0/UDP 127.0.0.1:{port};branch=z9hG4bK{uuid.uuid4().hex}\r\n'
               f'From: {to}\r\nTo: {header(invite, "From")}\r\n'
               f'Call-ID: {header(invite, "Call-ID")}\r\nCSeq: 90 BYE\r\n'
               'Max-Forwards: 70\r\nContent-Length: 0\r\n\r\n')
    peer.sendto(request.encode(), address)

def incoming_call():
    call_id = uuid.uuid4().hex
    from_header = f'<sip:incoming@127.0.0.1:{port}>;tag=caller'
    invite = (f'INVITE sip:custom-test@127.0.0.1:5096 SIP/2.0\r\n'
              f'Via: SIP/2.0/UDP 127.0.0.1:{port};branch=z9hG4bK{uuid.uuid4().hex}\r\n'
              f'From: {from_header}\r\nTo: <sip:custom-test@127.0.0.1>\r\n'
              f'Call-ID: {call_id}\r\nCSeq: 1 INVITE\r\nMax-Forwards: 70\r\n'
              f'Contact: <sip:incoming@127.0.0.1:{port}>\r\nContent-Type: application/sdp\r\n'
              f'Content-Length: {len(sdp)}\r\n\r\n{sdp}')
    peer.sendto(invite.encode(), ('127.0.0.1', 5096))
    return invite, call_id, from_header

def incoming_response(call_id, status):
    while True:
        raw, address = peer.recvfrom(65535)
        reply = raw.decode(errors='replace')
        if reply.startswith(f'SIP/2.0 {status}') and call_id in reply:
            return reply, address
        if reply.startswith('INVITE ') and ';tag=' in header(reply, 'To'):
            response(reply, address, 200, 'OK', sdp)

def cancel_incoming(invite, call_id):
    # CANCEL must reuse the INVITE transaction's branch and original To.
    cancel = ('CANCEL sip:custom-test@127.0.0.1:5096 SIP/2.0\r\n'
              + '\r\n'.join(f'{name}: {header(invite, name)}'
                            for name in ('Via', 'From', 'To', 'Call-ID'))
              + '\r\nCSeq: 1 CANCEL\r\nMax-Forwards: 70\r\nContent-Length: 0\r\n\r\n')
    peer.sendto(cancel.encode(), ('127.0.0.1', 5096))
    ended, address = incoming_response(call_id, 487)
    ack = ('ACK sip:custom-test@127.0.0.1:5096 SIP/2.0\r\n'
           + '\r\n'.join(f'{name}: {header(ended, name)}'
                         for name in ('Via', 'From', 'To', 'Call-ID'))
           + '\r\nCSeq: 1 ACK\r\nMax-Forwards: 70\r\nContent-Length: 0\r\n\r\n')
    peer.sendto(ack.encode(), address)
    pump(.2)


try:
    eventually(microphone_closed, 'previous test microphone still open')
    # Exercise ringing before any keypad/call has selected the sound mode.
    # Repeat the hunt-group pattern, where another phone answers and cancels
    # this leg without the user ever accepting it.
    baseline = len(log())
    for attempt in range(3):
        incoming, incoming_id, incoming_from = incoming_call()
        incoming_response(incoming_id, 180)
        time.sleep(.3)
        assert audio_restored(), 'unanswered incoming ringing started muting'
        assert 'WMME capture stream started' not in log()[baseline:], 'unanswered hunt-group ringing opened microphone'
        cancel_incoming(incoming, incoming_id)
    print('PASS: repeated incoming SDP/ringing/CANCEL leaves capture closed, including first sound after startup', flush=True)

    baseline = len(log())
    command('/dtmf:1')
    eventually(lambda: '(tonegen) transmitting to port 0' in log()[baseline:], 'keypad tone did not reach playback')
    assert log().rfind('Opening sound device (speaker only)') > log().rfind('Opening sound device (speaker + mic)'), 'keypad did not use playback-only sound'
    assert 'Sound recorder' not in log()[baseline:]
    assert 'capture stream started' not in log()[baseline:]
    assert audio_restored(), 'keypad started muting other apps'
    print('PASS: keypad tone uses speaker-only mode, no microphone or app muting', flush=True)

    command(f'sip:peer@127.0.0.1:{port}')
    invite, address = receive('INVITE', new_dialog=True)
    response(invite, address, 180, 'Ringing')
    eventually(audio_muted, 'outgoing call did not start muting')
    response(invite, address, 200, 'OK', sdp)
    receive('ACK')
    pump(.4)
    eventually(lambda: 'WMME capture stream started' in log()[baseline:], 'call did not enable microphone')
    during = len(log())
    command('/dtmf:2')
    time.sleep(.3)
    assert 'mode=1' not in log()[during:], 'in-call digit downgraded the microphone'
    command('msip:hold')
    pump(.6)
    assert audio_muted(), 'hold released muting'
    command('msip:hold')
    pump(.6)
    assert audio_muted(), 'resume released muting'
    command('/hangupall')
    bye, address = receive('BYE')
    # Leave BYE unanswered first: local hangup must close capture immediately.
    # Music restoration follows actual transport recovery, independently of
    # this SIP acknowledgment. This is a test deadline, not a release delay.
    eventually(microphone_closed, 'unacknowledged local hangup kept microphone open', timeout=2)
    eventually(audio_restored, 'unacknowledged local hangup did not finish audio recovery', timeout=15)
    response(bye, address, 200, 'OK')
    eventually(audio_restored, 'hangup did not restore other audio', timeout=15)
    eventually(lambda: 'Stopped WMME capture stream' in log()[baseline:], 'microphone not released')
    print('PASS: outgoing answer, microphone transition, DTMF, hold/resume, unacknowledged hangup', flush=True)

    command(f'sip:peer@127.0.0.1:{port}')
    invite, address = receive('INVITE', new_dialog=True)
    eventually(audio_muted, 'failed call test did not start muting')
    response(invite, address, 486, 'Busy Here')
    receive('ACK')
    eventually(audio_restored, 'busy response did not restore other audio', timeout=15)
    print('PASS: rejected outgoing call releases app muting', flush=True)

    command(f'sip:first@127.0.0.1:{port}')
    first, first_address = receive('INVITE', new_dialog=True)
    response(first, first_address, 200, 'OK', sdp)
    receive('ACK')
    pump(.4)
    command(f'sip:second@127.0.0.1:{port}')
    second, second_address = receive('INVITE', new_dialog=True)
    response(second, second_address, 200, 'OK', sdp)
    receive('ACK')
    pump(.4)
    remote_bye(second, second_address)
    pump(.4)
    assert audio_muted(), 'ending one overlapping call restored music too early'
    command('/hangupall')
    pump(.4)
    eventually(audio_restored, 'last overlapping call did not restore audio', timeout=15)
    print('PASS: overlapping calls retain muting until the last call ends', flush=True)
    eventually(microphone_closed, 'microphone not released after overlapping calls')

    incoming_baseline = len(log())
    incoming, incoming_id, incoming_from = incoming_call()
    ringing, incoming_address = incoming_response(incoming_id, 180)
    time.sleep(.3)
    assert audio_restored(), 'unanswered incoming call started muting'
    assert 'WMME capture stream started' not in log()[incoming_baseline:], 'ringing opened microphone'
    command('/answer')
    answered, incoming_address = incoming_response(incoming_id, 200)
    ack = (f'ACK sip:custom-test@127.0.0.1:5096 SIP/2.0\r\n'
           f'Via: SIP/2.0/UDP 127.0.0.1:{port};branch=z9hG4bK{uuid.uuid4().hex}\r\n'
           f'From: {incoming_from}\r\nTo: {header(answered, "To")}\r\n'
           f'Call-ID: {incoming_id}\r\nCSeq: 1 ACK\r\nMax-Forwards: 70\r\nContent-Length: 0\r\n\r\n')
    peer.sendto(ack.encode(), incoming_address)
    eventually(audio_muted, 'answered incoming call did not start muting')
    eventually(lambda: 'WMME capture stream started' in log()[incoming_baseline:], 'incoming answer did not open microphone')
    command('/hangupall')
    pump(.4)
    eventually(audio_restored, 'incoming hangup did not restore audio', timeout=15)
    eventually(microphone_closed, 'incoming hangup kept microphone open', timeout=2)
    print('PASS: incoming ringing leaves microphone closed; answer/hangup activate and release audio', flush=True)

    command(f'sip:active@127.0.0.1:{port}')
    active, active_address = receive('INVITE', new_dialog=True)
    response(active, active_address, 200, 'OK', sdp)
    receive('ACK')
    pump(.4)
    incoming, incoming_id, incoming_from = incoming_call()
    incoming_response(incoming_id, 180)
    assert audio_muted(), 'call waiting interrupted active-call muting'
    command('/hangupall')
    bye, active_address = receive('BYE')
    eventually(microphone_closed, 'pending BYE plus ringing call retained microphone', timeout=2)
    eventually(audio_restored, 'unanswered call waiting retained music muting after audio recovery', timeout=15)
    response(bye, active_address, 200, 'OK')
    cancel_incoming(incoming, incoming_id)
    print('PASS: last answered call releases capture while another unanswered call is still ringing', flush=True)
finally:
    command('/hangupall')
    command('/hangupincoming')
    peer.close()
    rtp.close()
