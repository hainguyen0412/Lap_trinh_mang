"""Check ACK handling in the real client in B0/B1/B2 using a loopback server."""
import argparse
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
from command_retry_test import ROOT, crc_x25, extract_frames, heartbeat


def ack_packet(command=400, result=0, progress=100, target_system=255,
               target_component=190, source_system=1, source_component=1,
               truncated=False, corrupt=False):
    payload = struct.pack('<HB', command, result) if truncated else struct.pack(
        '<HBBiBB', command, result, progress, 0, target_system, target_component)
    body = bytes([len(payload), 0, 0, 80, source_system, source_component, 77, 0, 0]) + payload
    raw = b'\xfd' + body + struct.pack('<H', crc_x25(body + bytes([143])))
    return raw[:-1] + bytes([raw[-1] ^ 1]) if corrupt else raw


def scenarios():
    cases = {}

    def add(name, steps, outcome, ignored=0, commands=1, observe=0.4, progress=False):
        events = ['submitted', 'queued']
        events += ['ack_ignored'] * ignored
        if progress:
            events += ['ack_in_progress']
        events += ['queued'] * (commands - 1)
        events += outcome
        cases[name] = {'steps': steps, 'events': events, 'commands': commands, 'observe': observe}

    add('accepted', [(0, {})], ['ack_accepted'])
    add('truncated_ack', [(0, {'truncated': True})], ['ack_accepted'])
    add('unspecified_system', [(0, {'target_system': 0})], ['ack_accepted'])
    add('unspecified_component', [(0, {'target_component': 0})], ['ack_accepted'])
    for name, wrong in [
        ('wrong_component', {'target_component': 189}),
        ('wrong_component_unspecified_system', {'target_system': 0, 'target_component': 189}),
        ('wrong_system', {'target_system': 77}),
        ('wrong_source_system', {'source_system': 2}),
        ('wrong_source_component', {'source_component': 2}),
        ('wrong_command', {'command': 21}),
    ]:
        add(name, [(0, wrong), (0.15, {})], ['ack_accepted'], ignored=1)
    add('bad_crc', [(0, {'corrupt': True}), (0.15, {})], ['ack_accepted'])
    add('in_progress', [(0, {'result': 5, 'progress': 30}), (1.2, {})],
        ['ack_accepted'], observe=1.5, progress=True)
    add('rejected', [(0, {'result': 2})], ['ack_rejected'])
    add('timeout', [], ['final_timeout'], commands=3, observe=3.6)
    add('disconnect', [], ['final_unknown'], observe=0.2)
    add('late_ack', [(4.0, {})], ['final_timeout', 'ack_late'], commands=3, observe=4.3)
    return cases


def run_case(client, output, mode, name, spec, suffix):
    case_id = f'{mode}_{name}'
    run_id = f'c_ack_{suffix}_{case_id}'
    result = {'case': case_id, 'mode': mode, 'run_id': run_id,
              'errors': [], 'commands': [], 'acks_sent': []}
    wire = bytearray()
    try:
        with socket.socket() as server, (output / f'{case_id}.log').open('wb') as log:
            server.bind(('127.0.0.1', 0))
            server.listen(1)
            server.settimeout(5)
            process = subprocess.Popen([
                str(client), '127.0.0.1', str(server.getsockname()[1]), '--role', 'controller',
                '--mode', mode, '--run-id', run_id,
            ], cwd=ROOT, stdin=subprocess.PIPE, stdout=log, stderr=log)
            try:
                connection, _ = server.accept()
                with connection:
                    connection.settimeout(0.03)
                    process.stdin.write(b'arm\n')
                    process.stdin.flush()
                    pending = bytearray()
                    first_command_at = None
                    deadline = time.monotonic() + 5
                    next_heartbeat = time.monotonic()
                    heartbeat_seq = 0
                    step = 0
                    while time.monotonic() < deadline:
                        now = time.monotonic()
                        if now >= next_heartbeat:
                            connection.sendall(heartbeat((1, 1), heartbeat_seq))
                            heartbeat_seq = (heartbeat_seq + 1) % 256
                            next_heartbeat = now + 0.1
                        if first_command_at is not None:
                            while step < len(spec['steps']) and now - first_command_at >= spec['steps'][step][0]:
                                raw = ack_packet(**spec['steps'][step][1])
                                connection.sendall(raw)
                                result['acks_sent'].append(raw.hex())
                                step += 1
                        try:
                            chunk = connection.recv(4096)
                        except socket.timeout:
                            continue
                        if not chunk:
                            break
                        wire.extend(chunk)
                        pending.extend(chunk)
                        for raw in extract_frames(pending):
                            if int.from_bytes(raw[7:10], 'little') == 76:
                                fields = struct.unpack('<7fHBBB', raw[10:-2].ljust(33, b'\0'))
                                result['commands'].append({'command': fields[7], 'target_system': fields[8],
                                    'target_component': fields[9], 'confirmation': fields[10]})
                                if first_command_at is None:
                                    first_command_at = time.monotonic()
                                    deadline = first_command_at + spec['observe']
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
        command_log = ROOT / f'output/runs/{run_id}/controller/commands.csv'
        with command_log.open(newline='', encoding='utf-8') as log:
            events = [row['event'] for row in csv.DictReader(log)]
        result['events'] = events
        if events != spec['events']:
            result['errors'].append(f"Events {events}, expected {spec['events']}")
        if len(result['commands']) != spec['commands']:
            result['errors'].append(f"Received {len(result['commands'])} commands, expected {spec['commands']}")
        for attempt, command in enumerate(result['commands']):
            if command != {'command': 400, 'target_system': 1, 'target_component': 1, 'confirmation': attempt}:
                result['errors'].append(f'Unexpected command frame: {command}')
        console = (output / f'{case_id}.log').read_text(errors='replace')
        if mode == 'B0' and ('HEARTBEAT type=' in console or '== Thong ke:' in console):
            result['errors'].append('B0 ran telemetry display or Inspector reporting')
        if mode != 'B0' and '== Thong ke:' not in console:
            result['errors'].append('Inspector reporting missing in B1/B2')
    except Exception as error:
        result['errors'].append(f'{type(error).__name__}: {error}')
    finally:
        (output / f'{case_id}.bin').write_bytes(wire)
    result['passed'] = not result['errors']
    return result


def main():
    cases = scenarios()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client', type=Path, default=ROOT / 'build/bin/c_client.exe')
    parser.add_argument('--output', type=Path, default=ROOT / f'output/test_results/c_ack_{time.time_ns()}')
    parser.add_argument('--modes', nargs='+', choices=['B0', 'B1', 'B2'], default=['B0', 'B1', 'B2'])
    parser.add_argument('--cases', nargs='+', choices=list(cases), default=list(cases))
    args = parser.parse_args()
    client, output = args.client.resolve(), args.output.resolve()
    if not client.is_file():
        parser.error(f'Build C first: executable not found: {client}')
    output.mkdir(parents=True, exist_ok=False)
    results = []
    suffix = str(time.time_ns())
    for mode in args.modes:
        for name in args.cases:
            result = run_case(client, output, mode, name, cases[name], suffix)
            results.append(result)
            print(f"{'PASS' if result['passed'] else 'FAIL'} {result['case']}", flush=True)
            for error in result['errors']:
                print(f'  {error}', flush=True)
    passed = sum(result['passed'] for result in results)
    report = {'client': str(client), 'client_sha256': hashlib.sha256(client.read_bytes()).hexdigest(),
              'passed': passed, 'total': len(results), 'cases': results}
    (output / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f'{passed}/{len(results)} cases passed. Evidence: {output}', flush=True)
    return 0 if passed == len(results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
