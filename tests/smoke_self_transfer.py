# SPDX-License-Identifier: GPL-2.0-or-later
"""Loopback PBX regression for an attended transfer back to the local account.

Usage: python tests/smoke_self_transfer.py PID [TRANSCRIPT.json] [--incoming-original] [--repeat N]
The PID must be an isolated portable MicroSIP instance with sourcePort=5096,
singleMode=1, callWaiting=1, and local username=custom-test. This test sends
SIP only to 127.0.0.1 and never reads the installed account configuration.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
from pathlib import Path
import socket
import sys
import time
import uuid

import smoke_call_controls as sip

sip.user.IsWindowVisible.argtypes = [w.HWND]
sip.user.IsWindowEnabled.argtypes = [w.HWND]
sip.peer.settimeout(.04)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('pid', type=int)
parser.add_argument('transcript', nargs='?')
parser.add_argument('--incoming-original', action='store_true')
parser.add_argument('--repeat', type=int, default=1)
options = parser.parse_args()


class LoopbackPBX:
    def __init__(self):
        self.original = None
        self.original_id = None
        self.original_address = None
        self.original_answered = False
        self.consult = None
        self.consult_address = None
        self.consult_id = None
        self.incoming = None
        self.incoming_id = None
        self.incoming_tagged_to = None
        self.sequence = 1
        self.records = []
        self.statuses = []
        self.cancel_relayed = False
        self.bye_relayed = False
        self.preserve_original = True
        self.last_ping = 0

    def ping(self):
        if time.monotonic() - self.last_ping > .15:
            sip.send(sip.main, 0)  # WM_NULL with a two-second timeout
            self.last_ping = time.monotonic()

    def request_to_incoming(self, method, same_transaction=False):
        assert self.incoming
        target = 'sip:custom-test@127.0.0.1:5096'
        branch = sip.header(self.incoming, 'Via')
        if method == 'BYE' or (method == 'ACK' and not same_transaction):
            branch = f'SIP/2.0/UDP 127.0.0.1:{sip.port};branch=z9hG4bK{uuid.uuid4().hex}'
        to = self.incoming_tagged_to or sip.header(self.incoming, 'To')
        if method == 'CANCEL':
            to = sip.header(self.incoming, 'To')
        sequence = self.sequence + (1 if method == 'BYE' else 0)
        packet = (f'{method} {target} SIP/2.0\r\nVia: {branch}\r\n'
                  f'From: {sip.header(self.incoming, "From")}\r\nTo: {to}\r\n'
                  f'Call-ID: {self.incoming_id}\r\nCSeq: {sequence} {method}\r\n'
                  f'Max-Forwards: 70\r\nContact: <sip:relay@127.0.0.1:{sip.port}>\r\n'
                  'Content-Length: 0\r\n\r\n')
        sip.peer.sendto(packet.encode(), ('127.0.0.1', 5096))

    def receive_one(self):
        self.ping()
        try:
            packet, address = sip.peer.recvfrom(65535)
        except socket.timeout:
            return
        message = packet.decode(errors='replace')
        self.records.append(message)
        call_id = sip.header(message, 'Call-ID')
        first = message.split('\r\n', 1)[0]
        print(f'SIP {first} [{call_id}]', flush=True)
        if message.startswith('SIP/2.0 '):
            if (call_id == self.original_id and options.incoming_original
                    and sip.header(message, 'CSeq').endswith(' INVITE')):
                if int(message.split()[1]) == 200:
                    packet = (
                        'ACK sip:custom-test@127.0.0.1:5096 SIP/2.0\r\n'
                        f'Via: SIP/2.0/UDP 127.0.0.1:{sip.port};branch=z9hG4bK{uuid.uuid4().hex}\r\n'
                        f'From: {sip.header(self.original, "From")}\r\nTo: {sip.header(message, "To")}\r\n'
                        f'Call-ID: {self.original_id}\r\nCSeq: 1 ACK\r\n'
                        'Max-Forwards: 70\r\nContent-Length: 0\r\n\r\n')
                    sip.peer.sendto(packet.encode(), address)
                    self.original_answered = True
                return
            if call_id != self.incoming_id or not sip.header(message, 'CSeq').endswith(' INVITE'):
                return
            code = int(message.split()[1])
            self.statuses.append(code)
            if code >= 180:
                self.incoming_tagged_to = sip.header(message, 'To')
            body = message.split('\r\n\r\n', 1)[1]
            reason = first.split(' ', 2)[2]
            if code >= 200:
                self.request_to_incoming('ACK', same_transaction=code >= 300)
            if code > 100:
                sip.response(self.consult, self.consult_address, code, reason, body)
            return
        method = message.split(' ', 1)[0]
        if method == 'INVITE':
            if call_id == self.original_id or ';tag=' in sip.header(message, 'To'):
                sip.response(message, address, body=sip.sdp)
            elif self.consult is None:
                self.consult = message
                self.consult_address = address
                self.consult_id = call_id
                sip.response(message, address, 100, 'Trying')
            # Fresh consultation retransmissions are answered by the relayed leg.
        elif method == 'CANCEL':
            sip.response(message, address)
            if call_id == self.consult_id:
                sip.response(self.consult, self.consult_address, 487, 'Request Terminated')
                if not self.cancel_relayed:
                    self.cancel_relayed = True
                    self.request_to_incoming('CANCEL')
        elif method == 'BYE':
            sip.response(message, address)
            assert not (self.preserve_original and call_id == self.original_id), 'Original caller was disconnected during self consultation'
            if not self.bye_relayed:
                if call_id == self.consult_id:
                    self.bye_relayed = True
                    self.request_to_incoming('BYE')
                elif call_id == self.incoming_id:
                    self.bye_relayed = True
                    sip.remote_bye(self.consult, self.consult_address)
        elif method in ('OPTIONS', 'NOTIFY'):
            sip.response(message, address)

    def pump(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.receive_one()

    def wait(self, predicate, message, timeout=6):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.receive_one()
            value = predicate()
            if value:
                return value
        raise AssertionError(message + f'; incoming statuses={self.statuses}')

    def establish_original(self):
        if options.incoming_original:
            self.original_id = 'original-incoming-' + uuid.uuid4().hex
            self.original = self.make_incoming_invite(self.original_id, 'original', 'Original caller')
            self.original_address = ('127.0.0.1', 5096)
            sip.peer.sendto(self.original.encode(), self.original_address)
            dialog = self.wait(self.ringing_dialog, 'Original incoming call did not ring')
            sip.send(dialog, 0x111, 1000, 0)
            self.wait(lambda: self.original_answered, 'Original incoming call did not answer')
            self.pump(.5)
            return
        # There are no other calls yet, so the common one-leg helper is safe.
        sip.peer.settimeout(5)
        self.original, self.original_address = sip.establish('original')
        self.original_id = sip.header(self.original, 'Call-ID')
        sip.peer.settimeout(.04)
        self.ping()

    def begin_self_consultation(self):
        self.consult = self.consult_address = self.consult_id = None
        self.incoming = self.incoming_id = self.incoming_tagged_to = None
        self.statuses = []
        self.cancel_relayed = self.bye_relayed = False
        sip.destination(32793, 'Attended Transfer', f'sip:custom-test@127.0.0.1:{sip.port}')
        self.wait(lambda: self.consult, 'Consultation INVITE was not sent')
        self.incoming_id = 'self-loop-' + uuid.uuid4().hex
        # ParseCallSIPURI uses remote_info (From for UAS, To for UAC), with
        # asserted caller-ID overriding it if present. We send no such override.
        target_aor = sip.header(self.consult, 'To').split('<', 1)[1].split('>', 1)[0]
        self.incoming = self.make_incoming_invite(self.incoming_id, 'custom-test', 'Self consultation', target_aor)
        caller_aor = sip.header(self.incoming, 'From').split('<', 1)[1].split('>', 1)[0]
        assert target_aor == caller_aor, f'Self-test AOR mismatch: {target_aor} != {caller_aor}'
        sip.peer.sendto(self.incoming.encode(), ('127.0.0.1', 5096))
        dialog = self.wait(self.ringing_dialog, 'Self consultation did not expose Answer and Decline')
        assert not any(code >= 300 for code in self.statuses), f'Self consultation rejected: {self.statuses}'
        self.pump(.3)
        return dialog

    @staticmethod
    def make_incoming_invite(call_id, username, display, caller_uri=None):
        caller_uri = caller_uri or f'sip:{username}@127.0.0.1'
        return (
            'INVITE sip:custom-test@127.0.0.1:5096 SIP/2.0\r\n'
            f'Via: SIP/2.0/UDP 127.0.0.1:{sip.port};branch=z9hG4bK{uuid.uuid4().hex}\r\n'
            f'From: "{display}" <{caller_uri}>;tag=loopback-test\r\n'
            'To: <sip:custom-test@127.0.0.1:5096>\r\n'
            f'Call-ID: {call_id}\r\nCSeq: 1 INVITE\r\n'
            f'Contact: <sip:{username}@127.0.0.1:{sip.port}>\r\nMax-Forwards: 70\r\n'
            f'Content-Type: application/sdp\r\nContent-Length: {len(sip.sdp)}\r\n\r\n{sip.sdp}')

    @staticmethod
    def ringing_dialog():
        for hwnd in sip.windows():
            answer = sip.user.GetDlgItem(hwnd, 1000)
            decline = sip.user.GetDlgItem(hwnd, 1032)
            if (answer and decline and sip.user.IsWindowVisible(hwnd)
                    and sip.user.IsWindowVisible(answer) and sip.user.IsWindowVisible(decline)
                    and sip.user.IsWindowEnabled(answer) and sip.user.IsWindowEnabled(decline)):
                return hwnd

    def cancel_from_dropdown(self):
        button = sip.user.GetDlgItem(sip.dialer, 1135)
        assert sip.user.IsWindowVisible(button) and sip.user.IsWindowEnabled(button), 'Transfer button is not usable'
        sip.user.PostMessageW(sip.dialer, 0x111, 1135, 0)
        menu = self.wait(lambda: next((m for m in sip.popup_menus()
                                      if 'Cancel consultation and return' in m[2]), None),
                         'Transfer dropdown did not expose consultation cancellation')
        index = menu[2].index('Cancel consultation and return')
        assert not sip.user.GetMenuState(menu[1], index, 0x400) & 3, 'Cancel consultation is disabled'
        sip.click_menu(menu, index)

    @staticmethod
    def press_end():
        button = sip.user.GetDlgItem(sip.dialer, 1055)
        assert sip.user.IsWindowVisible(button) and sip.user.IsWindowEnabled(button), 'End button is not visible and enabled'
        sip.send(sip.dialer, 0x111, 1055, 0)

    def assert_original_resumed(self, mark):
        self.wait(lambda: any(message.startswith('INVITE ')
                              and sip.header(message, 'Call-ID') == self.original_id
                              and 'a=sendrecv' in message for message in self.records[mark:]),
                  'Original call did not resume')
        self.pump(.5)
        assert not self.ringing_dialog(), 'Incoming self-consultation popup remained after cancellation'
        assert not any(message.startswith('BYE ') and sip.header(message, 'Call-ID') == self.original_id
                       for message in self.records), 'Original caller was disconnected'
        self.ping()

    def answer_self(self):
        dialog = self.begin_self_consultation()
        sip.send(dialog, 0x111, 1000, 0)
        self.wait(lambda: 200 in self.statuses, 'Self consultation did not answer')
        self.pump(.7)
        assert not any(message.startswith('BYE ') and sip.header(message, 'Call-ID') == self.original_id
                       for message in self.records), 'Answer in single-call mode disconnected original'


def run_tests():
    pbx = LoopbackPBX()
    try:
        pbx.establish_original()
        for iteration in range(options.repeat):
            print(f'ITERATION {iteration + 1}/{options.repeat}', flush=True)
            pbx.begin_self_consultation()
            mark = len(pbx.records)
            pbx.cancel_from_dropdown()
            pbx.assert_original_resumed(mark)
            assert pbx.cancel_relayed, 'Cancel did not terminate the outgoing consultation'
            print('PASS: self consultation rings with Answer/Decline; dropdown cancellation preserves original', flush=True)

            pbx.begin_self_consultation()
            mark = len(pbx.records)
            pbx.press_end()
            pbx.assert_original_resumed(mark)
            assert pbx.cancel_relayed, 'End did not cancel the ringing consultation'
            print('PASS: visible End cancels ringing self consultation and preserves original', flush=True)

            dialog = pbx.begin_self_consultation()
            mark = len(pbx.records)
            sip.send(dialog, 0x111, 1032, 0)
            pbx.assert_original_resumed(mark)
            assert any(code >= 300 for code in pbx.statuses), 'Decline did not reject incoming leg'
            print('PASS: declining self consultation preserves and resumes original', flush=True)

            pbx.answer_self()
            mark = len(pbx.records)
            pbx.cancel_from_dropdown()
            pbx.assert_original_resumed(mark)
            assert pbx.bye_relayed, 'Answered consultation was not disconnected'
            print('PASS: answering self consultation and cancelling it preserves original in single-call mode', flush=True)

            pbx.answer_self()
            mark = len(pbx.records)
            pbx.press_end()
            pbx.assert_original_resumed(mark)
            assert pbx.bye_relayed, 'End did not disconnect the answered consultation'
            print('PASS: End on answered self consultation preserves and resumes original', flush=True)
    finally:
        try:
            pbx.preserve_original = False
            sip.command('/hangupall')
            pbx.pump(.5)
        except (TimeoutError, OSError):
            # The fixture owner can capture a dump and terminate only its own PID.
            pass
        sip.peer.close()
        sip.rtp.close()
        if options.transcript:
            Path(options.transcript).write_text(json.dumps({
                'test_pid': sip.pid, 'peer_port': sip.port,
                'incoming_original': options.incoming_original, 'iterations': options.repeat,
                'original_call_id': pbx.original_id,
                'received_sip_messages': pbx.records,
            }, indent=2), encoding='utf-8')


if __name__ == '__main__':
    run_tests()
