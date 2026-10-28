# USB network: archived log (entries before 2026-10-25)
Current file: [features/usb-network.md](../../features/usb-network.md). Same format, newest first.

## Log

### 2026-10-24 15:00 New location; phone booted as "demo"
- Booted kernel40 (sweetpresentwait=1 flamereboot=0) at 4005 mV. UI fine on the webcam (home screen, 33% charging), kernel log shows 18-34 swaps/s at boot.
- USB network not reachable after boot (link flaps "link up" every second, ssh: No route to host, no fastboot). Phone work blocked until someone can reboot it by hand.
- Webcams here: Iriun = /dev/video0 (iPhone over USB), laptop camera = /dev/video1.
- Wi-Fi does not join at this location (user report); left for later.

### 2026-10-24 10:45 Charger: Thunderbolt 4 port fixes the drain
- See [charging](charging.md) for this entry; it also touches this feature.

### 2026-10-23 20:00 Offline work while the phone is unreachable
- See [wifi](wifi.md) for this entry; it also touches this feature.

### 2026-10-23 18:56 STOP — iOS USB (USB 2) dead on every boot since ~17:15; fastboot (USB 3) fine
- PC: `usb 5-2-port2: Cannot enable. Maybe the USB cable is bad?` on every iOS boot (k51–k56).
- Not software: fails with Wi-Fi off, after `fastboot reboot-bootloader`, and with this morning's kernel14
  (GPU off) that ran all day. Port power-cycle and dock USB2 hub reset changed nothing.
- Needs a human: unplug/replug the cable (or another port/cable), full power-off of the phone.
- Found on the way, fixed: GPU pool range 2 began at 0xe1000000 = wland's rmtfs buffer (hyp-assigned to
  the modem). xnu 0150 starts it at 0xe1400000 (kernel26).
- No wlan.conf written since k49 (verified absent then).

### 2026-10-23 17:36 STOP — hard hang (demo boot k50, flamereboot=0)
- See [boot](boot.md) for this entry; it also touches this feature.
