"""Check reference fixtures independently of both the MAVLink C library and our parser."""
import json
from pathlib import Path
import struct
import math
import sys

project = Path(__file__).resolve().parents[1]
root = Path(sys.argv[1]) if len(sys.argv) > 1 else project / 'tests/fixtures/v1'
manifest = json.loads((root / 'manifest.json').read_text(encoding='utf-8'))
reference = json.loads((project / 'third_party/mavlink/UPSTREAM.json').read_text(encoding='utf-8-sig'))
assert manifest['reference_commit'] == reference['commit']
extras = {0: 50, 1: 124, 30: 39, 33: 104, 77: 143, 76: 152, 4: 237}

def crc16(data):
    # Bitwise reflected CRC-16/MCRF4XX; C reference uses a different table-free update formula.
    value = 0xffff
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0x8408 if value & 1 else 0)
    return value

assert crc16(b'123456789') == 0x6f91

def decode(raw):
    p = raw[10:10 + raw[1]].ljust(255, b'\x00')
    msgid = int.from_bytes(raw[7:10], 'little')
    if msgid == 0:
        values = struct.unpack_from('<IBBBBB', p)
        return dict(zip(('custom_mode', 'type', 'autopilot', 'base_mode', 'system_status'), values[:5]))
    if msgid == 1:
        voltage, = struct.unpack_from('<H', p, 14)
        percent, = struct.unpack_from('<b', p, 30)
        return {'battery_voltage_v': None if voltage == 65535 else voltage / 1000,
                'battery_remaining_percent': None if percent == -1 else percent}
    if msgid == 30:
        return dict(zip(('roll_rad', 'pitch_rad', 'yaw_rad'), struct.unpack_from('<fff', p, 4)))
    if msgid == 33:
        lat, lon, alt, rel = struct.unpack_from('<iiii', p, 4)
        return dict(zip(('latitude_deg', 'longitude_deg', 'altitude_msl_m', 'relative_altitude_m'),
                        (lat / 1e7, lon / 1e7, alt / 1000, rel / 1000)))
    if msgid == 77:
        return dict(zip(('command', 'result', 'progress', 'result_param2', 'target_system', 'target_component'),
                        struct.unpack_from('<HBBiBB', p)))
    return None

stream = (root / 'valid.bin').read_bytes()
cursor = 0
for item in manifest['frames']:
    raw = (root / (item['name'] + '.bin')).read_bytes()
    assert raw.hex() == item['raw_hex']
    assert raw[0] == 0xfd and raw[2] == 0 and len(raw) == raw[1] + 12
    assert cursor == item['offset'] and len(raw) == item['length']
    assert stream[cursor:cursor + len(raw)] == raw
    cursor += len(raw)
    assert list(raw[4:7]) == [item['seq'], item['sysid'], item['compid']]
    assert int.from_bytes(raw[7:10], 'little') == item['msgid']
    assert crc16(raw[1:-2] + bytes([extras[item['msgid']]])) == int.from_bytes(raw[-2:], 'little')
    actual, expected = decode(raw), item['expected']
    assert (actual is None) == (expected is None)
    if expected is not None:
        for field, value in expected.items():
            assert math.isclose(actual[field], value, rel_tol=1e-6, abs_tol=1e-6), (item['name'], field)
assert cursor == len(stream)
assert decode((root / 'battery_unknown.bin').read_bytes()) == {'battery_voltage_v': None, 'battery_remaining_percent': None}
assert decode((root / 'attitude_zero_trimmed.bin').read_bytes()) == {'roll_rad': 0, 'pitch_rad': 0, 'yaw_rad': 0}
for path in root.glob('*.json'):
    json.loads(path.read_text(encoding='utf-8'))
print(f"Independent Python verification passed: {len(manifest['frames'])} frames, bitwise CRC and decoded fields")
