"""Check client retries while receiving valid telemetry, without returning ACKs."""
import argparse
import hashlib
import json
from pathlib import Path
import socket
import struct
import subprocess
import time


ROOT = Path(__file__).resolve().parents[2]
# Fixed CRC extras from the project's pinned MAVLink common/minimal headers.
CRC_EXTRAS = {0: 50, 76: 152}


def crc_x25(data):
    value = 0xffff
    for byte in data:
        tmp = byte ^ (value & 0xff)
        tmp ^= (tmp << 4) & 0xff
        value = ((value >> 8) ^ (tmp << 8) ^ (tmp << 3) ^ (tmp >> 4)) & 0xffff
    return value


def heartbeat(target, seq):
    payload = struct.pack('<IBBBBB', 0, 2, 12, 0, 4, 3)
    body = bytes([len(payload), 0, 0, seq, target[0], target[1], 0, 0, 0]) + payload
    return b'\xfd' + body + struct.pack('<H', crc_x25(body + bytes([CRC_EXTRAS[0]])))


def extract_frames(pending):
    frames = []
    while len(pending) >= 10:
        if pending[0] != 0xfd or pending[2] != 0:
            raise ValueError('Expected an unsigned MAVLink 2 frame')
        length = 12 + pending[1]
        if len(pending) < length:
            break
        raw = bytes(pending[:length])
        del pending[:length]
        msgid = int.from_bytes(raw[7:10], 'little')
        if msgid not in CRC_EXTRAS:
            raise ValueError(f'Unexpected uplink message: {msgid}')
        expected_crc = crc_x25(raw[1:-2] + bytes([CRC_EXTRAS[msgid]]))
        if int.from_bytes(raw[-2:], 'little') != expected_crc:
            raise ValueError(f'Invalid CRC for uplink message {msgid}')
        frames.append(raw)
    return frames


