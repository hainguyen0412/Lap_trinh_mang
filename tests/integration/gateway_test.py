"""Student A network tests. Python standard library only; no SITL required.
CRC oracle uses independent bit-at-a-time polynomial implementation.
"""
import argparse
import csv
import copy
import json
import platform
import socket
import struct
import subprocess
import threading
import time
import traceback
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SHARED_ROOT = ROOT
EXTRAS = {0: 50, 1: 124, 2: 137, 4: 237, 30: 39, 33: 104, 76: 152, 77: 143}


def frame(msg=0, seq=0, sysid=1, compid=1, payload=None, incompat=0):
    if payload is None:
        payload = struct.pack('<IBBBBB', 0, 2, 3, 81, 4, 3) if msg == 0 else struct.pack('<Iffffff', seq, .1, .2, .3, 0, 0, 0)
    header = bytes([len(payload), incompat, 0, seq % 256, sysid, compid]) + msg.to_bytes(3, 'little')
    crc = 0xffff
    for value in header + payload + bytes([EXTRAS.get(msg, 0)]):
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return b'\xfd' + header + payload + struct.pack('<H', crc) + (b'\0' * 13 if incompat & 1 else b'')


def command(cmd=400, target=1, component=1, sysid=255):
    return frame(76, sysid=sysid, compid=190,
                 payload=struct.pack('<7fHBBB', 1, 0, 0, 0, 0, 0, 5, cmd, target, component, 0))


def ports():
    held = []
    try:
        for typ in (socket.SOCK_DGRAM, socket.SOCK_STREAM, socket.SOCK_STREAM):
            s = socket.socket(socket.AF_INET, typ)
            s.bind(('127.0.0.1', 0))
            held.append(s)
        return [s.getsockname()[1] for s in held]
    finally:
        for s in held:
            s.close()


def recv_exact(s, n, timeout=3):
    data = bytearray()
    deadline = time.monotonic() + timeout
    while len(data) < n:
        s.settimeout(max(.01, deadline - time.monotonic()))
        chunk = s.recv(n - len(data))
        if not chunk:
            raise AssertionError(f'Unexpected EOF after {len(data)}/{n} bytes')
        data.extend(chunk)
    return bytes(data)


def no_data(s, timeout=.2, udp=False):
    s.settimeout(timeout)
    try:
        data = s.recv(65536)
        assert not data, f'Unexpected forwarded bytes: {data.hex()[:80]}'
    except socket.timeout:
        pass
    except ConnectionResetError:
        if udp:
            raise


class Run:
    def __init__(self, suite, name, **changes):
        self.directory = suite / name
        self.directory.mkdir()
        self.udp_port, self.obs_port, self.ctrl_port = ports()
        self.config = json.loads((SHARED_ROOT / 'config/gateway.example.json').read_text(encoding='utf-8'))
        for key, port in zip(('udp_bind', 'tcp_observer_bind', 'tcp_controller_bind'), (self.udp_port, self.obs_port, self.ctrl_port)):
            self.config['network'][key] = {'host': '127.0.0.1', 'port': port}
        self.config['logging']['root_directory'] = str(self.directory / 'runs')
        self.config['measurement'].update(mode='B0', warmup_seconds=0, duration_seconds=0, drain_timeout_ms=500)
        self.stop_file = self.directory / 'stop.request'
        runtime = {}
        mapping = {
            'max_observers': ('network', 'max_observers'),
            'max_udp_candidates': ('limits', 'udp_candidate_endpoints'),
            'client_queue_frames': ('limits', 'client_send_queue_frames'),
            'client_queue_bytes': ('limits', 'client_send_queue_bytes'),
            'heartbeat_stale_ms': ('px4', 'heartbeat_stale_after_ms'),
            'drain_ms': ('measurement', 'drain_timeout_ms'),
            'warmup_seconds': ('measurement', 'warmup_seconds'),
            'measurement_seconds': ('measurement', 'duration_seconds'),
            'mode': ('measurement', 'mode'),
        }
        runtime_flags = {'send_chunk_bytes': '--send-chunk-bytes', 'socket_send_buffer_bytes': '--socket-send-buffer-bytes', 'send_timeout_ms': '--send-timeout-ms'}
        for key, value in changes.items():
            if key in mapping:
                group, field = mapping[key]
                self.config[group][field] = value
            elif key in runtime_flags:
                runtime[runtime_flags[key]] = value
            else:
                raise ValueError('Unknown test override: ' + key)
        self.config['measurement']['expected_clients'] = min(self.config['measurement']['expected_clients'], self.config['network']['max_observers'] + 1)
        self.path = self.directory / 'config.json'
        self.path.write_text(json.dumps(self.config, indent=2), encoding='utf-8')
        self.log = (self.directory / 'console.log').open('w', encoding='utf-8')
        args = [str(SHARED_ROOT / 'build/bin/gateway.exe'), '--config', str(self.path), '--run-id', name, '--stop-file', str(self.stop_file)]
        for key, value in runtime.items():
            args += [key, str(value)]
        self.proc = subprocess.Popen(args, cwd=ROOT, stdout=self.log, stderr=subprocess.STDOUT)
        self.sockets = []
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                self.log.close()
                raise AssertionError((self.directory / 'console.log').read_text())
            if 'READY' in (self.directory / 'console.log').read_text():
                break
            time.sleep(.03)
        else:
            self.proc.kill()
            self.proc.wait()
            self.log.close()
            raise AssertionError('Gateway startup timeout')

    def tcp(self, controller=False, receive_buffer=0):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if receive_buffer:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
        s.settimeout(3)
        s.connect(('127.0.0.1', self.ctrl_port if controller else self.obs_port))
        self.sockets.append(s)
        time.sleep(.06)  # accept/role allocation, not a packet delimiter
        return s

    def udp(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind(('127.0.0.1', 0))
        s.settimeout(2)
        self.sockets.append(s)
        return s

    def send(self, sock, data):
        sock.sendto(data, ('127.0.0.1', self.udp_port))

    def stop(self):
        start = time.monotonic()
        self.stop_file.touch()
        try:
            self.proc.wait(timeout=8)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            raise AssertionError('Shutdown hung')
        finally:
            for s in self.sockets:
                s.close()
            self.log.close()
        assert self.proc.returncode == 0, (self.directory / 'console.log').read_text()
        self.shutdown_seconds = time.monotonic() - start
        return self

    @property
    def output(self):
        return self.directory / 'runs' / self.directory.name / 'gateway'

    def events(self):
        with (self.output / 'events.csv').open(newline='') as f:
            return list(csv.DictReader(f))

    def summary(self):
        with (self.output / 'summary.csv').open(newline='') as f:
            return next(csv.DictReader(f))


def basic(suite):
    r = Run(suite, 'basic', send_chunk_bytes=3)
    try:
        a, b = r.tcp(), r.tcp()
        control = r.tcp(True)
        second = r.tcp(True)
        second.settimeout(2)
        try:
            assert second.recv(1) == b''
        except ConnectionResetError:
            pass
        px4 = r.udp()
        # Controller cannot send until PX4 endpoint is authenticated by valid HEARTBEAT.
        control.sendall(command())
        no_data(px4, udp=True)
        hb = frame()
        r.send(px4, hb)
        for s in (a, b, control):
            assert recv_exact(s, len(hb)) == hb
        samples = frame(30, 1) + frame(4, 2, payload=struct.pack('<QIBB', 1234, 9, 0, 0))
        r.send(px4, samples)
        for s in (a, b, control):
            assert recv_exact(s, len(samples)) == samples
        # One-byte chunks and coalesced frames must survive TCP stream semantics.
        cmd = command()
        for value in cmd:
            control.sendall(bytes([value]))
        assert px4.recv(65536) == cmd
        land, takeoff = command(21), command(22)
        control.sendall(land + takeoff)
        assert px4.recv(65536) == land
        assert px4.recv(65536) == takeoff
        gcs_hb = frame(sysid=255, compid=190)
        control.sendall(gcs_hb)
        assert px4.recv(65536) == gcs_hb
        for bad in (command(target=2), command(target=0), command(cmd=999), command(sysid=254), frame(30, sysid=255, compid=190)):
            control.sendall(bad)
            no_data(px4, .12, udp=True)
        a.sendall(command())
        no_data(px4, udp=True)
        # observer is disconnected after attempting uplink.
        a.settimeout(2)
        try:
            assert a.recv(1) == b''
        except ConnectionResetError:
            pass
        time.sleep(.15)
        a2 = r.tcp()
        control.close()
        time.sleep(.2)
        control2 = r.tcp(True)
        # Reconnect gets no partial prefix retained from previous TCP connection.
        control2.sendall(cmd[len(cmd)//2:])
        no_data(px4, .15, udp=True)
        control2.sendall(cmd)
        assert px4.recv(65536) == cmd
        fresh = frame(30, 3)
        r.send(px4, fresh)
        for s in (a2, b, control2):
            assert recv_exact(s, len(fresh)) == fresh
        # Foreign endpoint cannot forward telemetry or change command destination.
        stranger = r.udp()
        r.send(stranger, frame(seq=99))
        for s in (a2, b, control2):
            no_data(s, .12)
        control2.sendall(command())
        assert px4.recv(65536) == command()
        no_data(stranger, .12, udp=True)
    finally:
        r.stop()
    events = r.events()
    names = {e['event'] for e in events}
    assert {'controller_busy', 'observer_uplink_denied', 'target_mismatch', 'uplink_not_allowed', 'unexpected_source', 'px4_endpoint_unknown'} <= names
    connections = [e for e in events if e['event'] == 'client_connected']
    ids = [int(e['session_id']) for e in connections]
    sources = [int(e['source_id']) for e in connections]
    assert len(ids) == len(set(ids)) and ids == sorted(ids)
    assert len(sources) == len(set(sources))
    assert r.shutdown_seconds < 3
    return {'cases': ['I01', 'I02', 'I03', 'I04', 'I11', 'I12-source'], 'summary': r.summary(), 'shutdown_s': r.shutdown_seconds}


def stale(suite):
    r = Run(suite, 'stale', heartbeat_stale_ms=250)
    try:
        ctrl, px4 = r.tcp(True), r.udp()
        hb = frame()
        r.send(px4, hb)
        assert recv_exact(ctrl, len(hb)) == hb
        time.sleep(.35)
        ctrl.sendall(command())
        no_data(px4, .2, udp=True)
        r.send(px4, hb)
        assert recv_exact(ctrl, len(hb)) == hb
        ctrl.sendall(command())
        assert px4.recv(65536) == command()
    finally:
        r.stop()
    assert 'heartbeat_stale' in {e['event'] for e in r.events()}
    return {'cases': ['I12-stale-and-recover'], 'timeout_override_ms': 250}


def parser_boundaries(suite):
    r = Run(suite, 'boundaries')
    try:
        obs, px4 = r.tcp(), r.udp()
        hb = frame()
        r.send(px4, hb)
        assert recv_exact(obs, len(hb)) == hb
        raw = frame(30, 10)
        bad = bytearray(raw)
        bad[12] ^= 1
        r.send(px4, bytes(bad))
        r.send(px4, frame(999, payload=b'\x01'))
        r.send(px4, frame(incompat=1))
        r.send(px4, frame(incompat=2))
        r.send(px4, raw[:15])
        r.send(px4, raw[15:])
        no_data(obs)
        r.send(px4, b'noise' + raw + raw)
        assert recv_exact(obs, len(raw) * 2) == raw + raw
        no_data(obs)
        # Independent parser per controller connection; old partial bytes must be reset.
        ctrl = r.tcp(True)
        cmd = command()
        ctrl.sendall(cmd[:10])
        ctrl.close()
        time.sleep(.2)
        ctrl2 = r.tcp(True)
        ctrl2.sendall(cmd[10:])
        no_data(px4, udp=True)
        ctrl2.sendall(cmd)
        assert px4.recv(65536) == cmd
    finally:
        r.stop()
    # B0 carries network counters, but leaves SEQ fields empty.
    summary = r.summary()
    assert int(summary['valid_window']) == 3
    return {'cases': ['invalid-not-forwarded', 'UDP-no-cross-datagram', 'TCP-reconnect-reset'], 'summary': summary}


def limits(suite):
    r = Run(suite, 'limits', max_observers=2, max_udp_candidates=2)
    try:
        a, b, denied = r.tcp(), r.tcp(), r.tcp()
        denied.settimeout(2)
        try:
            assert denied.recv(1) == b''
        except ConnectionResetError:
            pass
        p1, p2, p3 = r.udp(), r.udp(), r.udp()
        r.send(p1, b'noise')
        r.send(p2, b'noise')
        time.sleep(.1)
        r.send(p3, frame())
        no_data(a)
        r.send(p1, frame())
        for s in (a, b):
            assert recv_exact(s, len(frame())) == frame()
    finally:
        r.stop()
    assert {'observer_limit', 'udp_candidate_limit'} <= {e['event'] for e in r.events()}
    return {'cases': ['observer-limit', 'candidate-limit']}


def slow_client(suite):
    # Overrides force backpressure quickly; this is a fault test, not a baseline benchmark.
    r = Run(suite, 'slow', client_queue_frames=128, client_queue_bytes=32768,
            socket_send_buffer_bytes=4096, send_timeout_ms=10000, heartbeat_stale_ms=60000)
    collected = bytearray()
    stop_read = threading.Event()
    healthy_errors = []
    try:
        slow = r.tcp(receive_buffer=1024)
        healthy = r.tcp()
        px4 = r.udp()

        def reader():
            healthy.settimeout(.2)
            while not stop_read.is_set():
                try:
                    chunk = healthy.recv(65536)
                    if not chunk:
                        healthy_errors.append('healthy EOF')
                        return
                    collected.extend(chunk)
                except socket.timeout:
                    pass
                except OSError as exc:
                    if not stop_read.is_set():
                        healthy_errors.append(str(exc))
                    return

        thread = threading.Thread(target=reader)
        thread.start()
        expected = bytearray()
        for i in range(2400):
            raw = frame(seq=i % 256) if i % 100 == 0 else frame(30, i % 256)
            expected.extend(raw)
            r.send(px4, raw)
            time.sleep(.001)
        deadline = time.monotonic() + 5
        while len(collected) < len(expected) and time.monotonic() < deadline:
            time.sleep(.02)
        assert not healthy_errors, healthy_errors
        assert collected == expected, (len(collected), len(expected))
        stop_read.set()
        thread.join(2)
    finally:
        stop_read.set()
        r.stop()
    events = r.events()
    assert any(e['event'] == 'client_queue_full' and e['session_id'] == '1' for e in events), 'Slow client did not reach queue limit'
    assert not any(e['event'] == 'client_queue_full' and e['session_id'] == '2' for e in events), 'Healthy client incorrectly closed'
    assert r.shutdown_seconds < 3
    return {'cases': ['I05', 'I11-with-backpressure'], 'healthy_frames': 2400,
            'healthy_bytes': len(collected), 'shutdown_s': r.shutdown_seconds, 'summary': r.summary()}


def invalid_config(suite):
    base = json.loads((SHARED_ROOT / 'config/gateway.example.json').read_text(encoding='utf-8'))
    cases = [('{', 'invalid JSON'), (json.dumps({'udp_port': 14550}), 'legacy flat config rejected')]
    mutations = [
        (('network', 'udp_bind', 'port'), -1, 'negative port'),
        (('network', 'udp_bind', 'port'), 70000, 'port overflow'),
        (('limits', 'client_send_queue_frames'), 0, 'zero queue'),
        (('measurement', 'mode'), 'B3', 'invalid measurement mode'),
        (('network', 'tcp_controller_bind', 'port'), base['network']['tcp_observer_bind']['port'], 'same TCP role endpoints'),
        (('logging', 'mispelled_limit'), 10, 'unknown nested key'),
        (('protocol', 'allow_signed_frames'), True, 'unsupported protocol profile'),
    ]
    for fields, value, label in mutations:
        config = copy.deepcopy(base)
        target = config
        for key in fields[:-1]:
            target = target[key]
        target[fields[-1]] = value
        cases.append((json.dumps(config), label))
    for i, (text, label) in enumerate(cases):
        path = suite / f'invalid_{i}.json'
        path.write_text(text, encoding='utf-8')
        result = subprocess.run([str(SHARED_ROOT / 'build/bin/gateway.exe'), '--config', str(path)], capture_output=True, text=True, timeout=5)
        assert result.returncode != 0, label
        (suite / f'invalid_{i}.log').write_text(result.stdout + result.stderr, encoding='utf-8')
    return {'cases': [label for _, label in cases]}


def integrated_modes(suite):
    results = {}
    for mode in ('B0', 'B1', 'B2'):
        r = Run(suite, 'mode_' + mode, mode=mode)
        try:
            observer, px4 = r.tcp(), r.udp()
            battery = bytearray(31)
            struct.pack_into('<H', battery, 14, 12300)
            battery[30] = 85
            # SYSTEM_TIME (ID 2) was not accepted by A's old 7-message DemoParser.
            payload = frame(seq=10) + frame(1, 11, payload=bytes(battery)) + frame(2, 12, payload=struct.pack('<QI', 1000000, 1000)) + frame(30, 15)
            r.send(px4, payload)
            assert recv_exact(observer, len(payload)) == payload
            time.sleep(1.15)  # Allow one real configured metrics snapshot.
        finally:
            r.stop()
        with (r.output / 'metrics.csv').open(newline='') as stream:
            metrics = list(csv.DictReader(stream))
        assert metrics and int(metrics[-1]['valid_total']) == 4
        metadata = json.loads((r.output / 'run.json').read_text(encoding='utf-8'))
        assert metadata['mavlink_commit']
        assert metadata['resolved_config']['schema_version'] == 1
        assert metadata['resolved_config']['measurement']['mode'] == mode
        if mode == 'B0':
            assert metrics[-1]['seq_gap_total'] == ''
            assert metadata['inspector']['decoded_frames'] == 0
        else:
            assert metrics[-1]['seq_gap_total'] == '2'
            assert metadata['inspector']['decoded_frames'] == 3
            assert metadata['inspector']['seq_gap_total'] == 2
        if mode == 'B2':
            with (r.output / 'frames.csv').open(newline='') as stream:
                frames = list(csv.DictReader(stream))
            assert [int(f['msgid']) for f in frames] == [0, 1, 2, 30]
            with (r.output / 'delivery.csv').open(newline='') as stream:
                deliveries = list(csv.DictReader(stream))
            assert len(deliveries) == 4 and all(d['status'] == 'sent_to_socket' for d in deliveries)
        else:
            assert not (r.output / 'frames.csv').exists()
            assert not (r.output / 'delivery.csv').exists()
        results[mode] = {'summary': r.summary(), 'inspector': metadata['inspector'], 'full_common_msgid_2': True}
    return results


def benchmark(suite, seconds=60):
    r = Run(suite, 'baseline', warmup_seconds=10, measurement_seconds=seconds,
            heartbeat_stale_ms=3000, drain_ms=5000)
    buffers = [bytearray() for _ in range(5)]
    halt = threading.Event()
    readers = []
    errors = []
    expected = bytearray()
    total = (10 + seconds) * 200
    try:
        clients = [r.tcp() for _ in range(4)] + [r.tcp(True)]
        px4 = r.udp()

        def read(index, client):
            client.settimeout(.2)
            while not halt.is_set():
                try:
                    data = client.recv(65536)
                    if not data:
                        errors.append(f'client {index} EOF')
                        return
                    buffers[index].extend(data)
                except socket.timeout:
                    pass
                except OSError as exc:
                    if not halt.is_set():
                        errors.append(str(exc))
                    return

        for i, s in enumerate(clients):
            t = threading.Thread(target=read, args=(i, s))
            t.start()
            readers.append(t)
        start = time.monotonic()
        for i in range(total):
            raw = frame(seq=i % 256) if i % 200 == 0 else frame(30, i % 256)
            target = start + i / 200
            left = target - time.monotonic()
            if left > 0:
                time.sleep(left)
            r.send(px4, raw)
            expected.extend(raw)
        elapsed = time.monotonic() - start
        deadline = time.monotonic() + 5
        while any(len(b) < len(expected) for b in buffers) and time.monotonic() < deadline:
            time.sleep(.02)
        assert not errors, errors
        assert all(b == expected for b in buffers), [len(b) for b in buffers]
    finally:
        halt.set()
        for t in readers:
            t.join(2)
        r.stop()
    summary = r.summary()
    assert int(summary['not_sent_window']) == 0
    assert int(summary['pending_after_drain']) == 0
    assert int(summary['log_drop']) == 0
    assert float(summary['latency_p95_ms']) <= 50, summary
    with (r.output / 'client_summary.csv').open(newline='') as f:
        client_stats = list(csv.DictReader(f))
    assert len(client_stats) == 5
    assert all(int(c['not_sent_window']) == 0 and float(c['latency_p95_ms']) <= 50 for c in client_stats)
    return {'cases': ['P03-B0-only'], 'requested_fps': 200, 'actual_fps': total / elapsed,
            'total_frames_including_warmup': total, 'exact_raw_match_clients': 5,
            'bytes_each_client': len(expected), 'summary': summary, 'client_stats': client_stats,
            'limitation': 'One B0 network run using integrated B Parser and localhost. Not SITL, not 3 repetitions or a performance comparison.'}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--baseline', action='store_true', help='Add 10s warmup + 60s measurement')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    suite = (args.output or SHARED_ROOT / 'output/test_results' / time.strftime('test_%Y%m%d_%H%M%S')).resolve()
    suite.mkdir(parents=True, exist_ok=False)
    report = {'platform': platform.platform(), 'python': platform.python_version(), 'results': {}, 'passed': False}
    try:
        assert frame().hex() == 'fd090000000101000000000000000203510403e71e'
        for fn in (invalid_config, basic, stale, parser_boundaries, limits, slow_client, integrated_modes):
            print(f'RUN {fn.__name__}', flush=True)
            report['results'][fn.__name__] = fn(suite)
            print(f'PASS {fn.__name__}', flush=True)
        if args.baseline:
            print('RUN baseline (70 seconds)', flush=True)
            report['results']['baseline'] = benchmark(suite)
            print('PASS baseline', flush=True)
        report['passed'] = True
    except Exception:
        report['failure'] = traceback.format_exc()
        raise
    finally:
        (suite / 'test_report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        print(f'Evidence: {suite}', flush=True)


if __name__ == '__main__':
    main()
