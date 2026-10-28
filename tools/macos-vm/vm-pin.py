#!/usr/bin/env python3
# Pin the build VM's vCPU threads 1:1 to host E-cores 4-11; QEMU's other threads go to the P-cores 0-3.
import json, os, socket, subprocess
# VM data (disk, state, keys, credentials) lives outside git, in $MACOS_VM_DIR
D = os.environ.get('MACOS_VM_DIR', os.path.expanduser('~/VMs/macos'))
s = socket.socket(socket.AF_UNIX); s.connect(os.path.join(D, 'qmp.sock')); f = s.makefile('rw')
f.readline(); f.write('{"execute":"qmp_capabilities"}\n'); f.flush(); f.readline()
f.write('{"execute":"query-cpus-fast"}\n'); f.flush()
while True:
    r = json.loads(f.readline())
    if 'return' in r: break
vcpus = {c['cpu-index']: c['thread-id'] for c in r['return']}
pid = int(open(os.path.join(D, 'qemu.pid')).read())
for tid in os.listdir(f'/proc/{pid}/task'):
    if int(tid) not in vcpus.values():
        os.sched_setaffinity(int(tid), {0, 1, 2, 3})
for idx, tid in sorted(vcpus.items()):
    os.sched_setaffinity(tid, {4 + idx})
    print(f'vcpu {idx} thread {tid} -> cpu {4 + idx}')
