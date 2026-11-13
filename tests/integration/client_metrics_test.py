"""Check periodic client metrics and measured windows with controlled TCP input."""
import argparse
from copy import deepcopy
import csv
import ctypes
import hashlib
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import time

sys.dont_write_bytecode = True
from command_retry_test import ROOT, extract_frames

BASE = json.loads((ROOT / 'config/gateway.example.json').read_text(encoding='utf-8'))
MANIFEST = json.loads((ROOT / 'tests/fixtures/v1/manifest.json').read_text(encoding='utf-8'))
FRAMES = [bytes.fromhex(frame['raw_hex']) for frame in MANIFEST['frames']]


def rows(path):
    if not path.exists():
        return []
    with path.open(newline='', encoding='utf-8') as file:
        return list(csv.DictReader(file))


def exclusive_file(path):
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                                  ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    kernel.CreateFileW.restype = ctypes.c_void_p
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    handle = kernel.CreateFileW(str(path), 0x40000000, 0, None, 4, 0x80, None)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    return lambda: kernel.CloseHandle(handle)


def run_case(client, output, mode, kind):
    name = f'{kind}_{mode}'
    config = deepcopy(BASE)
    controller = kind == 'controller'
    config['measurement'].update(mode=mode, warmup_seconds=1 if kind in ('window', 'early') else 0,
        duration_seconds=0 if kind == 'zero' else 1, snapshot_interval_ms=100, drain_timeout_ms=200)
    config['logging'].update(root_directory=str(output / 'runs'), flush_interval_ms=20,
        shutdown_drain_ms=500, max_file_bytes=12000 if kind == 'cap' else 1048576,
        max_component_run_bytes=131072 if kind == 'cap' else 2097152)
    config['client']['heartbeat_interval_ms'] = 100
    run_id = f'c_metrics_{time.time_ns()}_{name}'
    role = 'controller' if controller else 'observer'
    component = 'controller' if controller else 'observer_01'
    directory = output / 'runs' / run_id / component
    wire = bytearray()
    release_file = None
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0)); listener.listen(1); listener.settimeout(5)
        key = 'tcp_controller_bind' if controller else 'tcp_observer_bind'
        config['network'][key]['port'] = listener.getsockname()[1]
        config_path = output / f'{name}.json'
        config_path.write_text(json.dumps(config, indent=2), encoding='utf-8')
        with (output / f'{name}.log').open('wb') as log:
            process = subprocess.Popen([str(client), '--config', str(config_path), '--role', role,
                '--run-id', run_id], cwd=output, stdin=subprocess.PIPE, stdout=log, stderr=log)
            try:
                connection, _ = listener.accept()
                with connection:
                    connection.settimeout(0.02)
                    if kind == 'io_error':
                        release_file = exclusive_file(directory / 'frames.csv')
                    if kind == 'window':
                        bad = FRAMES[0][:-1] + bytes([FRAMES[0][-1] ^ 1])
                        schedule = [(0.15, FRAMES[0]), (1.15, FRAMES[1] + FRAMES[2] + FRAMES[3] + bad + b'\0\1'), (2.15, FRAMES[4])]
                        duration = 2.35
                    elif kind == 'cap':
                        schedule, duration = [(0.1, FRAMES[0] * 2000)], 0.65
                    else:
                        schedule, duration = [(0.1, FRAMES[0])], 1.35 if controller else 0.4
                    at = time.monotonic()
                    step = 0
                    while time.monotonic() - at < duration:
                        elapsed = time.monotonic() - at
                        if step < len(schedule) and elapsed >= schedule[step][0]:
                            connection.sendall(schedule[step][1]); step += 1
                        try:
                            part = connection.recv(4096)
                            if not part:
                                break
                            wire.extend(part)
                        except socket.timeout:
                            pass
                    if controller:
                        process.stdin.write(b'quit\n')
                        process.stdin.close()
                        connection.settimeout(2)
                        while True:
                            part = connection.recv(4096)
                            if not part:
                                break
                            wire.extend(part)
                    else:
                        process.stdin.close()
                        connection.shutdown(socket.SHUT_RDWR)
                    process.wait(timeout=5)
            finally:
                if process.stdin and not process.stdin.closed:
                    process.stdin.close()
                if process.poll() is None:
                    process.terminate(); process.wait(timeout=2)
                if release_file:
                    release_file()
    (output / f'{name}.bin').write_bytes(wire)
    expected_exit = 1 if kind == 'io_error' else 0
    if process.returncode != expected_exit:
        raise AssertionError(f'Exit {process.returncode}, expected {expected_exit}')
    report = json.loads((directory / 'measurement.json').read_text(encoding='utf-8'))
    summary = rows(directory / 'summary.csv')[0]
    metadata = json.loads((directory / 'run.json').read_text(encoding='utf-8'))
    if metadata['metadata']['measurement_file'] != 'measurement.json':
        raise AssertionError('Missing report reference in run.json')
    if report['program_source_sha256'] == 'unavailable' or report['logical_cpus'] < 1:
        raise AssertionError('Build identity or actual CPU count missing')
    if report['latency']['scope'] != 'client_uplink_enqueue_to_socket_completion':
        raise AssertionError('Client latency was presented as gateway latency')
    if kind in ('window', 'controller'):
        samples = rows(directory / 'metrics.csv')
        if len(samples) < 5 or samples[-1]['active_clients'] != '0':
            raise AssertionError('Missing periodic or final metrics while receive is idle')
        if not any(row['cpu_percent'] and 0 <= float(row['cpu_percent']) <= 100 for row in samples):
            raise AssertionError('Normalized CPU measurement missing')
        if not any(row['rss_bytes'] and int(row['rss_bytes']) > 0 for row in samples):
            raise AssertionError('RSS measurement missing')
        if any(row['udp_queue_drop'] or row['udp_queue_depth'] for row in samples):
            raise AssertionError('Unmeasured UDP fields should stay blank')
        inspector_fields = ['bad_crc_total', 'unsupported_total', 'noise_bytes', 'seq_gap_total', 'seq_repeat_total', 'seq_ambiguous_total']
        if mode == 'B0' and any(row[key] for row in samples for key in inspector_fields):
            raise AssertionError('B0 reported Inspector counters as measured')
        if kind == 'window':
            if summary['valid_window'] != '3' or samples[-1]['valid_total'] != '5' or float(summary['measurement_s']) != 1:
                raise AssertionError(f'Warmup/post-window data leaked into summary: {summary}')
            if mode != 'B0' and samples[-1]['bad_crc_total'] != '1':
                raise AssertionError('Inspector CRC count mismatch')
        else:
            sent = extract_frames(bytearray(wire))
            if len(sent) != report['uplink']['sent_total'] or len(wire) != report['uplink']['socket_bytes_total']:
                raise AssertionError('Socket totals differ from independently captured traffic')
            if int(summary['sent_window']) < 5 or summary['not_sent_window'] != '0' or summary['pending_after_drain'] != '0':
                raise AssertionError(f'Incorrect uplink summary: {summary}')
            if summary['valid_window'] != '1' or int(summary['latency_samples']) != int(summary['sent_window']):
                raise AssertionError('RX count was used as the TX denominator, or latency was mode-dependent')
            if report['uplink']['bytes_sent_window'] != int(summary['sent_window']) * 21:
                raise AssertionError('Completed frame bytes mismatch')
            if mode == 'B2':
                events = rows(directory / 'events.csv')
                latencies = []
                for event in events:
                    if event['event'] == 'uplink_sent_to_socket':
                        detail = json.loads(event['detail'])
                        if report['window_start_ns'] <= detail['queued_ns'] < report['window_end_ns'] and int(event['event_ns']) <= report['delivery_cutoff_ns']:
                            latencies.append((int(event['event_ns']) - detail['queued_ns']) / 1e6)
                latencies.sort()
                expected = latencies[(95 * len(latencies) + 99) // 100 - 1]
                if abs(float(summary['latency_p95_ms']) - expected) > 0.00001:
                    raise AssertionError('p95 is not nearest-rank of successful local-clock samples')
    if not controller and any(summary[key] for key in ['latency_p50_ms', 'latency_p95_ms', 'latency_max_ms']):
        raise AssertionError('Observer reported a fabricated send latency')
    if kind in ('early', 'zero') and (float(summary['measurement_s']) != 0 or summary['valid_window'] != '0'):
        raise AssertionError('Empty measurement window was filled with configured duration')
    if kind == 'cap':
        if not report['log_truncated'] or int(summary['log_drop']) <= 0 or report['valid_total'] != 2000:
            raise AssertionError('Log truncation hid data or drop counters')
        if any(file.stat().st_size > 12000 for file in directory.iterdir() if file.is_file()):
            raise AssertionError('A log/control file exceeded max_file_bytes')
    if kind == 'io_error' and (not report['client_log_failed'] or not report['log_error']):
        raise AssertionError('Logger I/O failure was swallowed')
    return {'log_directory': str(directory), 'valid_total': report['valid_total'],
            'valid_window': summary['valid_window'], 'measurement_s': summary['measurement_s'],
            'uplink': report['uplink'], 'exit_code': process.returncode}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--client', type=Path, default=ROOT / 'build/bin/c_client.exe')
    parser.add_argument('--output', type=Path, default=ROOT / f'output/test_results/c_metrics_{time.time_ns()}')
    args = parser.parse_args()
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=False)
    cases = [(mode, kind) for mode in ['B0', 'B1', 'B2'] for kind in ['window', 'controller']]
    cases += [('B2', kind) for kind in ['early', 'zero', 'cap', 'io_error']]
    results = []
    for mode, kind in cases:
        result = {'case': f'{kind}_{mode}'}
        try: result.update(passed=True, details=run_case(args.client.resolve(), output, mode, kind))
        except Exception as error: result.update(passed=False, error=f'{type(error).__name__}: {error}')
        results.append(result)
        print(f"{'PASS' if result['passed'] else 'FAIL'} {result['case']}: {result.get('error', 'OK')}", flush=True)
    report = {'client_sha256': hashlib.sha256(args.client.read_bytes()).hexdigest(),
              'passed': sum(result['passed'] for result in results), 'total': len(results), 'cases': results}
    (output / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f"{report['passed']}/{report['total']} cases passed. Evidence: {output}", flush=True)
    return 0 if report['passed'] == report['total'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
