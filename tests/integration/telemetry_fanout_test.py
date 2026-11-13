"""Check exact telemetry fanout to three TCP clients with one UDP endpoint."""
from pathlib import Path
import csv,json,socket,subprocess,time
ROOT=Path(__file__).resolve().parents[2]
def free(kind):
    with socket.socket(socket.AF_INET,kind) as sock:
        sock.bind(('127.0.0.1',0));return sock.getsockname()[1]
def main():
    output=ROOT/'output/test_results'/('telemetry_'+str(time.time_ns()));output.mkdir(parents=True)
    c=json.loads((ROOT/'config/gateway.example.json').read_text())
    for key,kind in [('udp_bind',socket.SOCK_DGRAM),('tcp_observer_bind',socket.SOCK_STREAM),('tcp_controller_bind',socket.SOCK_STREAM)]:c['network'][key]={'host':'127.0.0.1','port':free(kind)}
    c['measurement'].update(warmup_seconds=0,duration_seconds=1,snapshot_interval_ms=100)
    c['logging'].update(root_directory=str(output/'runs'),flush_interval_ms=20)
    cfg=output/'config.json';cfg.write_text(json.dumps(c));stop=output/'stop'
    children=[]
    with (output/'console.log').open('wb') as log:
        process=subprocess.Popen([str(ROOT/'build/bin/gateway.exe'),'--config',str(cfg),'--run-id','telemetry','--stop-file',str(stop)],stdout=log,stderr=log,cwd=ROOT)
        try:
            until=time.monotonic()+8
            while 'READY' not in (output/'console.log').read_text(errors='replace'):
                assert process.poll() is None and time.monotonic()<until,'gateway readiness';time.sleep(.03)
            for key in ['tcp_observer_bind','tcp_observer_bind','tcp_controller_bind']:
                s=socket.create_connection(('127.0.0.1',c['network'][key]['port']));s.settimeout(3);children.append(s)
            events=output/'runs/telemetry/gateway/events.csv'
            while True:
                if events.exists():
                    with events.open(newline='') as f:rows=list(csv.DictReader(f))
                    if sum(r.get('event')=='client_connected' for r in rows)==3:break
                assert time.monotonic()<until,'client readiness';time.sleep(.03)
            raw=(ROOT/'tests/fixtures/v1/valid.bin').read_bytes()
            with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as udp:udp.sendto(raw,('127.0.0.1',c['network']['udp_bind']['port']))
            for s in children:
                got=bytearray()
                while len(got)<len(raw):
                    part=s.recv(4096);assert part,'unexpected EOF';got.extend(part)
                assert got==raw,'raw frame fanout changed bytes'
            print('PASS: 3 TCP clients received the exact 209-byte MAVLink stream',flush=True)
        finally:
            stop.touch()
            for s in children:s.close()
            try:process.wait(timeout=10)
            except subprocess.TimeoutExpired:process.terminate();process.wait();raise
        assert process.returncode==0
    (output/'result.json').write_text(json.dumps({'passed':True,'clients':3,'bytes_each':len(raw)}))
if __name__=='__main__':main()
