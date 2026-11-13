"""Run the real A+B gateway and C clients against a local UDP protocol simulator.

The simulator accepts commands for transport testing only. It does not simulate flight.
All children belong to this invocation; existing user processes are left alone.
"""
import argparse
import csv
import json
from pathlib import Path
import socket
import struct
import subprocess
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
EXTRAS = {0: 50, 1: 124, 30: 39, 33: 104, 76: 152, 77: 143}


def crc(data):
    value = 0xffff
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0x8408 if value & 1 else 0)
    return value


def frame(msgid, payload, seq):
    header = bytes([len(payload), 0, 0, seq & 255, 1, 1]) + msgid.to_bytes(3, 'little')
    return b'\xfd' + header + payload + struct.pack('<H', crc(header + payload + bytes([EXTRAS[msgid]])))


def split(data):
    result = []
    while len(data) >= 12:
        if data[0] != 253:
            data = data[1:]; continue
        size = data[1] + 12
        if len(data) < size:
            break
        raw, data = data[:size], data[size:]
        msgid = int.from_bytes(raw[7:10], 'little')
        if msgid in EXTRAS and raw[2] == 0 and crc(raw[1:-2] + bytes([EXTRAS[msgid]])) == int.from_bytes(raw[-2:], 'little'):
            result.append((msgid, raw))
    return result


def rows(path):
    if not path.exists():
        return []
    with path.open(encoding='utf-8', newline='') as stream:
        return list(csv.DictReader(stream))


def wait_for(predicate, children, timeout=8):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        if predicate():
            return
        for name, child in children:
            if child.poll() is not None:
                raise RuntimeError(f'{name} exited early with {child.returncode}')
        time.sleep(.03)
    raise TimeoutError('Timed out waiting for readiness or command ACK; see console logs')


def free_tcp_port():
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        return listener.getsockname()[1]


class UdpSimulator:
    def __init__(self, port, rate):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(('127.0.0.1', 0)); self.socket.setblocking(False)
        self.target = ('127.0.0.1', port)
        manifest = json.loads((ROOT / 'tests/fixtures/v1/manifest.json').read_text())
        self.templates = [bytes.fromhex(item['raw_hex']) for item in manifest['frames'] if item['msgid'] in (0, 1, 30, 33)]
        self.stop_event = threading.Event()
        self.rate = rate
        self.sent = self.heartbeats = 0
        self.commands = []
        self.error = None
        self.thread = threading.Thread(target=self.run)

    def send(self, msgid, payload):
        self.socket.sendto(frame(msgid, payload, self.sent), self.target)
        self.sent += 1

    def run(self):
        index, due = 0, time.monotonic()
        try:
            while not self.stop_event.is_set():
                now = time.monotonic()
                if now >= due:
                    template = self.templates[index % len(self.templates)]
                    self.send(int.from_bytes(template[7:10], 'little'), template[10:-2])
                    index += 1; due = now + 1 / self.rate
                try:
                    data, sender = self.socket.recvfrom(65535)
                    if sender != self.target:
                        continue
                    for msgid, raw in split(data):
                        if msgid == 0:
                            self.heartbeats += 1
                        if msgid == 76:
                            payload = raw[10:-2].ljust(33, b'\0')
                            command = int.from_bytes(payload[28:30], 'little')
                            if payload[30:32] == b'\1\1' and command in (400, 22, 21):
                                self.commands.append(command)
                                self.send(77, struct.pack('<HBBiBB', command, 0, 0, 0, raw[5], raw[6]))
                except BlockingIOError:
                    pass
                self.stop_event.wait(.001)
        except Exception as error:
            self.error = repr(error)

    def close(self):
        self.stop_event.set(); self.thread.join(timeout=3); self.socket.close()
        if self.thread.is_alive():
            raise RuntimeError('Simulator did not stop')


