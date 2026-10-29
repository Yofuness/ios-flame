#!/bin/sh
# Keep the phone's USB Ethernet addressed across reboots and append its live
# kernel console (UDP 5140) to LOGFILE. Each time the interface (re)appears a
# "=== link up <time>" marker is written, so one file holds every boot.
# Each boot also gets its own file, DIR/boots/YYYYMMDD-HHMMSS.log (DIR = LOGFILE's
# directory), with DIR/boots/current.log pointing at the newest: a new boot is a
# kernel uptime that drops back under 30 s. Search those, not the whole LOGFILE.
#   tools/sunfish-livelog.sh LOGFILE
set -u
log=${1:?usage: $0 LOGFILE}
boots=$(cd "$(dirname "$log")" && pwd)/boots
mkdir -p "$boots"
mac=02:00:5e:77:ee:01
socat -u udp-recv:5140,reuseaddr - | gawk -v lf="$log" -v dir="$boots" '
function newboot() {
	if (cur != "") close(cur)
	cur = dir "/" strftime("%Y%m%d-%H%M%S") ".log"
	system("ln -sfn \"" cur "\" \"" dir "/current.log\"")
}
{
	print >> lf; fflush(lf)
	if (match($0, /^\[ *([0-9]+)\./, m)) {
		up = m[1] + 0
		if (cur == "" || (up < 30 && up + 5 < prev)) newboot()
		prev = up
	}
	if (cur != "") { print >> cur; fflush(cur) }
}' &
trap 'kill $! 2>/dev/null' EXIT
while true; do
	ifname=$(ip -o link | grep -i "$mac" | awk -F': ' '{print $2}' | cut -d@ -f1)
	if [ -n "$ifname" ] && ! ip -br addr show "$ifname" | grep -q 192.168.7.1/; then
		nmcli dev set "$ifname" managed no 2>/dev/null || true
		sudo ip addr replace 192.168.7.1/24 dev "$ifname"
		sudo ip link set "$ifname" up
		echo "=== link up $(date +%T) on $ifname" | tee -a "$boots/current.log" >> "$log"
	fi
	sleep 1
done
