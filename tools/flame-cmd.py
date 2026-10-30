#!/usr/bin/env python3
"""Send commands to the phone's command server (bsd/dev/sweet_usb.c, udp 192.168.7.2:5141)
and show the results from the live log (udp 5140).

  tools/flame-cmd.py 'r 0x15000020 3'            one poke line (see bsd/dev/sweet_poke.c)
  tools/flame-cmd.py -f tools/poke/X.txt         a poke script file
  tools/flame-cmd.py reboot                      PSCI reset into fastboot
  tools/flame-cmd.py 'reboot!'                    PSCI reset at once, no sync (hung kernel)
  tools/flame-cmd.py --wait 5 ...                listen longer for the results

Results are the POKE:/SWEET CMD: lines the phone prints; all received log
text is also appended to logs/live.txt. Don't run tools/flame-live.py at the
same time (one listener gets the datagrams).
"""
import argparse, os, socket, sys, time

ap = argparse.ArgumentParser()
ap.add_argument('cmd', nargs='*')
ap.add_argument('-f', '--file')
ap.add_argument('--wait', type=float, default=3)
ap.add_argument('--all', action='store_true', help='print every log line, not just results')
a = ap.parse_args()
text = open(a.file).read() if a.file else ' '.join(a.cmd)
if not text.strip():
    ap.error('nothing to send')
rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try:
    rx.bind(('0.0.0.0', 5140))
    rx.settimeout(0.5)
except OSError:
    # flame-live.py (or flame-run.sh's live recorder) holds the port: the
    # results go to its file instead.
    print('(live log port busy: results go to the running live recorder)', file=sys.stderr)
    rx = None
tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
tx.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
try:
    tx.sendto(text.encode(), ('192.168.7.2', 5141))
except OSError:
    # The Mac has no ARP entry for the phone yet: the subnet broadcast needs none.
    tx.sendto(text.encode(), ('192.168.7.255', 5141))
end = time.time() + a.wait if rx else 0
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'logs', 'usb-live.log'), 'ab') as f:
    while time.time() < end:
        try:
            data, _ = rx.recvfrom(65536)
        except socket.timeout:
            continue
        f.write(data)
        for line in data.decode(errors='replace').splitlines():
            if a.all or 'POKE:' in line or 'SWEET CMD' in line:
                print(line, flush=True)
