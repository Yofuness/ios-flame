# Wi-Fi
**State:** 🟡 wland joins the home network at boot from /var/local/wlan.conf; the iOS join from Settings (wifid) still fails. **Updated:** 2026-10-28.

## How to use
- wland reads /var/local/wlan.conf (mode 600, phone only, never in git) at every boot (since 2026-10-28). Install: `tools/sunfish-install-wifi.sh`. Guarded join test: `tools/sunfish-wifitest.sh` (WIFI_PSK via env only).
- configd needs a Wi-Fi service: `tools/sunfish-netprefs.py` adds the en0 DHCP service + set and restarts configd.
- `sweetwifi=0` boots without Wi-Fi.

## Facts
- Root cause of the old firmware crash at vdev start: the MSA region must be 0x98d00000 + 2 MB (wlfw.c MSA_SIZE 0x200000, 2026-10-23).
- iOS has no known network: no com.apple.wifi.known-networks.plist in /private/var/preferences (only com.apple.wifi.plist, 8 KB). xnu 0193 answers set 51 COUNTRY_CODE and set 383; get 103 CURRENT_NETWORK unanswered (error 6), get 423 not built; wifid never sends ASSOCIATE.
- Older status.md text (stale since 2026-10-23 20:55, kept for the record): "Wi-Fi join — fails. WLAN firmware crashes at vdev start (first TX/channel setup): `PD FAILURE "wlan_process": EX:wlan_process:0x1:WLAN RT:0x1084/0x1089:PC=0xb01ab7b0`, then modem ERR-FATAL. Ruled out: BDF (EVT1.0 vs prod: same crash), channel TLV encoding, missing TFTP files. Suspects: RF/PA rail voltages (L10C/L11C 3304, L9A 640 mV), extra AOP/QMP resource, firmware HL.3.2.2 vs flame's HL.3.2.0."

## Open
- The iOS join from Settings: wifid CURRENT_NETWORK error 6 (~270 / h); the user joins once in Settings and we check the known-networks plist appears and survives a reboot.
- Status-bar icon not confirmed; Safari not tried by hand since wj1.
- Weak-signal joins, 5 GHz BSSes, roaming untested (epik1/epik2).

## Log
(Newest first, append-only. When this Log passes about 150 lines, move its oldest entries to the archive.)

### 2026-10-28 14:55 k76: iOS shows Wi-Fi (xnu 0199, sweetwifiev=1)
- kernel76, 4456 mV. The status-bar Wi-Fi icon shows on every frame (lock screen, Settings, home); Settings' Wi-Fi row reads the home network's name. wifid set 22 / set 20: 0 / 0 (no fight with wland). get 103 answered (1164 bytes). nettest: DNS 60 ms, TCP 66 ms, HTTP 200 in 497 ms. The Wi-Fi page itself was not opened (`openapp prefs:root=WIFI` refused).

### 2026-10-28 12:45 iOS Wi-Fi status: analysis (read-only)
- Correction: the shim (redwood_apple80211.c get(), :444-485) already answers SSID/BSSID/CHANNEL/RSSI/AUTH/CURRENT_NETWORK from wland's own join (fw_link_up set by FW_LINK from bridge.c:48 -> flame_sta.c:135-150). The 54 "get 103 error 6" in wj1 all fall in the 1.6 s before the link came up (88.84-90.0 s); from 90.39 s get 103 is answered every ~6 s. Error 6 = ENXIO while phase != JOINED.
- CURRENT_NETWORK is the scan-result struct (req_len 1164, the same as SCAN_RESULT), filled by give_network() :275. If wland joined before the kernel heard the AP's beacon, only ssid/bssid are filled (no RSN element).
- The gap: sta_link_changed() (flame_sta.c:189) posts only link-down; on a self-join nothing posts SSID_CHANGED/BSSID_CHANGED/LINK_CHANGED, so wifid never re-reads 1/9/4/16/2 after the link is up.
- Design (~40-60 LOC, kernel): post redwood_apple80211_event(REDWOOD_STA_LINK, 1) on link-up (and maybe ASSOC_DONE); fill the details from the last scan's heard[] list when the beacon wasn't heard; answer PHY_MODE 14 / RATE 8 / NOISE 17 / MCS 57 with fixed values. Risk: wifid may send DISASSOC 22 / LEAVE for an unknown network and stop wland's rejoin (bridge_joins=1): gate with a boot arg, log every set 22/20.
- First check (no patch): a webcam frame of the status bar and Settings > Wi-Fi on a joined boot; the icon may already show.

### 2026-10-28 06:35-06:45 Wi-Fi join on the home network
- Lasting setup at the user's request: the network file lives on the phone at /var/local/wlan.conf (mode 600), read by wland at every boot; the PC copy is in a gitignored file under work/.
- wj1 (kernel69, 4456 mV, SSH after 43 s): joined on the first try (-63 dBm); DHCP address and default route; DNS 46 ms, TCP 96 ms, HTTP 200 in 403 ms.
- Open: wifid still says CURRENT_NETWORK error 6 (54 in ~2 min), so the status bar probably shows no Wi-Fi icon (webcam frame too dim to tell); Safari not tried by hand.
- Phone left on wj1 (kernel69), dim 10.

### 2026-10-28 00:00-02:10 overnight run, part 2 (Wi-Fi part)
- Wi-Fi (sk68 counts): 0193 answers set 51 (1/1) and set 383 (1277/1277); get 103 asked 7178x, answered 0; get 423 refused; 0 ASSOCIATE. iOS has no known network: needs one join in Settings by the user.

### 2026-10-28 experiments.md lines
- 2026-10-28 sk68 Wi-Fi counts (investigator): set 51 1/1 and set 383 1277/1277 answered by 0193, get 103 CURRENT_NETWORK 7178 asked / 0 answered, get 423 refused (not built), 0 ASSOCIATE: answering 51/383 did not make wifid join; it still has no known network.
- 2026-10-28 wj1 (kernel69, 4456 mV, boot to SSH 43 s): /var/local/wlan.conf kept on the phone (user's request; no longer deleted after the test). wland join try 1: 2.4 GHz -63 dBm WPA2, auth 0, assoc 0, joined ~1.6 s later; DHCP address + default route; nettest DNS 46 ms, TCP 96 ms, HTTP 200 403 ms; tx failed 5/~2470 at +60 s. wifid CURRENT_NETWORK error 6 x54 regardless. The earlier 'no IP on en0' note for the home network no longer holds.

### 2026-10-27 ~21:50- overnight run (user away until 05:00; plan.md "Overnight run") (Wi-Fi part)
- Wi-Fi diag (k66e, 0188): scan elements intact (room 1024, have 325, returned 325); no ASSOCIATE ever; set 51 COUNTRY_CODE and set 383 (4104-byte XML plist) refused; no com.apple.wifi.known-networks.plist on the phone. 0193 (kernel67+) answers both.

### 2026-10-27 ~19:00-20:07 plan steps 1-4 checks (ck1-ck3, kernel60 defaults) (step 1 Wi-Fi part)
- Battery 4456 mV in fastboot before each boot.
- Step 1 Wi-Fi: not joined at boot+8 min. nettest: DNS for apple.com fails, https error -1009 "No network route"; no Wi-Fi icon (`logs/ck1-wifi.jpg`). Live log: 1072 wifi lines in 8 min; wifid gets 11 (SCAN_RESULT) answered 341x, gets 103 (CURRENT_NETWORK) error 6 331x, gets 1 (SSID) error 6; configd deleted en0 addresses 192.168.1.238 and 192.0.0.2 once each. The phone has no ifconfig/netstat/ping/ps in toybox. status.md said "✅ Safari works": that was with wland + wlan.conf, not iOS's own join. Corrected.

### 2026-10-27 experiments.md lines
- 2026-10-27 ck1 (kernel60, defaults, 30 min idle on charger): Wi-Fi not joined (nettest DNS fail, -1009 no route; wifid CURRENT_NETWORK error 6 x331/8 min, SCAN_RESULT answered x341); battery 4431-4434 mV, 0 mA, 100% shown and on screen, no jumps (discharge untested); AegirPoster 0 crashes, 0 CRASH lines total.

### 2026-10-25 11:30 Wi-Fi on "Epik Wifi" (parents' OpenWrt) — joins and works
- Networks (read-only SSH, routers untouched): R1-Dlink ch 1 / ch 100 (DFS), R3-One ch 6 / ch 149; both `psk-mixed`
  (hostapd wpa=3, CCMP only, AKM WPA-PSK + FT-PSK, 802.11r/k/v on, PMF off).
- Boot epik0 = kernel42 `sweetpresentwait=1`, 4392 mV; then `tools/sunfish-wifitest.sh 40151712848 epik1`
  (WIFI_SSID='Epik Wifi', psk via env), 4459 mV.
- wland picked R1 2.4 GHz ch 1, -64 dBm: auth 0, assoc 0 (aid 4), msg 1 -> msg 2, keys installed (GTK idx 1), joined.
  hostapd: AP-STA-CONNECTED auth_alg=open, EAPOL-4WAY-HS-COMPLETED. Stats after 90 s: tx 2635, 0 failed.
- nettest: DNS apple.com 45 ms, TCP 51 ms, HTTPS 200 in 374 ms. wlan.conf deleted + verified absent.
- So the radio/WPA stack handles wpa=3 + FT-PSK beacons. Not tested: the join from Settings → Wi-Fi (wifid path),
  5 GHz BSSes, roaming between R1 and R3.
- 11:44 epik2 (kernel42 `sweetpresentwait=1 sweettouch=2 flamereboot=0`, 4456 mV): join from Settings → Wi-Fi,
  password typed with flame-cmd taps (webcam-guided). wifid ASSOCIATE: any access point, auth 0x1/0x10, 32-byte
  key kind 6 (PMK); bridge "join 'Epik Wifi' (PMK) -> 0"; R1 2.4 GHz ch 1 -64 dBm; 4-way OK; join over in 0.47 s.
  Settings shows the checkmark and the status-bar Wi-Fi icon. nettest: DNS 122 ms, TCP 211 ms, HTTPS 200 (401 ms).
- The user's earlier endless spinner did not reproduce at -64 dBm. Open: weak-signal joins (user was in a weaker
  spot), 5 GHz BSSes (ch 100 DFS, ch 149), roaming R1 <-> R3. Note: epik1's `flamereboot=600` rebooted the phone
  to fastboot after 10 min.

### 2026-10-25 experiments.md lines
- 2026-10-25 epik1 (kernel42, userland 40151712848): wland wlan.conf join to "Epik Wifi" (OpenWrt psk-mixed, FT-PSK, 11r/k/v) on 2.4 GHz ch 1: joined, 4-way OK, nettest HTTPS 200. Settings/wifid join and 5 GHz untested.
- 2026-10-25 epik2: Settings → Wi-Fi join to "Epik Wifi" (wifid PMK path) works at -64 dBm, HTTPS 200; the user's spinner did not reproduce. Weak-signal and 5 GHz untested.

Older entries (before 2026-10-25): [archive/features/wifi.md](../archive/features/wifi.md).
