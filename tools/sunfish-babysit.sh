#!/bin/bash
# Babysitter for long phone tests: checks every 60 s without touching the phone (ping, `fastboot devices` and the
# live log only; no SSH, no fastboot getvar), and prints one line per event, so it can run under a monitor:
#   ALERT ...   something needs a look (phone gone, in fastboot, live log stalled, GPU hang not recovered, panic,
#               watchdog, battery low, the live-log listener gone)
#   NOTE ...    worth knowing, no action (a GPU hang that recovered, the phone back)
#   OK ...      a heartbeat every HEARTBEAT seconds (default 900) with uptime, battery and counts
#   tools/sunfish-babysit.sh [LIVELOG]
cd "$(dirname "$0")/.." || exit 1
live=${1:-../logs/usb-live.log}
PHONE=192.168.7.2 HB=${HEARTBEAT:-900}
lines=$(wc -l < "$live")
gone=0 state=up last_hb=$(date +%s) hangs=0 recovered=0 crashes=0 lowbatt=0 stalled=0 nolistener=0
say() { echo "$(date +%T) $*"; }
while true; do
	sleep 60
	now=$(date +%s)
	# reachability: 3 missed pings in a row = gone; then say whether fastboot answers
	if ping -c1 -W2 $PHONE >/dev/null 2>&1; then
		[ $state != up ] && say "NOTE phone reachable again (was $state)"
		gone=0 state=up
	else
		gone=$((gone + 1))
		if [ $gone -ge 3 ] && [ $state = up ]; then
			if timeout 5 fastboot devices 2>/dev/null | grep -q fastboot; then state=fastboot; say "ALERT phone fell into fastboot (reset or reboot)"
			else state=unreachable; say "ALERT phone unreachable for 3 min and not in fastboot (hang? USB link?)"; fi
		fi
	fi
	# the listener and the log
	if ! pgrep -f 'sunfish-livelog\.sh' >/dev/null; then
		[ $nolistener = 0 ] && say "ALERT live-log listener not running (also carries the USB network link)"; nolistener=1
	else nolistener=0; fi
	age=$((now - $(stat -c %Y "$live")))
	if [ $state = up ] && [ $age -gt 180 ]; then
		[ $stalled = 0 ] && say "ALERT live log silent for ${age} s while the phone answers ping"; stalled=1
	else stalled=0; fi
	# new log lines since the last check
	n=$(wc -l < "$live"); [ $n -lt $lines ] && lines=0
	new=$(tail -n +$((lines + 1)) "$live" | head -n $((n - lines))); lines=$n
	while IFS= read -r l; do
		case "$l" in
		*"GPU hang or fault"*) hangs=$((hangs + 1)) ;;
		*"recover2: FAILED"*|*"GPU not recovered"*|*"recover3"*FAILED*) say "ALERT GPU hang not recovered: ${l:0:160}" ;;
		*"recover2: done after"*|*"recover3: done"*|*"recover: "*"-> 0"*) recovered=$((recovered + 1)); say "NOTE GPU hang recovered: ${l:0:120}" ;;
		*panic*|*Panic*) say "ALERT panic line: ${l:0:160}" ;;
		*"SWEET UIWDT"*|*"SWEET BOOTWDT"*|*"FLAME WDT"*) say "ALERT watchdog: ${l:0:160}" ;;
		*"=== "*STOP*) say "ALERT test script stopped: ${l:0:160}" ;;
		*"CRASH "*"["*) crashes=$((crashes + 1)) ;;
		esac
	done < <(printf '%s\n' "$new" | grep -aE 'GPU hang or fault|recover|panic|Panic|UIWDT|BOOTWDT|FLAME WDT|=== .*STOP|CRASH ')
	mv=$(tail -n 20000 "$live" | grep -a 'SWEET BATTERY' | tail -1 | grep -oE '[0-9]{4}mV' | head -1 | tr -d mV)
	if [ -n "$mv" ] && [ "$mv" -lt 3700 ]; then
		[ $lowbatt = 0 ] && say "ALERT battery ${mv} mV (stop booting below 3600)"; lowbatt=1
	else lowbatt=0; fi
	if [ $((now - last_hb)) -ge $HB ]; then
		up=$(tail -n 50 "$live" | grep -aoE '^\[ *[0-9]+\.[0-9]+\]' | tail -1 | tr -d '[] ')
		say "OK phone $state, uptime ${up:-?} s, battery ${mv:-?} mV, hangs $hangs (recovered $recovered), crash lines $crashes"
		last_hb=$now
	fi
done
