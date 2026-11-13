"""Check fake_gateway frame boundaries, delayed ACKs and session shutdown over loopback."""
import argparse
from contextlib import contextmanager
import csv
import hashlib
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import time

sys.dont_write_bytecode = True
from command_retry_test import ROOT, crc_x25

FIXTURE = ROOT / 'tests/fixtures/v1/valid.bin'
MANIFEST = json.loads((FIXTURE.parent / 'manifest.json').read_text(encoding='utf-8'))
FIXTURE_FRAMES = [bytes.fromhex(frame['raw_hex']) for frame in MANIFEST['frames']]


def command_long(confirmation=0):
    payload = struct.pack('<7fHBBB', *([0.0] * 7), 21, 1, 1, confirmation)
    body = bytes([len(payload), 0, 0, 40 + confirmation, 255, 190, 76, 0, 0]) + payload
    return b'\xfd' + body + struct.pack('<H', crc_x25(body + bytes([152])))


def split_stream(data):
    frames = []
    at = 0
    while at < len(data):
        if len(data) - at < 12 or data[at] != 0xfd or data[at + 2] != 0:
            raise AssertionError(f'Broken MAVLink frame header at byte {at}')
        size = 12 + data[at + 1]
        raw = data[at:at + size]
        if len(raw) != size:
            raise AssertionError(f'Truncated frame at byte {at}')
        msgid = int.from_bytes(raw[7:10], 'little')
        if msgid == 77:
            if crc_x25(raw[1:-2] + bytes([143])) != int.from_bytes(raw[-2:], 'little'):
                raise AssertionError(f'Broken COMMAND_ACK CRC at byte {at}')
        elif raw not in FIXTURE_FRAMES:
            raise AssertionError(f'Telemetry bytes changed at byte {at}')
        frames.append(raw)
        at += size
    return frames


def read_exact(connection, length):
    data = b''
    while len(data) < length:
        part = connection.recv(length - len(data))
        if not part:
            raise AssertionError('Server closed before all expected bytes arrived')
        data += part
    return data


def stop_process(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=2)


@contextmanager
def server(binary, fixture, output, name, mode='accept', chunk=7, loop=False, connect=True):
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    with (output / f'{name}_server.log').open('wb') as log:
        args = [str(binary), str(fixture), '--port', str(port), '--chunk', str(chunk),
                '--ack', mode, '--once']
        if loop:
            args.append('--loop')
        process = subprocess.Popen(args, cwd=ROOT, stdout=log, stderr=log)
        connection = None
        try:
            deadline = time.monotonic() + 4
            while connection is None:
                if not connect:
                    if 'doi client o cong' in (output / f'{name}_server.log').read_text(errors='replace'):
                        break
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError('Server did not become ready')
                    time.sleep(0.02)
                    continue
                try:
                    connection = socket.create_connection(('127.0.0.1', port), timeout=0.1)
                except OSError:
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise
                    time.sleep(0.02)
            if connection is not None:
                connection.settimeout(6)
            yield process, connection, port
        finally:
            if connection is not None:
                connection.close()
            try:
                process.wait(timeout=0.5)
            except subprocess.TimeoutExpired:
                stop_process(process)


def frame_boundaries(binary, output, chunk):
    name = f'frames_chunk_{chunk}'
    with server(binary, FIXTURE, output, name, chunk=chunk) as (_, connection, _):
        first = read_exact(connection, 1)
        connection.sendall(command_long())
        wire = first + read_exact(connection, FIXTURE.stat().st_size + 22 - 1)
        (output / f'{name}.bin').write_bytes(wire)
        frames = split_stream(wire)
        telemetry = [raw for raw in frames if raw in FIXTURE_FRAMES]
        generated = [raw for raw in frames if raw not in FIXTURE_FRAMES]
        if telemetry != FIXTURE_FRAMES or len(generated) != 1:
            raise AssertionError('Expected the exact seven fixture frames and one generated ACK')
        if int.from_bytes(generated[0][10:12], 'little') != 21:
            raise AssertionError('ACK did not match the command')
    return {'frames': len(frames), 'telemetry_exact': True}


def delayed_ack(binary, output):
    with server(binary, FIXTURE, output, 'late_ack', mode='late', chunk=4096) as (_, connection, _):
        read_exact(connection, FIXTURE.stat().st_size)
        start = time.monotonic()
        connection.sendall(command_long())
        wire = read_exact(connection, 22)
        elapsed = time.monotonic() - start
        (output / 'late_ack.bin').write_bytes(wire)
        split_stream(wire)
        if elapsed < 3.5:
            raise AssertionError(f'ACK arrived after {elapsed:.3f}s, before the default three-send timeout')
    return {'ack_after_seconds': elapsed}


def disconnect_delayed(binary, output):
    with server(binary, FIXTURE, output, 'disconnect_delayed', mode='late', chunk=4096) as (process, connection, _):
        read_exact(connection, FIXTURE.stat().st_size)
        connection.sendall(command_long())
        time.sleep(0.1)
        start = time.monotonic()
        connection.shutdown(socket.SHUT_RDWR)
        connection.close()
        process.wait(timeout=1.5)
        elapsed = time.monotonic() - start
        if process.returncode != 0:
            raise AssertionError(f'Server returned {process.returncode}')
    return {'shutdown_seconds': elapsed}


def disconnect_mid_frame(binary, output):
    with server(binary, FIXTURE, output, 'disconnect_mid_frame', chunk=1, loop=True) as (process, connection, _):
        read_exact(connection, 1)
        start = time.monotonic()
        connection.shutdown(socket.SHUT_RDWR)
        connection.close()
        process.wait(timeout=1.5)
        elapsed = time.monotonic() - start
        if process.returncode != 0:
            raise AssertionError(f'Server returned {process.returncode}')
    return {'shutdown_seconds': elapsed}


def loop_frames(binary, output):
    with server(binary, FIXTURE, output, 'loop_frames', chunk=4096, loop=True) as (_, connection, _):
        first = read_exact(connection, FIXTURE.stat().st_size)
        connection.sendall(command_long())
        ack = read_exact(connection, 22)
        second = read_exact(connection, FIXTURE.stat().st_size)
        if first != FIXTURE.read_bytes() or second != first:
            raise AssertionError('Loop changed fixture bytes')
        split_stream(ack)
    return {'telemetry_frames': 14, 'generated_acks': 1}


def queue_tests(binary, output):
    completed = subprocess.run([str(binary)], cwd=ROOT, capture_output=True, timeout=12)
    (output / 'send_queue_tests.log').write_bytes(completed.stdout + completed.stderr)
    if completed.returncode != 0 or b'7/7 transport tests passed' not in completed.stdout:
        raise AssertionError(f'SendQueue tests failed (exit {completed.returncode})')
    return {'checks': '7/7'}


def client_ack(binary, client, output, mode, ack_mode):
    name = f'client_{mode}_{ack_mode}'
    fixture = output / 'heartbeat_only.bin'
    fixture.write_bytes(FIXTURE_FRAMES[0])
    run_id = f'c_fake_{time.time_ns()}_{mode}_{ack_mode}'
    with server(binary, fixture, output, name, mode=ack_mode, connect=False) as (gateway, _, port):
        with (output / f'{name}_client.log').open('wb') as log:
            process = subprocess.Popen([str(client), '127.0.0.1', str(port), '--role', 'controller',
                '--mode', mode, '--run-id', run_id], cwd=ROOT, stdin=subprocess.PIPE, stdout=log, stderr=log)
            try:
                process.stdin.write(b'arm\n')
                process.stdin.flush()
                deadline = time.monotonic() + 4
                server_log = output / f'{name}_server.log'
                while 'COMMAND_LONG command=400' not in server_log.read_text(errors='replace'):
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise AssertionError('Server did not receive client command')
                    time.sleep(0.02)
                duration = {'accept': 0.4, 'reject': 0.4, 'progress': 1.9, 'none': 3.7, 'late': 4.5}[ack_mode]
                until = time.monotonic() + duration
                while time.monotonic() < until:
                    if process.poll() is not None:
                        raise AssertionError('Client ended before the transaction completed')
                    time.sleep(0.02)
                process.stdin.write(b'quit\n')
                process.stdin.close()
                process.wait(timeout=5)
                gateway.wait(timeout=2)
                if process.returncode != 0 or gateway.returncode != 0:
                    raise AssertionError('Client/server exited with an error')
            finally:
                if process.stdin and not process.stdin.closed:
                    process.stdin.close()
                stop_process(process)
    command_log = ROOT / f'output/runs/{run_id}/controller/commands.csv'
    with command_log.open(newline='', encoding='utf-8') as log:
        events = [row['event'] for row in csv.DictReader(log)]
    expected = {
        'accept': ['submitted', 'queued', 'ack_accepted'],
        'reject': ['submitted', 'queued', 'ack_rejected'],
        'progress': ['submitted', 'queued', 'ack_in_progress', 'ack_accepted'],
        'none': ['submitted', 'queued', 'queued', 'queued', 'final_timeout'],
        'late': ['submitted', 'queued', 'queued', 'queued', 'final_timeout', 'ack_late'],
    }[ack_mode]
    if events != expected:
        raise AssertionError(f'Events {events}, expected {expected}')
    return {'run_id': run_id, 'events': events, 'client_exit': 0, 'server_exit': 0}


def main():
    names = ['frames_chunk_1', 'frames_chunk_7', 'loop_frames', 'late_ack',
             'disconnect_delayed', 'disconnect_mid_frame', 'send_queue']
    names += [f'client_{mode}_{ack}' for mode in ['B0', 'B1', 'B2']
              for ack in ['accept', 'reject', 'progress', 'none', 'late']]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', type=Path, default=ROOT / 'build/bin/fake_gateway.exe')
    parser.add_argument('--client', type=Path, default=ROOT / 'build/bin/c_client.exe')
    parser.add_argument('--queue-tests', type=Path, default=ROOT / 'build/bin/send_queue_tests.exe')
    parser.add_argument('--output', type=Path, default=ROOT / f'output/test_results/c_transport_{time.time_ns()}')
    parser.add_argument('--cases', nargs='+', choices=names, default=names)
    args = parser.parse_args()
    binary, output = args.server.resolve(), args.output.resolve()
    if not binary.is_file():
        parser.error(f'Build C first: {binary}')
    output.mkdir(parents=True, exist_ok=False)
    tests = {
        'frames_chunk_1': lambda: frame_boundaries(binary, output, 1),
        'frames_chunk_7': lambda: frame_boundaries(binary, output, 7),
        'loop_frames': lambda: loop_frames(binary, output),
        'late_ack': lambda: delayed_ack(binary, output),
        'disconnect_delayed': lambda: disconnect_delayed(binary, output),
        'disconnect_mid_frame': lambda: disconnect_mid_frame(binary, output),
        'send_queue': lambda: queue_tests(args.queue_tests.resolve(), output),
    }
    for mode in ['B0', 'B1', 'B2']:
        for ack in ['accept', 'reject', 'progress', 'none', 'late']:
            tests[f'client_{mode}_{ack}'] = lambda mode=mode, ack=ack: client_ack(
                binary, args.client.resolve(), output, mode, ack)
    results = []
    for name in args.cases:
        result = {'case': name}
        try:
            result.update(tests[name]())
            result['passed'] = True
        except Exception as error:
            result.update(passed=False, error=f'{type(error).__name__}: {error}')
        results.append(result)
        print(f"{'PASS' if result['passed'] else 'FAIL'} {name}: {result.get('error', 'OK')}", flush=True)
    report = {'server': str(binary), 'server_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
              'passed': sum(result['passed'] for result in results), 'total': len(results), 'cases': results}
    (output / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f"{report['passed']}/{report['total']} cases passed. Evidence: {output}", flush=True)
    return 0 if report['passed'] == report['total'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
