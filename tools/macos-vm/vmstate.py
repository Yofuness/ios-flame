#!/usr/bin/env python3
# Hibernate / resume helpers for the build VM over QMP.
#   vmstate.py save     guest sync, migrate RAM+device state to state.zst (zstd), quit QEMU
#   vmstate.py wait     after `mac-vm.sh resume`: wait until the incoming migration has finished and the VM runs
import json, os, socket, subprocess, sys, time
# VM data (disk, state, keys, credentials) lives outside git, in $MACOS_VM_DIR
D = os.environ.get('MACOS_VM_DIR', os.path.expanduser('~/VMs/macos'))
def conn():
    s = socket.socket(socket.AF_UNIX); s.connect(os.path.join(D, 'qmp.sock')); f = s.makefile('rw')
    f.readline(); call(f, 'qmp_capabilities'); return s, f
def call(f, cmd, **args):
    f.write(json.dumps({'execute': cmd, 'arguments': args} if args else {'execute': cmd}) + '\n'); f.flush()
    while True:
        r = json.loads(f.readline())
        if 'return' in r: return r['return']
        if 'error' in r: raise SystemExit(f'{cmd}: {r["error"]["desc"]}')
t0 = time.time()
if sys.argv[1] == 'save':
    subprocess.run([os.path.join(D, 'vssh'), '-o', 'ConnectTimeout=5', 'sync'], stdin=subprocess.DEVNULL, timeout=60)
    s, f = conn()
    tmp = os.path.join(D, 'state.zst.tmp')
    # the default 128 MiB/s cap is for live migration over a network; here it only slows the save down
    call(f, 'migrate-set-parameters', **{'max-bandwidth': 16 << 30})
    call(f, 'migrate', uri=f'exec:zstd -T0 -1 -q -o {tmp}')
    while True:
        m = call(f, 'query-migrate'); st = m.get('status')
        if st == 'completed': break
        if st in ('failed', 'cancelled'): raise SystemExit(f'migration {st}: {m.get("error-desc", "")}')
        time.sleep(0.3)
    call(f, 'quit'); s.close()
    for i in range(100):
        try:
            os.kill(int(open(os.path.join(D, 'qemu.pid')).read()), 0); time.sleep(0.1)
        except (OSError, ValueError): break
    os.replace(tmp, os.path.join(D, 'state.zst'))
    print(f'saved {os.path.getsize(os.path.join(D, "state.zst")) >> 20} MB in {time.time() - t0:.1f} s')
elif sys.argv[1] == 'wait':
    for i in range(300):
        try:
            s, f = conn(); break
        except OSError: time.sleep(0.1)
    while True:
        st = call(f, 'query-status')['status']
        if st == 'running': break
        m = call(f, 'query-migrate')
        if m.get('status') == 'failed': raise SystemExit('incoming migration failed')
        time.sleep(0.2)
    print(f'resumed in {time.time() - t0:.1f} s')
