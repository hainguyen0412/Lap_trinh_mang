from pathlib import Path
import importlib.util
import json
import subprocess
import time

R=Path(__file__).resolve().parents[2]
import udp_simulator as module
output=R/'output/test_results'/('abc_transport_'+str(time.time_ns()));output.mkdir(parents=True)
config=json.loads((R/'config/gateway.example.json').read_text())
import socket
with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as s:s.bind(('127.0.0.1',0));udp=s.getsockname()[1]
for key,port in [('udp_bind',udp),('tcp_observer_bind',module.free_tcp_port()),('tcp_controller_bind',module.free_tcp_port())]:config['network'][key]={'host':'127.0.0.1','port':port}
config['measurement'].update(mode='B2',warmup_seconds=0,duration_seconds=2,snapshot_interval_ms=100)
config['logging'].update(root_directory=str(output/'runs'),flush_interval_ms=20)
path=output/'config.json';path.write_text(json.dumps(config));stop=output/'stop';run_id='abc_transport'
children=[];logs=[];sim=None
def start(name,args):
    log=(output/(name+'.log')).open('wb');logs.append(log)
    p=subprocess.Popen(args,cwd=R,stdin=subprocess.PIPE,stdout=log,stderr=log);children.append((name,p));return p
try:
    common=['--config',str(path),'--run-id',run_id]
    start('gateway',[str(R/'build/bin/gateway.exe'),*common,'--stop-file',str(stop)])
    module.wait_for(lambda:'READY' in (output/'gateway.log').read_text(errors='replace'),children)
    for i in (1,2):start(f'observer_{i:02d}',[str(R/'build/bin/c_client.exe'),*common,'--role','observer','--name',f'observer_{i:02d}'])
    controller=start('controller',[str(R/'build/bin/c_client.exe'),*common,'--role','controller'])
    gateway=output/'runs'/run_id/'gateway'
    module.wait_for(lambda:sum(r['event']=='client_connected' for r in module.rows(gateway/'events.csv'))==3,children)
    sim=module.UdpSimulator(udp,50);sim.thread.start()
    module.wait_for(lambda:any(r['event']=='px4_locked' for r in module.rows(gateway/'events.csv')),children)
    command_log=output/'runs'/run_id/'controller/commands.csv'
    for n,command in enumerate(('arm','takeoff 5','land'),1):
        controller.stdin.write((command+'\n').encode());controller.stdin.flush()
        module.wait_for(lambda:sum(r['event']=='ack_accepted' for r in module.rows(command_log))>=n,children)
    module.wait_for(lambda:sim.heartbeats>0,children)
    time.sleep(.3);sim.close();time.sleep(.2);stop.touch()
    for name,p in children:p.wait(timeout=10);assert p.returncode==0,(name,p.returncode)
    counts={name:len(module.rows(output/'runs'/run_id/name/'frames.csv')) for name,_ in children if name!='gateway'}
    assert all(count==sim.sent for count in counts.values()),(counts,sim.sent)
    assert sim.commands==[400,22,21] and sim.heartbeats>0,(sim.commands,sim.heartbeats)
    (output/'result.json').write_text(json.dumps({'passed':True,'frame_counts':counts,'sent':sim.sent,'commands':sim.commands,'simulated':True}))
    print('PASS A+B+C: exact frame counts on 3 clients, heartbeat uplink, ARM/TAKEOFF/LAND ACKs and clean shutdown',flush=True)
finally:
    if sim and sim.thread.is_alive():sim.close()
    stop.touch()
    for _,p in children:
        if p.poll() is None:p.terminate();p.wait(timeout=3)
        if p.stdin:p.stdin.close()
    for log in logs:log.close()
