# USB network
**State:** ✅ Ethernet 192.168.7.2, live kernel log (UDP 5140, full backlog: 0127). **Updated:** 2026-10-27.

## How to use
- `tools/sunfish-livelog.sh ../logs/usb-live.log` on the PC: appends every kernel log line and brings up the USB network link (phone 192.168.7.2). Start it before booting (`pgrep -af sunfish-livelog`).
- If SSH says "No route to host", this listener is probably not running.

## Facts
- USB Ethernet (CDC ECM), live kernel log over UDP 5140, full backlog since xnu 0127 (status.md).
- sunfish HS PHY is QUSB2 v2 (mainline sm7150.dtsi); xnu 0151 (kernel27): GCC_QUSB2PHY_PRIM_BCR pulse + Linux qusb2 v2 table (`sweetqusb=0` = old path).
- xnu 0184 (kernel60, `sweetusbreset=0` turns it off): re-enumeration on bus reset/disconnect. Port resets, replug and boot all fine since (ck1-ck3: 0 drops in 3x5 min).
- Replugging the charger cable kills the link on older kernels; moving to the TB4 port did (2026-10-24).

## Open
- None open (broken.md: "port resets, replug and boot all fine since xnu 0184").

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

### 2026-10-28 21:30 the packet-loss window lines up with VMware's bridge (PC side)
- bta (slow, 36 s): the PC sends unicast ARP probes at kernel 29.6/30.6/31.6 s; the phone logs each `SWEET ARP rx op 1` and its `tx op 2` reply, and the ue0 stats show TX not busy (busy 0, tx frames 150 -> 357). The PC still falls back to a broadcast probe at 33.2 s: the replies leave the phone but don't reach the PC's stack.
- PC kernel log: VMware's vmnet (`userif-3: sent link down/up event`) fires at every link change; in each slow boot one comes 15-32 s after the phone's USB attach (ns1a +27, ns1b +32, sr1b +20 (Wi-Fi roam), bta +15 (Wi-Fi roam)). `vmnet-bridge -s 6 -n 0` runs without a fixed interface (automatic bridging).
- Suspect (~60%): automatic bridging takes over / re-attaches to interfaces on link events, and frames from the phone are lost meanwhile. Fix needs the user's OK (plan U6): pin vmnet0 to a fixed adapter or exclude enp0s20f0u1. No PC change made.

### 2026-10-28 20:40 a ~10-15 s window of lost packets shortly after link-up
- Seen in the boot-to-SSH analysis: ~10-15 s after the USB link comes up, packets from the phone stop reaching the PC for ~10-15 s (PC ARP every 1 s, phone answers each, PC keeps asking; NCM rx 94 -> 109, tx 137 -> 288 frames between 10 and 20 s). Causes the 36-41 s boots-to-SSH. Not yet known whether the phone's NCM TX or the host side drops them. Next: tcpdump on the PC's USB interface during one boot + a SWEET USB tx-error counter.

### 2026-10-27 ~19:00-20:07 plan steps 1-4 checks (ck1-ck3, kernel60 defaults) (step 4 USB part)
- Battery 4456 mV in fastboot before each boot.
- Step 4 USB: ck1/ck2/ck3, 5 min each, SSH probe every 10 s: 0 failures, one `=== link up` per boot, boot to SSH 29 s (ck2) / 43 s (ck3). Done.
- Process: the first phone-runner left background loggers, ended its turn waiting on them and never ran ck2/ck3; a fresh foreground-only agent finished. Phone left up on ck3.

### 2026-10-27 ~11:20-11:58 plan step 5: USB link survives a port reset (kernel60, xnu 0184)
- Cause (investigator): sweet_usb.c handled a bus reset by clearing the address only; ep0 state, `configured`, `tx_busy` and the started transfers stayed stale, so after a host reset the endpoints were never reconfigured. VBUS is forced valid, so no disconnect event. The "busERR / SMMU cb fsr 400" line is a harmless diagnostic print (translation off, FSR format bits).
- 0184: on reset/disconnect, ENDTRANSFER (ForceRM) on all started endpoints, CLEARSTALL, DALEPENA=3, address 0, clear configured/tx queue/tx_busy, link down event, re-arm ep0 SETUP; ignore stale data-endpoint events until configured; DEVTEN link-state change logged. Off with `sweetusbreset=0`. CI run 40528851023 (third attempt: the first two never got a macOS runner).
- ur1 (kernel60, defaults): link up 12 s after boot. 5 port cycles from the PC (`echo 1 > /sys/bus/usb/devices/3-0:1.0/usb3-port1/disable`, 5 s, `echo 0`): link back after 2 s in 5/5, SSH ok. No "0184" lines reached the live log (lines during the reset are lost; not checked further).
- ur0 control (kernel60, `sweetusbreset=0`): 0/2; the PC re-enumerated the phone (new device 51, cdc_ether registered, 192.168.7.1 assigned) but no ping. The phone is now unreachable (no Wi-Fi either) and needs a physical reboot (hold power + vol-down to fastboot).
- Open: physical unplug/replug (VBUS loss) untested; the link "flap" after boot not reproduced today.

#### k60d physical replug (2026-10-27 18:46)
- kernel60 defaults, battery 4457 mV. Cable out 18:46:26-18:46:42, host saw 2 bounces (dev 53, 54), final enumeration 18:46:45; ping 0% loss, SSH ok.

### 2026-10-27 kernel57 (xnu 0181) er4-er7: early CB_RESUME recovery repeats
- See [gpu](gpu.md) for this entry; it also touches this feature.

### 2026-10-27 experiments.md lines
- 2026-10-27 ur1/ur0: kernel60 (xnu 0184 USB re-enumeration on bus reset/disconnect). Replug simulated from the PC by disabling/re-enabling root port usb3-port1 (sysfs `disable`, 5 s off): ur1 (sweetusbreset=1) link back 5/5, 2 s each, SSH ok; control ur0 (sweetusbreset=0) link never came back 0/2 (host re-enumerates, data path dead = the old replug bug). Fix works for port resets; a physical unplug (VBUS loss) is not tested.
- 2026-10-27 k60d (kernel60, defaults): physical cable replug, 16 s out, 2 contact bounces -> link + SSH back. PASS.
- 2026-10-27 ck1-ck3 (kernel60, defaults): USB post-boot flap check, 3 boots x 5 min, 30 SSH probes each: 0 failures, 1 link-up per boot, boot-to-SSH 29/43 s (ck2/ck3). PASS.

Older entries (before 2026-10-25): [archive/features/usb-network.md](../archive/features/usb-network.md).
