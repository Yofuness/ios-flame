#!/usr/bin/env python3
# Drive the VM over QMP: vmctl.py shot OUT.png | click X Y | key K [K...] | type TEXT | move X Y
import json, socket, sys, os, subprocess, time
# VM data (disk, state, keys, credentials) lives outside git, in $MACOS_VM_DIR
D = os.environ.get('MACOS_VM_DIR', os.path.expanduser('~/VMs/macos'))
W, H = 1920, 1200
def qmp(cmds):
    s = socket.socket(socket.AF_UNIX); s.connect(os.path.join(D, 'qmp.sock')); f = s.makefile('rw')
    f.readline(); f.write(json.dumps({'execute': 'qmp_capabilities'}) + '\n'); f.flush(); f.readline()
    out = []
    for c in cmds:
        f.write(json.dumps(c) + '\n'); f.flush()
        while True:
            r = json.loads(f.readline())
            if 'return' in r or 'error' in r: out.append(r); break
    s.close(); return out
def abs_ev(x, y):
    return [{'type': 'abs', 'data': {'axis': 'x', 'value': int(x * 32767 / W)}},
            {'type': 'abs', 'data': {'axis': 'y', 'value': int(y * 32767 / H)}}]
def btn(down):
    return [{'type': 'btn', 'data': {'down': down, 'button': 'left'}}]
def keys(names):
    return {'execute': 'send-key', 'arguments': {'keys': [{'type': 'qcode', 'data': k} for k in names]}}
CH = {' ': ['spc'], '.': ['dot'], '-': ['minus'], '/': ['slash'], ',': ['comma'], '_': ['shift', 'minus'],
      '@': ['shift', '2'], ':': ['shift', 'semicolon'], '=': ['equal'], '\n': ['ret'], "'": ['apostrophe'], '~': ['shift', 'grave_accent'], '|': ['shift', 'backslash'], '\\': ['backslash'], '>': ['shift', 'dot'], '<': ['shift', 'comma'], '*': ['shift', '8'], '&': ['shift', '7'], '$': ['shift', '4'], '"': ['shift', 'apostrophe'], ';': ['semicolon'], '(': ['shift', '9'], ')': ['shift', '0'], '!': ['shift', '1'], '#': ['shift', '3'], '+': ['shift', 'equal'], '%': ['shift', '5'], '?': ['shift', 'slash'], '[': ['bracket_left'], ']': ['bracket_right'], '{': ['shift', 'bracket_left'], '}': ['shift', 'bracket_right'], '^': ['shift', '6']}
cmd = sys.argv[1]
if cmd == 'typepw':
    pw = [l.split('=',1)[1].strip() for l in open(os.path.join(D,'credentials')) if l.startswith('password=')][0]
    cmd, sys.argv = 'type', [sys.argv[0], 'type', pw]
if cmd == 'shot':
    ppm = os.path.join(D, 'shot.ppm'); qmp([{'execute': 'screendump', 'arguments': {'filename': ppm}}])
    subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', ppm, '-vf', 'scale=960:-1', sys.argv[2]], check=True)
elif cmd in ('click', 'move', 'dclick'):
    x, y = float(sys.argv[2]), float(sys.argv[3])
    ev = [{'execute': 'input-send-event', 'arguments': {'events': abs_ev(x, y)}}]
    if cmd != 'move':
        for _ in range(2 if cmd == 'dclick' else 1):
            ev += [{'execute': 'input-send-event', 'arguments': {'events': btn(True)}},
                   {'execute': 'input-send-event', 'arguments': {'events': btn(False)}}]
    print(qmp(ev)[-1])
elif cmd == 'key':
    print(qmp([keys(k.split('+')) for k in sys.argv[2:]])[-1])
elif cmd == 'type':
    seq = []
    for c in ' '.join(sys.argv[2:]):
        if c in CH: seq.append(keys(CH[c]))
        elif c.isupper(): seq.append(keys(['shift', c.lower()]))
        else: seq.append(keys([c]))
    for c in seq: qmp([c]); time.sleep(0.03)
    print('typed', len(seq))
