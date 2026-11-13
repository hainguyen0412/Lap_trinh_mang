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


def run_case(client, output, target, command, console, params, run_suffix):
    name = f'{command}_{target[0]}_{target[1]}'
    result = {'case': name, 'target': list(target), 'commands': [], 'errors': []}
    wire = bytearray()
    process = None
    try:
        with socket.socket() as server, (output / f'{name}.log').open('wb') as log:
            server.bind(('127.0.0.1', 0))
            server.listen(1)
            server.settimeout(5)
            process = subprocess.Popen([
                str(client), '127.0.0.1', str(server.getsockname()[1]),
                '--role', 'controller', '--mode', 'B1',
                '--px4-sys', str(target[0]), '--px4-comp', str(target[1]),
                '--run-id', f'c_retry_{run_suffix}_{name}',
            ], cwd=ROOT, stdin=subprocess.PIPE, stdout=log, stderr=log)
            try:
                connection, _ = server.accept()
                with connection:
                    connection.settimeout(0.1)
                    process.stdin.write((console + '\n').encode('ascii'))
                    process.stdin.flush()
                    pending = bytearray()
                    deadline = time.monotonic() + 5
                    first_command = True
                    next_heartbeat = time.monotonic()
                    heartbeat_seq = 0
                    while time.monotonic() < deadline:
                        if time.monotonic() >= next_heartbeat:
                            connection.sendall(heartbeat(target, heartbeat_seq))
                            heartbeat_seq = (heartbeat_seq + 1) % 256
                            next_heartbeat = time.monotonic() + 0.05
                        try:
                            chunk = connection.recv(4096)
                        except socket.timeout:
                            continue
                        if not chunk:
                            break
                        wire.extend(chunk)
                        pending.extend(chunk)
                        for raw in extract_frames(pending):
                            if int.from_bytes(raw[7:10], 'little') != 76:
                                continue
                            if raw[1] > 33:
                                raise ValueError('Unexpected COMMAND_LONG payload length')
                            fields = struct.unpack('<7fHBBB', raw[10:-2].ljust(33, b'\0'))
                            result['commands'].append({
                                'command': fields[7], 'params': list(fields[:7]),
                                'target_system': fields[8], 'target_component': fields[9],
                                'confirmation': fields[10],
                                'gcs_system': raw[5], 'gcs_component': raw[6], 'raw_hex': raw.hex(),
                            })
                            if first_command:
                                # Wait through all three 1-second ACK windows; never send an ACK.
                                deadline = time.monotonic() + 3.35
                                first_command = False
                    # Release the console reader before closing TCP, so cleanup can finish.
                    process.stdin.close()
                    connection.shutdown(socket.SHUT_RDWR)
                    process.wait(timeout=5)
            finally:
                if process.stdin and not process.stdin.closed:
                    process.stdin.close()
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=2)
        result['returncode'] = process.returncode
        if process.returncode != 0:
            result['errors'].append(f'Client exited with code {process.returncode}')
    except Exception as error:
        result['errors'].append(f'{type(error).__name__}: {error}')
    finally:
        (output / f'{name}.bin').write_bytes(wire)

    messages = result['commands']
    if len(messages) != 3:
        result['errors'].append(f'Expected 3 command frames; received {len(messages)}')
    for attempt, message in enumerate(messages):
        expected = {
            'command': command, 'params': params,
            'target_system': target[0], 'target_component': target[1],
            'confirmation': attempt, 'gcs_system': 255, 'gcs_component': 190,
        }
        for key, value in expected.items():
            if message[key] != value:
                result['errors'].append(f'Attempt {attempt + 1}: {key}={message[key]}, expected {value}')
    result['passed'] = not result['errors']
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client', type=Path, default=ROOT / 'build/bin/c_client.exe')
    parser.add_argument('--output', type=Path, default=ROOT / f'output/test_results/c_retry_{time.time_ns()}')
    args = parser.parse_args()
    client, output = args.client.resolve(), args.output.resolve()
    if not client.is_file():
        parser.error(f'Build C first: executable not found: {client}')
    output.mkdir(parents=True, exist_ok=False)
    run_suffix = str(time.time_ns())
    results = []
    for target in [(1, 1), (23, 42)]:
        for command, console, params in [
            (400, 'arm', [1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]),
            (22, 'takeoff 123.5', [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 123.5]),
        ]:
            result = run_case(client, output, target, command, console, params, run_suffix)
            results.append(result)
            print(f"{'PASS' if result['passed'] else 'FAIL'} {result['case']}: {len(result['commands'])} command frames", flush=True)
            for error in result['errors']:
                print(f'  {error}', flush=True)
    passed = sum(result['passed'] for result in results)
    report = {'client': str(client), 'client_sha256': hashlib.sha256(client.read_bytes()).hexdigest(),
              'telemetry_while_retrying': True,
              'passed': passed, 'total': len(results), 'cases': results}
    (output / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f'{passed}/{len(results)} cases passed. Evidence: {output}', flush=True)
    return 0 if passed == len(results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
