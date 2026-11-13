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

ROOT = Path(__file__).resolve().parents[1]
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['B0','B1','B2'], default='B1')
    parser.add_argument('--seconds', type=float, default=8)
    parser.add_argument('--observers', type=int, default=2)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if not 2 <= args.seconds <= 3600 or not 1 <= args.observers <= 9:
        parser.error('seconds must be 2..3600 and observers 1..9')
    run_id = 'local_' + str(time.time_ns()) + '_' + args.mode
    output = (args.output or ROOT / 'output/demos' / run_id).resolve()
    output.mkdir(parents=True, exist_ok=False)
    config = json.loads((ROOT / 'config/gateway.example.json').read_text())
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(('127.0.0.1', 0)); udp = probe.getsockname()[1]
    observer_port, controller_port = free_tcp_port(), free_tcp_port()
    while controller_port == observer_port:
        controller_port = free_tcp_port()
    for key, port in [('udp_bind', udp), ('tcp_observer_bind', observer_port), ('tcp_controller_bind', controller_port)]:
        config['network'][key] = {'host': '127.0.0.1', 'port': port}
    config['measurement'].update(mode=args.mode, warmup_seconds=1, duration_seconds=max(1, int(args.seconds)-1), snapshot_interval_ms=100, expected_clients=args.observers+1)
    config['logging'].update(root_directory=str(output / 'runs'), flush_interval_ms=20)
    config_path = output / 'config.json'; config_path.write_text(json.dumps(config, indent=2))
    stop_file = output / 'stop.gateway'
    children, files = [], []
    simulator = None
    success = False
    report = {'kind': 'localhost_protocol_simulation', 'mode': args.mode, 'run_id': run_id, 'px4_tested': False}

    def start(name, command):
        log = (output / (name + '.log')).open('wb'); files.append(log)
        process = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.PIPE, stdout=log, stderr=log)
        children.append((name, process)); return process

    try:
        common = ['--config', str(config_path), '--run-id', run_id]
        start('gateway', [str(ROOT / 'build/bin/gateway.exe'), *common, '--stop-file', str(stop_file)])
        wait_for(lambda: 'READY' in (output/'gateway.log').read_text(errors='replace'), children)
        for number in range(1, args.observers+1):
            start(f'observer_{number:02d}', [str(ROOT/'build/bin/c_client.exe'), *common, '--role', 'observer', '--name', f'observer_{number:02d}'])
        controller = start('controller', [str(ROOT/'build/bin/c_client.exe'), *common, '--role', 'controller'])
        gateway_log = output/'runs'/run_id/'gateway'
        wait_for(lambda: sum(row['event']=='client_connected' for row in rows(gateway_log/'events.csv')) == args.observers+1, children)
        simulator = UdpSimulator(udp, 50); simulator.thread.start()
        started = time.monotonic()
        wait_for(lambda: any(row['event']=='px4_locked' for row in rows(gateway_log/'events.csv')), children)
        command_log = output/'runs'/run_id/'controller'/'commands.csv'
        for number, command in enumerate(('arm', 'takeoff 5', 'land'), 1):
            controller.stdin.write((command+'\n').encode()); controller.stdin.flush()
            wait_for(lambda: sum(row['event']=='ack_accepted' for row in rows(command_log)) >= number, children)
        wait_for(lambda: time.monotonic()-started >= args.seconds, children, timeout=args.seconds+2)
        simulator.close()
        time.sleep(.2)
        stop_file.touch()
        for name, process in children:
            process.wait(timeout=10)
            if process.returncode:
                raise RuntimeError(f'{name} exited {process.returncode}')
        summaries = {}
        for name, _ in children:
            component = output/'runs'/run_id/name
            summaries[name] = rows(component/'summary.csv')[0]
            if name != 'gateway':
                measured = json.loads((component/'measurement.json').read_text())
                if measured['valid_total'] != simulator.sent:
                    raise AssertionError(f'{name}: received {measured["valid_total"]}, expected {simulator.sent}')
        if simulator.error or simulator.commands != [400,22,21] or simulator.heartbeats == 0:
            raise AssertionError(f'UDP uplink/ACK mismatch: {simulator.commands}, HB={simulator.heartbeats}, error={simulator.error}')
        report.update(passed=True, downlink_frames=simulator.sent, uplink_heartbeats=simulator.heartbeats, commands=simulator.commands, summaries=summaries)
        success = True
    except Exception as error:
        report.update(passed=False, error=f'{type(error).__name__}: {error}')
    finally:
        if simulator and simulator.thread.is_alive():
            simulator.close()
        stop_file.touch()
        for _, process in reversed(children):
            if process.poll() is None:
                try: process.wait(timeout=3)
                except subprocess.TimeoutExpired: process.terminate(); process.wait(timeout=3)
            if process.stdin:
                process.stdin.close()
        for file in files:
            file.close()
        (output/'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(('PASS' if success else 'FAIL') + f' A+B+C {args.mode}: {output}', flush=True)
    if not success:
        print(report['error'], flush=True)
    return 0 if success else 1


if __name__ == '__main__':
    raise SystemExit(main())
