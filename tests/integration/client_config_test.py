"""Check shared JSON, CLI precedence and actual configured client traffic."""
import argparse
from copy import deepcopy
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
from command_retry_test import ROOT, crc_x25, extract_frames

BASE = json.loads((ROOT / 'config/gateway.example.json').read_text(encoding='utf-8'))


def ack_packet(source, target, result=0):
    payload = struct.pack('<HBBiBB', 400, result, 30 if result == 5 else 100, 0, *target)
    body = bytes([10, 0, 0, 80, *source, 77, 0, 0]) + payload
    return b'\xfd' + body + struct.pack('<H', crc_x25(body + bytes([143])))


def write_config(output, name, config):
    path = output / f'{name}.json'
    path.write_text(json.dumps(config, indent=2), encoding='utf-8')
    return path


def inspect(client, arguments, cwd):
    completed = subprocess.run([str(client), *arguments, '--print-config'], cwd=cwd,
                               capture_output=True, text=True, timeout=5)
    if completed.returncode != 0:
        raise AssertionError(completed.stderr or completed.stdout)
    return json.loads(completed.stdout)


def custom_config(output):
    config = deepcopy(BASE)
    config['client'].update(gateway_host='localhost', gcs_sysid=201, gcs_compid=77, heartbeat_interval_ms=80)
    config['px4'].update(expected_sysid=23, expected_compid=42)
    config['commands'].update(ack_timeout_ms=180, max_total_attempts=2,
        in_progress_idle_timeout_ms=220, in_progress_total_timeout_ms=500)
    config['measurement'].update(mode='B2', seed=12345)
    config['limits'].update(tcp_parser_buffer_bytes=4096, client_send_queue_frames=8,
                            client_send_queue_bytes=4096, logger_queue_records=1000)
    config['logging'].update(root_directory=str(output / 'run logs'), flush_interval_ms=20,
        shutdown_drain_ms=500, max_record_bytes=2048, max_file_bytes=1048576,
        max_component_run_bytes=2097152)
    return config


def actual_traffic(client, output, kind):
    config = custom_config(output)
    if kind == 'blocked_command':
        config['commands']['allowed_command_ids'] = [21]
    if kind == 'parser_limit':
        config['limits']['tcp_parser_buffer_bytes'] = 1
    if kind == 'queue_limit':
        config['limits']['client_send_queue_bytes'] = 1
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen(1)
        listener.settimeout(5)
        config['network']['tcp_controller_bind']['port'] = listener.getsockname()[1]
        config['client']['gateway_host'] = '127.0.0.1'
        path = write_config(output, kind, config)
        run_id = f'c_config_{time.time_ns()}_{kind}'
        records, wire, sent_progress = [], bytearray(), False
        with (output / f'{kind}.log').open('wb') as log:
            process = subprocess.Popen([str(client), '--config', str(path), '--role', 'controller',
                '--run-id', run_id], cwd=output, stdin=subprocess.PIPE, stdout=log, stderr=log)
            try:
                connection, _ = listener.accept()
                with connection:
                    connection.settimeout(0.02)
                    if kind == 'parser_limit':
                        connection.sendall(b'\xfd\x09')
                    if kind != 'parser_limit':
                        process.stdin.write(b'arm\n')
                        process.stdin.flush()
                    pending = bytearray()
                    deadline = time.monotonic() + 0.85
                    first_command_at = None
                    next_progress = None
                    while time.monotonic() < deadline:
                        now = time.monotonic()
                        if kind == 'progress_total' and next_progress is not None and now >= next_progress:
                            if now - first_command_at < 0.45:
                                connection.sendall(ack_packet((23, 42), (201, 77), 5))
                                next_progress = now + 0.07
                            else:
                                next_progress = None
                        try:
                            chunk = connection.recv(4096)
                        except socket.timeout:
                            continue
                        if not chunk:
                            break
                        wire.extend(chunk)
                        pending.extend(chunk)
                        for raw in extract_frames(pending):
                            msgid = int.from_bytes(raw[7:10], 'little')
                            record = {'msgid': msgid, 'at': time.monotonic(), 'sysid': raw[5], 'compid': raw[6]}
                            if msgid == 76:
                                fields = struct.unpack('<7fHBBB', raw[10:-2].ljust(33, b'\0'))
                                record.update(command=fields[7], target_system=fields[8], target_component=fields[9], confirmation=fields[10])
                                if first_command_at is None:
                                    first_command_at = record['at']
                                if kind == 'accepted':
                                    connection.sendall(ack_packet((23, 42), (201, 77)))
                                elif kind.startswith('progress') and not sent_progress:
                                    connection.sendall(ack_packet((23, 42), (201, 77), 5))
                                    sent_progress = True
                                    if kind == 'progress_total':
                                        next_progress = time.monotonic() + 0.07
                            records.append(record)
                    process.stdin.close()
                    try:
                        connection.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                    process.wait(timeout=5)
            finally:
                if process.stdin and not process.stdin.closed:
                    process.stdin.close()
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=2)
        if process.returncode != 0:
            raise AssertionError(f'Client returned {process.returncode}')
    (output / f'{kind}.bin').write_bytes(wire)
    directory = Path(config['logging']['root_directory']) / run_id / 'controller'
    snapshot = json.loads((directory / 'resolved_config.json').read_text(encoding='utf-8'))
    effective = snapshot['effective_client']
    if effective['connection']['port'] != config['network']['tcp_controller_bind']['port']:
        raise AssertionError('Runtime port did not come from shared JSON')
    if effective['logging']['root_directory'] != str(directory.parent.parent):
        raise AssertionError('Configured log root was not applied')
    metadata = json.loads((directory / 'run.json').read_text(encoding='utf-8'))
    if metadata['metadata']['resolved_config_file'] != 'resolved_config.json' or metadata['seed'] != 12345:
        raise AssertionError('Missing config reference or configured seed in metadata')
    command_file = directory / 'commands.csv'
    with command_file.open(newline='', encoding='utf-8') if command_file.exists() else open_null_csv() as log:
        rows = list(csv.DictReader(log))
    events = [row['event'] for row in rows]
    commands = [record for record in records if record['msgid'] == 76]
    heartbeats = [record for record in records if record['msgid'] == 0]
    if kind in ('blocked_command', 'queue_limit'):
        if commands:
            raise AssertionError('Rejected command reached the socket')
        if kind == 'blocked_command':
            if 'command_not_allowed' not in (directory / 'events.csv').read_text(encoding='utf-8'):
                raise AssertionError('Disabled command was not recorded')
        elif events != ['submitted', 'send_failed']:
            raise AssertionError(f'Queue limit was not applied: {events}')
    elif kind == 'parser_limit':
        if 'tcp_buffer_limit' not in (directory / 'issues.csv').read_text(encoding='utf-8'):
            raise AssertionError('Configured parser buffer limit was not applied')
    else:
        expected = {'accepted': ['submitted', 'queued', 'ack_accepted'],
                    'timeout': ['submitted', 'queued', 'queued', 'final_timeout'],
                    'progress_idle': ['submitted', 'queued', 'ack_in_progress', 'final_timeout']}[kind] if kind != 'progress_total' else None
        if expected is not None and events != expected:
            raise AssertionError(f'{kind}: events {events}, expected {expected}')
        if kind == 'progress_total' and (events[-1:] != ['final_timeout'] or events.count('queued') != 1):
            raise AssertionError(f'Progress total timeout not applied: {events}')
        for record in records:
            if (record['sysid'], record['compid']) != (201, 77):
                raise AssertionError(f'Configured GCS IDs not applied: {record}')
        for record in commands:
            if (record['target_system'], record['target_component']) != (23, 42):
                raise AssertionError('Configured PX4 IDs not applied')
        if len(heartbeats) < 5:
            raise AssertionError(f'Configured 80ms HEARTBEAT interval not applied ({len(heartbeats)} frames)')
        if kind == 'timeout':
            gap = commands[1]['at'] - commands[0]['at']
            if not 0.14 <= gap <= 0.4:
                raise AssertionError(f'Configured 180ms ACK timeout not applied: gap={gap}')
        if kind.startswith('progress'):
            terminal_ns = int(next(row['event_ns'] for row in rows if row['event'] == 'final_timeout'))
            progress_ns = int(next(row['event_ns'] for row in rows if row['event'] == 'ack_in_progress'))
            elapsed_ms = (terminal_ns - progress_ns) / 1e6
            low, high = (180, 360) if kind == 'progress_idle' else (450, 680)
            if not low <= elapsed_ms <= high:
                raise AssertionError(f'Configured progress timeout not applied: {elapsed_ms} ms')
    return {'run_id': run_id, 'events': events, 'commands': commands, 'heartbeat_count': len(heartbeats), 'log_directory': str(directory)}


def open_null_csv():
    from io import StringIO
    return StringIO('')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client', type=Path, default=ROOT / 'build/bin/c_client.exe')
    parser.add_argument('--output', type=Path, default=ROOT / f'output/test_results/c_config_{time.time_ns()}')
    args = parser.parse_args()
    client, output = args.client.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    results = []

    def test(name, action):
        try:
            details = action()
            result = {'case': name, 'passed': True, 'details': details}
        except Exception as error:
            result = {'case': name, 'passed': False, 'error': f'{type(error).__name__}: {error}'}
        results.append(result)
        print(f"{'PASS' if result['passed'] else 'FAIL'} {name}: {result.get('error', 'OK')}", flush=True)

    def default_config():
        data = inspect(client, [], output)['effective_client']
        if data['connection']['port'] != BASE['network']['tcp_observer_bind']['port']:
            raise AssertionError('Default config was not located relative to the executable')
        if data['logging']['root_directory'] != str(ROOT / BASE['logging']['root_directory']):
            raise AssertionError('Relative logging root depends on cwd')
        return data

    test('default_from_external_cwd', default_config)
    config = custom_config(output)
    config['network']['tcp_observer_bind']['port'] = 15860
    config['network']['tcp_controller_bind']['port'] = 15861
    path = write_config(output, 'shared config with spaces', config)

    def config_values():
        value = inspect(client, ['--config', str(path), '--role', 'controller'], output)['effective_client']
        if value['connection'] != {'gateway_host': 'localhost', 'port': 15861, 'role': 'controller'}:
            raise AssertionError('Configured controller endpoint mismatch')
        if value['client'] != {key: config['client'][key] for key in value['client']} or value['commands'] != config['commands']:
            raise AssertionError('Client IDs, heartbeat or command policy mismatch')
        if value['limits'] != {key: config['limits'][key] for key in value['limits']}:
            raise AssertionError('Configured limits mismatch')
        observer = inspect(client, ['--config', str(path)], output)['effective_client']
        if observer['connection']['port'] != 15860 or observer['connection']['role'] != 'observer':
            raise AssertionError('Configured observer endpoint mismatch')
        return value

    test('shared_config_values', config_values)

    def overrides():
        values = ['--config', str(path), '127.0.0.1', '15870', '--role', 'controller',
            '--px4-sys', '31', '--px4-comp', '32', '--gcs-sys', '210', '--gcs-comp', '211',
            '--heartbeat-ms', '90', '--ack-timeout-ms', '240', '--max-attempts', '1',
            '--progress-idle-ms', '300', '--progress-total-ms', '900', '--mode', 'B0', '--log-root', 'output/custom C logs', '--verbose']
        document = inspect(client, values, output)
        value = document['effective_client']
        if value['connection'] != {'gateway_host': '127.0.0.1', 'port': 15870, 'role': 'controller'}:
            raise AssertionError('CLI endpoint precedence failed')
        if value['client'] != {'gcs_sysid': 210, 'gcs_compid': 211, 'heartbeat_interval_ms': 90}:
            raise AssertionError('CLI GCS/heartbeat precedence failed')
        if value['px4'] != {'expected_sysid': 31, 'expected_compid': 32}:
            raise AssertionError('CLI PX4 precedence failed')
        if value['commands'] != dict(config['commands'], ack_timeout_ms=240, max_total_attempts=1,
                in_progress_idle_timeout_ms=300, in_progress_total_timeout_ms=900):
            raise AssertionError('CLI command policy precedence failed')
        if value['measurement']['mode'] != 'B0' or value['logging']['root_directory'] != str(ROOT / 'output/custom C logs'):
            raise AssertionError('CLI mode/log-root precedence failed')
        if document['shared_config'] != config:
            raise AssertionError('Source snapshot was modified by CLI overrides')
        if value['display']['verbose'] is not True or document['cli_overrides']['--verbose'] != 'true':
            raise AssertionError('Verbose override was not recorded')
        return value

    test('cli_precedence', overrides)

    def rejected(arguments):
        result = subprocess.run([str(client), *arguments, '--print-config'], cwd=output,
                                capture_output=True, text=True, timeout=5)
        if result.returncode != 2 or 'Config/CLI error:' not in result.stderr:
            raise AssertionError(f'Expected config/CLI error: {result.returncode} {result.stdout} {result.stderr}')
        return result.stderr.strip()

    for name, values in [
        ('bad_role', ['--role', 'controler']), ('bad_mode', ['--mode', 'B9']),
        ('bad_port', ['--port', '0']), ('port_suffix', ['--port', '123x']),
        ('overflow_port', ['--port', '999999999999999999999999999999']),
        ('bad_id', ['--gcs-sys', '256']), ('negative_id', ['--px4-comp', '-1']),
        ('bad_attempts', ['--max-attempts', '4']), ('bad_heartbeat', ['--heartbeat-ms', '0']),
        ('bad_progress_bounds', ['--progress-idle-ms', '500', '--progress-total-ms', '100']),
        ('duplicate_mode', ['--mode', 'B0', '--mode', 'B1']),
        ('duplicate_host', ['localhost', '--host', '127.0.0.1']),
        ('extra_positional', ['localhost', '5760', 'extra']),
        ('bad_run_id', ['--run-id', '../escape']), ('unknown_flag', ['--typo']),
        ('missing_value', ['--config']),
    ]:
        test(name, lambda values=values: rejected(values))
    for name, mutate in [
        ('json_bad_heartbeat', lambda j: j['client'].update(heartbeat_interval_ms=0)),
        ('json_bad_attempts', lambda j: j['commands'].update(max_total_attempts=4)),
        ('json_wrong_type', lambda j: j['client'].update(gcs_sysid='201')),
        ('json_unknown_key', lambda j: j['client'].update(heartbeat_ms=80)),
        ('json_bad_progress', lambda j: j['commands'].update(in_progress_total_timeout_ms=1)),
    ]:
        invalid = deepcopy(config)
        mutate(invalid)
        invalid_path = write_config(output, name, invalid)
        test(name, lambda invalid_path=invalid_path: rejected(['--config', str(invalid_path)]))
    for kind in ['accepted', 'timeout', 'progress_idle', 'progress_total', 'blocked_command', 'parser_limit', 'queue_limit']:
        test('traffic_' + kind, lambda kind=kind: actual_traffic(client, output, kind))
    report = {'client': str(client), 'client_sha256': hashlib.sha256(client.read_bytes()).hexdigest(),
              'passed': sum(result['passed'] for result in results), 'total': len(results), 'cases': results}
    (output / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f"{report['passed']}/{report['total']} cases passed. Evidence: {output}", flush=True)
    return 0 if report['passed'] == report['total'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
