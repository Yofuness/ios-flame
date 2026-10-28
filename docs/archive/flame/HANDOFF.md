# Handoff: iOS on Pixel 4 (flame)

Last updated **2026-10-22, 09:40 local** (newest work first under "Start here"). Read this first, then [NOTES.md](NOTES.md) (hardware facts,
test loop, bootloader rules) and `docs/ios-sweet/NOTES.md` (history of every kernel patch).

## Start here (next session)

### WiFi IN SETTINGS: wifid, en0, iOS's own DHCP (2026-10-22, 11:15) - kernels 0123/0124
Settings -> Wi-Fi: switch on, the home network ticked, nearby networks listed (from a
background scan while joined), Wi-Fi icon in the status bar; configd's DHCP on **en0** is the primary
interface, Safari/nettest go over it. All from a cold boot, nothing by hand. Screens: work/wifi-ui/.
- **How**: ios-topaz published `0900-en0-Apple80211-for-wifid` (from "darwin-redwood"): bsd/dev/redwood_net.c
  (en0 via the ifnet KPI, SIOCGIFMEDIA says 802.11), redwood_apple80211.c (wifid's Apple80211 requests as
  SIOCGA80211/SIOCSA80211 ioctls; the IOKit service has no user client on purpose -> "Falling back to IOCTL
  path"), redwood_sta.c (one station thread), iokit RedwoodWLAN.cpp (IO80211Controller + IONetworkInterface
  registry entries), MiniIONetworkStack.cpp (configd's InterfaceNamer waits for it). Applied unchanged
  (kernel 0123) plus topaz 0128 (IOKit quiesce) and HackiOS's keystore wrap/unwrap (keychain items).
  Their glue topaz_sta.c -> **bsd/dev/flame_sta.c**, on **bsd/dev/flame_wlan.c** = the same C API over
  **/dev/wlan** to the radio in userland: protocol phone/wifi/flamewlan.h (= bsd/sys/flame_wlan.h), one
  message per read/write: kernel -> wland SCAN/JOIN/LEAVE/TX frame, wland -> kernel HELLO/BSS list/SCAN_DONE/
  JOIN_DONE/LEFT/LINK(+ssid, bssid)/RX frame. Station calls block on the answer (scan 20 s, join 40 s).
- **wland** (phone/wifi/bridge.c): opens /dev/wlan after radio start; then **no utun/ipnet** (iOS does IP on
  en0); HELLO after the first scan (kernel makes en0); wlan.conf auto-join stays as the fallback until wifid
  asks for a network (bridge_joins). SCAN while joined = a **background scan** (radio_scan_now: the firmware's
  min/max rest on the home channel; at most every 15 s, else the cached list; list = heard in the last 2 min).
  No /dev/wlan (older kernel or boot-arg `sweetwifid=0`) -> the old utun path.
- **ADT**: /arm-io/wlan node (tools/mkadt.py; launchd starts wifid only with it: `_LimitLoadToDeviceTree`).
  work/boot/flame-hi.adt rebuilt (old one: flame-hi.adt.pre-wlan).
- **configd had no network services** (preferences.plist held host names only, dated 1969): moved to
  /var/preferences/SystemConfiguration/preferences.plist.pre-wifi (copy /var/local/sc-preferences.plist.bak),
  configd restarted -> default configuration with a Wi-Fi service for en0 (persists).
- **Kernel 0124**: HackiOS's "10-bit surfaces ('w30r', 'l10r', 'w30R', 'b3a8') as one-plane BGRA" only for
  com.apple.WebKit.WebContent (`sweetsurf10=0` never, `=2` always). For WebContent it **fixed Safari** (page
  bodies were white: its CG-drawn 'w30r' tiles); for everyone it broke chronod's CPU-path widgets (white) and
  NewsToday2's Metal images (garbled colours). Also flame_wlan keeps the joined network's scan record.
- **Open**: (1) Settings labels the network "Unsecured": wifid never joined it itself (wland's wlan.conf join
  came first) and has no known network for it. Pick it in Settings once and type the password: wifid stores
  it (keychain wrap now works) and joins by itself; then wlan.conf is only the fallback. (2) Joining another
  network from Settings is untested (path: ASSOCIATE -> sta_join -> FW_JOIN -> radio_join with the PMK as hex).
  (3) wifid sets 383 (4104 bytes) and 419, gets 96/97 RATE/MCS/NOISE...: REFUSED, harmless so far.
  (4) `wland shutdown; wland up` (radio restart) still untested: reboot the phone instead.
- Safari now renders whole pages (text, images, tables) with Metal v2; Today view widgets render with chronod on
  the CPU path (`/var/local/mtl2-clients` = `* -chronod`; with a Metal device they are still white).
- **ios-topaz pulled** (now at its 0950): besides 0900 it has **audio** (0144 ADSP, 0145 codec, 0146 stream,
  0935 /dev/redwood-audio), 0148 charger, 0140 4K pages, display/touch fixes and many GPU-pool patches (0901-0947)
  for their Adreno 610: the next things to read before writing our audio driver.

### WiFi at boot, rejoin, frame pacing, DDR vote (2026-10-22, 10:40) - kernel 0122, sweetmtl=2
Boot: `tools/flame-fresh-run.sh NAME 1800 sweetmtl=2` (boots **kernel-0122**). WiFi now comes up by itself.
- **WiFi boot job**: launchd job `org.iosonpixel.wland` (in /System/Library/xpc/launchd.plist; work/userland/
  launchd.plist.cur = what is on the phone, .pre-wland on the phone = before) runs `wland auto`: once per phone
  boot (stamp /var/local/wland.boot = kern.boottime), off with /var/local/wland.off or boot-arg `sweetwifi=0`,
  log /var/local/wland.log (previous boot: .prev). Joined + DHCP ~23 s after start. It must lift its own
  jetsam limit (memorystatus_control SET_MEMLIMIT_PROPERTIES 2048 MB): launchd's per-process limit killed it
  while the modem firmware loaded (reason 7); `JetsamProperties` in the launchd cache did not help.
- **Rejoin** (radio.c `w_link_lost`, wland.c `join_poll`): deauth/disassoc, peer kickout and WMI roam event
  0xb001 reason 2 (beacon miss) end the link with ENOTCONN; wland joins again after 3 s (rescan first);
  failed joins retry 4 x 5 s, then every 30 s with a rescan. On link loss ipnet removes the lease from the utun
  and runs `netsvc down`. Test: `kill -USR1 $(psx wland | cut -c1-6)` drops the link: rejoined + new lease in
  ~6 s, HTTPS 200. wland idle CPU: ~1.6% of a core (psx now prints CPU seconds).
- **Kernel 0121: vsync notifications carry the refresh period.** CA's
  `IOMFBServer::vsync_callback(fb, a1, a2, a3, refcon)` takes a2 = vsync host time, a2 + a3 = the next one;
  we sent a3 = 0 (deadlines always past). Swipe benchmark 15.8 -> 21 fps.
- **Kernel 0120**: GPU stats per process, summed on every submit (the old line capped at 256 submits/5 s, so its
  "busy %" undercounted): `SWEET GPU: 5s: N submits (M missed), GPU busy ...; backboardd n/ms present n/ms ...`.
  RPMh lookups accept BCM resources: `wland bcm MC0 0x3fff` votes a bus from userland.
- **Kernel 0122**: DDR (BCM MC0) + memory NoC (SH0) voted to the top at GPU start (`sweetddr=0` off): 8-thread
  CPU bandwidth 13.7 -> 18-19.6 GB/s (`membw` in phone/tools), present blit 1.65 -> 1.1 ms, backboardd submits
  ~20% faster. GPU pool cap: 256 MB per process (`sweetgpucap`, 0 = none) and the last 160 MB
  (`sweetgpureserve`) only for backboardd/SpringBoard; refusals logged once per client ("SWEET MTL: v2 X: ... refused").
- **Metal v2 fixes**: (1) NBTexture adopts Metal's private `MTLTextureImplementation` protocol: Metal NSLogged
  "texture is not a MTLTextureImplementation." ~600 lines/s from backboardd while scrolling (flooded the live log).
  (2) `/var/local/mtl2-clients` takes `-NAME` (exclude); now `* -chronod`: **chronod's widgets (Photos,
  Calendar, Weather in the Today view) are white with a Metal device and render right on CA's CPU path**
  (root cause open; its RenderBox pipelines all compile). News widget (NewsToday2) renders with Metal.
  (3) bridge: `SWEET_M2V_MESSAGES=PATH` (via /var/local/mtl2-env) logs every distinct runtime message
  (degradations: why a texture is shadowed etc.). (4) bridge patch 0101: padded textures that render alias too.
  (5) tools/flame-metal2-install.sh restarts backboardd with `psx -k` (killall does not exist: it never did).
- **Tried, reverted**: Turnip `requiresDedicatedAllocation` off for host allocations
  (work/m2v/turnip-host-alloc-not-dedicated.patch.tried): CA's 1125x2436 frame then renders straight into the
  IOSurface (no shadow copies) but comes out in horizontal bands with text missing, and GPU time went up.
  The frame is still a shadow image copied in/out each frame (~1 ms + uploads); fixing the linear render is next.
- **Frame time now**: swipe benchmark `work/m2v/swipebench.sh RUN` (home pages 1<->2, averages the FB 5 s
  windows) ~19-26 fps; backboardd ~1.8 submits and ~10-15 ms GPU per frame, run in series with CA's CPU work.
  Turnip trace: `printf "MESA_GPU_TRACES=print_csv\nMESA_GPU_TRACEFILE=/var/local/gputrace-bb.csv\n" >
  /var/local/mtl2-env`, restart backboardd, remove the file; `python3 -I work/m2v/trace-sum.py CSV`.
- **Safari**: WebKit2GPUProcessForDOMRendering prefs deleted (iOS 16 draws DOM in WebContent anyway). Pages now
  show their top tiles (Wikipedia header/title) but the body below the first tile row stays white; WebContent
  without Metal draws nothing (black).

### Metal at user quality, step 3: memory, timing, async present, every process on Turnip (2026-10-22, 04:50) - kernel 0119
Written up afterwards (the session hit its usage limit at 04:51). Tour with `mtl2-clients` = `*`:
`work/scenes/m2v-1/sheet.png` - lock, home, Settings, General, Control Center (sliders right now), Spotlight,
Clock, Calendar, Notes all correct. Wrong: Today widgets (white panels, `work/m2v/today5.png`), Safari page white,
Photos crashed once.
- **Kernels**: 0117 pool accounting (whole pool + per v2 client, logged per 32 MB), 0118 per-submit CP_ALWAYS_ON
  stamps + "SWEET GPU: 5s: N submits, GPU busy X ms" line, **0119** present not waited for (two IB slots; waits
  only for the present two back; surface remaps wait for queued presents). flame-fresh-run.sh boots 0119.
- **Bridge patch 0100** (phone/metal2/bridge/patches/0100-sweet-8mb-memory-blocks.patch): runtime memory blocks
  64 MB -> 8 MB. backboardd ~69-96 MB of pool, SpringBoard ~32 MB.
- **SweetMetal2.mm**: 10 s telemetry line in mtl2.log, logs image bases (atos), `/var/local/mtl2-env` KEY=VALUE
  lines go into the process env before the device opens (e.g. `MESA_GPU_TRACES=print_csv`, TU_DEBUG).
- **Frontend**: newTextureLayoutWithDescriptor:isHeapOrBufferBacked: (SweetTextureLayout, per-format texel
  table; row/width was wrong for narrow textures), newTextureWithBytesNoCopy:... (linear texture over a
  no-copy buffer). AegirPoster's abort came from layer_private_check_pixel_format returning nil; no abort
  seen after these.
- **Turnip**: `TU_FETCH_GMEM_MIN_DRAWS` env in tu_autotune.cc (single-prim sysmem passes go GMEM only from N
  draws; no fps effect).
- **Frame rate (open)**: swipe benchmark 9.6-17.6 swaps/s; GPU busy 29-47%, mean submit 7-9 ms, longest ~25 ms.
  0119 cut the present from 15-30 ms to ~1 ms with no fps change. `tools/flame-profile.py PROC N` (pstack
  sampling + symbolication): backboardd mostly in semwait; SpringBoard idle; touches arrive at 100 Hz.
  Full-screen pass interval median 21.5 ms (p90 67 ms) in the GPU trace.

### Metal at user quality, step 2: THE HOME SCREEN RENDERS THROUGH metal2vulkan + TURNIP (2026-10-22, 04:00) - kernel 0116, sweetmtl=2
`work/m2v/home-metal2-first.png`: lock screen, unlock, home screen (icons, labels, Batteries widget, blurred dock,
badges) composited by backboardd through **SweetMetal2.bundle** = UniXNU's frontend (phone/metal2/
NativeBridgeMetalFrontend.mm, patched for iOS 16) + the metal2vulkan bridge (phone/metal2/bridge, static lib from
`tools/build-m2v-bridge.sh`) + Turnip (inside the bundle). CA's shaders are translated and compiled **on the phone**
(no host templates): ~270 render pipelines on a cold cache, 3-30 ms each; cache in /var/local/m2v-cache/<process>.
- Boot: `tools/flame-fresh-run.sh NAME 1800 sweetmtl=2` with FLAME_KERNEL=work/boot/kernel-0116 (**kernel 0116**:
  sweetmtl=2 names the SweetMetal2 plugin/class, 1 keeps SweetMetal v1). Build `phone/metal2/build.sh`, install
  `tools/flame-metal2-install.sh` (restarts backboardd). Log: /var/local/mtl2.log + console ("MTL2[pid name]:");
  pipeline journal /var/local/m2v-cache/<proc>/pipelines.log (S = specialisation, R = render pipeline).
- Who gets a device: backboardd always, others listed in /var/local/mtl2-clients ("*" = all). **SpringBoard must be
  listed** or the unlock swipe does nothing (now: `SpringBoard`).
- iOS 16 fixes to the frontend: `[super init]` before returning the frontend's device (else -[_MTLDevice dealloc]
  crashes on unset queues); 3-wide vector function constants (CA's fixed_frag uses uchar3 fc_color_opcodes0/1:
  "Metal failed to specialize fragment function"); no 536-byte _MTLDevice check (iOS 16: 528); iPadOS-offset
  diagnostics removed; newPipelineLibraryWithFilePath: -> nil; sys_dcache_flush overridden with dc civac.
- Open: ~60-130 MB of GPU pool per process (393 MB with backboardd + SpringBoard: pool is 944 MB); posters
  (AegirPoster crash-loops without a device); apps; fps (17 during the unlock animation); missing
  newTextureLayoutWithDescriptor:isHeapOrBufferBacked: (SpringBoard), newTextureWithBytesNoCopy:... (backboardd).

### Metal at user quality, step 1: Mesa's Turnip runs on the phone (2026-10-22, 03:40) - kernel 0115
Plan and the reasons for it: **METAL-NEXT.md** (port UniXNU's on-device stack: Metal frontend -> metal2vulkan
(AIR -> SPIR-V, Metal API on Vulkan) -> Turnip with a darwin kernel backend -> our kernel; host-made templates
cannot cover apps). Done so far:
- **Turnip for iOS 16** (`tools/build-turnip-ios.sh`): work/turnip/mesa = Mesa 85fda3b3 + UniXNU's 8 patches/mesa
  (all apply), venv in work/turnip/venv (meson/mako/pyyaml), Homebrew glslang/bison/flex. Output
  work/turnip/build-ios/src/freedreno/vulkan/libvulkan_freedreno.dylib (18 MB, min iOS 16.0); on the phone in
  /var/local/lib. It opens IOAcceleratorES with type 0x4147 and tunnels the ABI through selector 0.
- **Kernel 0115** (`xnu/patches/0115-flame-gpu-v2-client-turnip.patch`; flame-fresh-run.sh boots it; FLAME_KERNEL=
  overrides): SweetAccelerator v2 user client = NativeAdreno ABI v1 (iokit/Kernel/arm/SweetGPUABI.h, copy of
  phone/turnip/NativeAdrenoABI.h): 4 GB GPU VA window per connection from 0x10_0000_0000, client-chosen IOVAs,
  WC CPU maps, BoImport (client pages; client does `dc civac`, EL0 cache ops are enabled), up to 16 IBs per submit
  wrapped like msm's a6xx_submit (CCU invalidate, fence by CACHE_FLUSH_TS; v1 async submits now too), ns fence
  waits, per-client fault count from a ring of reset "lost fence" ranges, CP_ALWAYS_ON timestamp.
  Hangs: progress-based (no fence progress for 1 s with work queued; present: 500 ms) instead of fixed timeouts;
  after a reset the fence memory jumps to the last submitted fence; the log shows rb rptr, IB1/IB2 base+rem.
- **The big trap (cost an hour):** SWEET has no PPL, hence no `__ARM_PTE_PHYSMAP__`: the physmap maps all managed
  RAM with cacheable blocks that pmap_set_cache_attributes cannot change. Any WC/NC view of a managed page is a
  mismatched alias: CPU writes through it got lost / reads went stale (Turnip's command buffer read back 15 of
  410 dwords; `v2test` showed 2496 of 16384 words wrong after 200 ms). Fix: v2 buffers come from a **pool of RAM
  XNU never manages**: 0xc4000000-0xe0000000 + 0xe1000000-0x100000000 (944 MB; WiFi radio keeps 0xe0000000+16 MB),
  sweet_gpu_pool_alloc/free, zeroed through a WC kernel view; freed buffers still mapped by a task wait in a
  zombie list. (Also fixed, but not the cause: pmap_sync_page_attributes_phys shifted a 32-bit ppnum_t; and asking
  IOKit for a WC mapping of IOMalloc'd kernel memory makes vm_object_change_wimg_mode walk kernel_object: never.)
  Note SweetSurface maps IOSurfaces WC in the kernel (kernelMap/mapCache): the same mismatched-alias hazard.
- **Tests** (phone/turnip, `build.sh`): `v2test` (ABI without Turnip: CPU/GPU coherence, CP_MEM_WRITE, import) and
  `vktest` (Turnip: device, 1 MB fill+copy, compute shader compiled on the phone 2.6-10 ms, triangle pipeline
  5-7 ms, readback exact): **ALL PASS** with UBWC on, sysmem and forced GMEM. `TU_DEBUG=noubwc|sysmem|gmem|startup`,
  `FD_RD_DUMP=enable,full FD_RD_DUMP_PATH=/var/local/rd` writes .rd captures (`tools/fd cffdump`).
- metal2vulkan check on the Mac (scratchpad build, see METAL-NEXT.md): reads iOS 16.7 AIR; 230 of 242 logged CA
  pipeline keys translate (the 12 others are RenderBox/SiriUICore keys logged without their constants).
- **Next:** metal2vulkan bridge for iOS (UniXNU scripts/build-native-metal-bridge.py: pin 720ef1ce + 5 patches),
  a phone test that draws a QuartzCore pipeline through it, then the Metal frontend for backboardd.

### WiFi works: scan, WPA2 join, DHCP, internet (2026-10-21, 23:10) - userland only, no kernel change
After a fresh boot: `wland up 0 > /var/local/wland.log 2>&1 &` (0 = run forever; `wland up SECS` stops after
SECS and the utun goes with it). With `/var/local/wlan.conf` (`ssid=NAME`, `psk=PASS` lines; **phone only,
never a repo**) it scans, joins and gets a lease in ~25 s; without it, it only scans. Run wifi12 (kernel 0114):
joined the home network (2447 MHz, -41 dBm), DHCP lease /24 from the router, DNS 1.1.1.2/1.0.0.2, **utun3 is
the PrimaryInterface**, `nettest` DNS/TCP/HTTPS 200 over WiFi, 5 MB in 7.1 s (~5.7 Mbit/s). Not in the UI yet.
- **phone/wifi/radio.c**: port of ios-topaz TopazModem 0123/0125/0126/0129/0130 (ath10k snoc/ce/htc/wmi-tlv/htt
  for WCN3990) to /dev/pmem: copy engines, HTC, WMI INIT, HTT rx ring, passive scan (37 channels), station
  (auth/assoc via WMI mgmt tx, PEER_ASSOC, VDEV_UP), WPA2-PSK 4-way handshake (CommonCrypto: PBKDF2, HMAC-SHA1,
  RFC 3394 unwrap; self-test at the first join), HTT data TX/RX as native wifi <-> Ethernet.
  **phone/wifi/ipnet.c**: utun (PF_SYSTEM control socket; kern_control structs declared by hand), ARP both
  ways, DHCP client (T1 renew), lease on the utun (p2p to the router) + `netsvc up utunN ...`.
  wland.c: SMMU step first in `up`, radio after mission mode, `wland smmu`, wlan.conf, join retries (4 x 5 s),
  stats every 30 s. wlfw.c: shadow register table now 12 entries (was 9; Linux sends 12).
  **phone/tools/netsvc**: `netsvc up IF ADDR MASK ROUTER DNS [DNS2]` / `down IF` / `show IF` (no args = ue0
  as before; one service ID per interface name). Build: `ios-cc -fobjc-arc -framework Foundation -framework
  SystemConfiguration`, sign with netsvc-ents.plist.
- **Flame facts that made it work first time:** WLAN DMA = apps SMMU (0x15000000) stream **0x640/0x1** -> its own
  bank with translation off (CBAR 0x1f000, SCTLR 0xe0, as ABL's banks and our USB stream): came out as CB5 via
  SMR5; must be set before the WLAN touches host memory (`wland up` does it before the modem boots). CE regs
  0x18A40000 (+0x1000 per CE, RRI-in-DDR wrapper +0xC004), shadow regs 0x18832000 + 4*ce. Rings/buffers in RAM
  XNU does not manage below 4 GB at **0xE0000000** (pmem maps it as device memory: no cache maintenance; WMI
  host-memory chunks only take 32-bit addresses unless service 179; this firmware asked for none). Without the
  HTT host rx ring no beacon reaches the scan (topaz's finding). Firmware: WLAN.HL.3.2.0.c9-00554, chip 0x30224.
- Timeline (wifi12): WLFW mission at 18.0 s, HTC READY at once, WMI READY +170 ms, scan 3 s, auth+assoc+4-way
  170 ms, DHCP 4 s (the first DISCOVER is lost, the retry 2 s later works).
- **Next, in order:** (1) a launchd job for `wland up 0` (and stop relying on a fresh boot: `wland shutdown;
  wland up` restarts the modem, untested with the radio); (2) link loss: deauth/kickout tear down but nothing
  rejoins yet; (3) CPU: wland polls every ~1-2 ms through pread/pwrite syscalls - measure, then batch or
  sleep longer when idle; (4) **Settings/wifid** (see below); (5) the radio in the kernel for speed (topaz's
  code is kernel code already; keep wland for the modem services).
- **wifid / Settings notes** (from IO80211.framework, wifid, configd; nothing built yet): launchd starts wifid
  only with an ADT node named `wlan` (`_LimitLoadToDeviceTree` wlan, marconi-wifi) - add to mkadt.py; we also
  lack topaz 0128 (IOKit never quiesces; wifid hung their boot). wifid matches IOKit class
  **IO80211SkywalkInterface** and waits for APPLE80211_M_DRIVER_AVAILABLE. Apple80211 finds the driver by
  getifaddrs + SIOCGIFMEDIA (802.11 media) + IOService with `IOInterfaceName`=en0 and
  `IO80211InterfaceRole`="Infrastructure", then IOServiceOpen type 0: selector 0 = ioctl wrapper (0x3C30-byte
  struct, data first, `apple80211req` {ifname[16], type, value, len, data} at 0x3C08; first call type 43
  DRIVER_VERSION), selectors 1/5 = 1-byte flags (use IOUC / IOUC event pipe), 3 = notification port,
  IOConnectMapMemory type 1 = event ring; with the flags off it falls back to ioctls SIOCGA80211/SIOCSA80211
  and kernel events (vendor 1, class 6, subclass 1; data = ifname[16] + payload). Ioctl type numbers = the
  classic macOS apple80211_ioctl.h (SSID 1, SCAN_REQ 10 with 0x1550-byte request, SCAN_RESULT 11 = one
  0x48C-byte result per GET until errno 5/0xE0820446: RSSI @0x16, cap @0x1A, BSSID @0x1C, rates @0x22/0x24,
  SSID len/bytes @0x60/0x61, IE len @0x8A, IEs inline @0x8C; ASSOCIATE 20 with the PMK from
  Apple80211ParseWPAPassword; events SCAN_DONE 10, LINK_CHANGED 4, RSN_HANDSHAKE_DONE 20). configd/SC needs a
  registry chain bus -> controller conforming to `IO80211Controller` (IOMACAddress, IOSelfNamed=true so
  InterfaceNamer skips it) -> interface conforming to `IONetworkInterface` (IOInterfaceType 6, BSD Name,
  IOInterfaceNamePrefix/Unit, IOBuiltin, IOPrimaryInterface). The real kexts (IO80211Family 1200.13, classes
  IO80211APIUserClient, IO80211AsyncEventUserClient, IO80211IOUCEventPipe) are in
  downloads/ipsw/kernelcache.release.iphone10b (old prelinked format: `ipsw kernel extract` refuses it).

### First third-party app: YouTube (YouTubePlus 5.2b4 IPA) runs (2026-10-21, 22:45) - kernel 0114
YouTube 21.13.6 (decrypted, ad-hoc signed, iOS 26.2 SDK, min iOS 16.0; YouTubePlus tweak dylibs + CydiaSubstrate
in Frameworks/) launches with Metal on (`sweetmtl=1`), shows the welcome screen, "Use YouTube signed out" goes to
the home tab, and `openapp ... youtube://www.youtube.com/watch?v=...` opens a watch page (title, channel, likes,
comments, related thumbnails all render). **Video does not play yet** (black player, see below).
- **Install recipe (no installd):** unpacked IPA in `ytapp/Payload`; stream it gzipped to a staging dir, then
  rename into /Applications (same filesystem) and rebuild LaunchServices:
  `COPYFILE_DISABLE=1 tar --no-mac-metadata --no-xattrs --uid 0 --gid 0 --uname root --gname wheel -czf - -C
  ytapp/Payload YouTube.app | tools/flame-ssh 'toybox zcat | tar -xpf - -C /var/local/tmp/yt'` (~2 min for 308 MB;
  zcat is a toybox applet without a symlink), `mv /var/local/tmp/yt/YouTube.app /Applications/`, then
  **`lsreg -r`** (registers it as a System app; `lsreg APP` alone returns 0 and logs nothing). Keep it
  root-owned: MobileInstallation could not validate the signature and tried to chmod -x the executable.
- **Kernel 0114** (`xnu/patches/0114-flame-amfi-dyld-policy.patch`): every @rpath framework failed with
  "security policy does not allow @ path expansion". dyld asks `__sandbox_ms("AMFI", 0x5a, {u64 in, u64 *out})`
  (amfi_check_dyld_policy_self, checked in the phone's dyld) and treats an error as "allow nothing"; our
  policy was named "SWEET AMFI", so the call got ENOPOLICY. The policy is now named "AMFI" and answers 0x5a:
  @ paths always; DYLD_* vars, fallback paths, interposing, insert failures, embedded vars unless the process
  is restricted/encrypted (so `DYLD_PRINT_LIBRARIES=1`, `DYLD_INSERT_LIBRARIES` work now). Other AMFI calls still
  get ENOPOLICY (lsd uses 96). flame-fresh-run.sh boots **kernel-0114**.
- **fairplayd.H2 removed from launchd** (`/System/Library/xpc/launchd.plist`, key
  /System/Library/LaunchDaemons/com.apple.fairplayd.H2.plist; backups `launchd.plist.pre-fairplay` on the phone
  and in work/userland, work/userland/launchd.plist.cur = new). It exits silently right after a container lookup
  and launchd relaunched it every 10 s, so MIG requests to com.apple.fairplayd.versioned queued forever.
  locationd's `-[CLMescalSigner initInSilo:]` (FairPlay SAPInit) blocked on one at startup, which deadlocked
  locationd (main thread dispatch_sync on that silo), and every app making a synchronous CoreLocation call
  hung with it (YouTube's main thread: NSXPC to locationd, taps ignored). Without the job the lookup fails
  fast; no new crash loops seen.
- **New tool `openapp BUNDLE_ID [URL]`** (phone/tools/openapp.m, entitlement com.apple.frontboard.launchapplications;
  in /var/local/bin): launches through FBSOpenApplicationService, URL as __PayloadURL. Unlock first
  (`flame-cmd.py swipe 540 2270 540 1100 300`). Tap "Use YouTube signed out": `flame-cmd.py tap 538 2095`.
- **Playback (next):** mediaserverd crashes when the player starts: (1) SIGBUS in H264SW.videocodec (software
  H.264 decoder) writing to 0x210670040, which is inside another dylib's __TEXT, so the decoder writes through a
  wrong pointer. My guess is the output CVPixelBuffer/IOSurface plane base (biplanar 420v/420f) is wrong:
  check IOSurfaceGetBaseAddressOfPlane vs our kernel's planar layout (see 0104). (2) SIGABRT (uncaught C++
  exception on a dispatch source) after it restarts. There is no audio device either (CoreAudio "no object
  with given ID 0", AQMEIO -66680), so the audio session has nothing to route to.
- Rendering glitches in YouTube (grey dim over the top of the home tab, black lower half, garbled skeleton
  placeholders): run the template loop (mtl-keys.log -> tools/mtl-templates.py -> flame-metal-swap.sh) for
  the pipelines YouTube's views introduced.
- Optional: MobileInstallation logs "Code signing identifier (YouTube) does not match bundle identifier", so LS
  has no entitlements for it. A copy re-signed with `-i com.google.ios.youtube` and the same entitlements
  removes that if it matters.
- USB Ethernet moves only ~1.2 MB/s (ssh and the kernel file server alike), far below USB 2.0. Worth a look:
  it slows every push (templates, apps).

### Metal glitches, stage 11 (2026-10-21, 19:45) - kernel 0113, SweetMetal + host template fixes
**Method:** `tools/flame-scenes.sh OUTDIR [SCENE...]` drives a fixed tour (lock home settings general control
notifications today spotlight clock calendar notes photos safari) with flame-cmd taps/swipes and grabs each with
flame-shot; `tools/scene-sheet.sh DIR` makes a contact sheet. Reference set from the CPU renderer (boot with
-G, no sweetmtl): `work/scenes/cpu/`. Latest Metal set: `work/scenes/mtl-0113/`. Note: CA's software
renderer has no blur, so CPU Today/Spotlight backgrounds are black where Metal's blurred wallpaper is right.
**Fixed today:**
- **attachment_clear** (`path_blit_vert + attachment_clear_frag`, att0 wm 0): was "writes nothing" and skipped,
  so CA's memoryless attachment 1 was never cleared -> garbled glyphs (Wi-Fi-off icon, play button, Spotlight
  "Show More"). Rule now (mtl-templates.py and SweetMetal): att0 masked + att1 written => dest 1.
- **Non-standard blend factors** were captured as premultiplied source-over: the key now carries
  ` bf s,d,op,sa,da,aop` (Metal enums) per attachment only when not One/OneMinusSrcAlpha/Add (old keys
  unchanged); mtlkeys parses it, mtl-templates maps to Vulkan, vkdraw takes "s,d,op,sa,da,aop" as blend.
  CA uses dest-out (0,5), additive (1,1), (4,1) and source-atop (8,5): masks (lock-screen Head mask) and
  vibrant text (Spotlight "Search" placeholder) now right.
- **mtlkeys.py**: (1) parsed by (pid, pipeline#) across concatenated logs of many boots -> keys/libs crossed
  and lost; now in log order. (2) empty `| vfc  | ffc ` keys (RenderBox) were dropped as "old format".
- **air2glsl**: icmp `samesign`, dynamic sub-word loads (i8/i16/half at index*stride: RenderBox per-instance
  records), sign extension of narrow ints (sext, air.convert from s.i16), llvm.fmuladd/fma/fabs/minnum/...,
  llvm.copysign, air.get_width/height_texture_2d (GL_EXT_samplerless_texture_functions), vectors wider than
  4 as arrays; airspec inlines everything (-inline-threshold=100000: variable_blur's bicubic_upsample).
  ~60 new templates (chronod/RenderBox plane/mask/primitive_gradient/image, variable_blur, blend-factor keys).
- **No Metal device where the GPU cannot be opened** (SweetMetal initWithAcceleratorPort: returns nil;
  MTLRegisterDevices skips nil): sandboxed extensions got a device that drew nothing, so CoreImage/VT there
  never fell back to the CPU. Took away the dark box behind the lock-screen clock.
- **Lock screen black block = HEIC**: the default wallpaper (CollectionsPoster, "WWDC 2022" CA package) is 5
  HEIC layers and **HEIC decodes to nothing on this phone** (no HEVC hardware; ImageIO returns an image with
  no data: `imgtest` in phone/tools shows it; JPEG/PNG fine). CA's Metal path uploads the zeros and draws the
  opaque "Head" layer black; the CPU renderer skips it. Workaround applied on the phone: the layers converted
  to PNG on the Mac (sips) and written over the .HEIC files (ImageIO sniffs content), originals in
  /var/local/wallpaper-heic-backup. Lock screen now shows the real iOS 16 wallpaper. Real fix: a software
  HEVC decoder (Photos, other HEIC wallpapers still blank).
- **Kernel 0113**: SweetAcceleratorUserClient::free waited for nothing before unmapping (comment said
  submissions are synchronous - false since 0109): now waits for the client's last fence (crash-looping Metal
  processes like MusicWidgets exit with work queued). flame-fresh-run.sh boots 0113.
**Open (in order of visibility):**
- **Control Center sliders**: upper part black and/or green battery-ring arcs inside; nondeterministic across
  boots (black, battery-widget fragments). Readbacks show the slider's offscreen pass (207x459: backdrop copy
  aa4cd065362b -> a429968600a5 (att0 over att1, a no-op as expected) -> filter 35179d026854 -> 538215004f47)
  produces the right material, and the composite 2899e76dd60b right after it puts it on screen correctly
  during the opening animation; the static final frame is wrong. Not luma_boost, not async (mtl-sync), not
  the 1127x2438 copy-back. Suspects: what CA draws in the settled state (damage-tracked frames; dumps of the
  last frames show only glyph draws over the slider), and cross-surface aliasing (battery widget pixels).
- **Widgets' text/content** (Today view panels empty, calendar widget sometimes black): RenderBox's text and
  AA go through att1 = RG16Float (fmt 65) accumulator pipelines (mask_accumulate, accumulator_color: att0
  wm 0 or both attachments written at once) -> needs real MRT (RB_MRT[1] + capture with 2 attachments) and an
  RG16F scratch format (RB_MRT[0].BUF_INFO for dest-1 draws, fbfetch tex 14 format), plus RenderBox's
  depth/stencil (setDepthStencilState/setStencilReferenceValue are "no signature" = dropped).
- Siri chip (air.sample_texture_1d), MapKit snapshots (VectorKit shaders: no templates; maps stay blank).
- Batteries widget glyph occasionally garbled (chronod; same aliasing suspicion).
**New debug switches (files in /var/local, read by SweetMetal):** `mtl-skip` (template hashes not drawn;
read once per process: restart backboardd), `mtl-readback` ("WxH" pass end, "WxH MIN" pass of >= MIN draws,
"WxH @N" after draw N, "WxH #HASH [TEXW]" after the first draw with that template [and texture 3 width];
one-shot; GPU CP_MEMCPY of att0 + the att1 scratch -> rb-att0.raw/rb-att1.raw/rb-info.txt), `mtl-dump` now
takes a draw count (`echo 1500 > /var/local/mtl-dump`; pass lines show the clear colour), `mtl-sync`
(synchronous submission), `mtl-passflush` (UCHE flush after every pass). Helpers:
tools/mtl-drawsin.py DUMP X0 Y0 X1 Y1 (draws in a rect from a dump), tools/mtl-tregs.py HASH... (a template's RB_MRT0/blend registers; run from the repo root).
- Also: a pmap_assert_free panic once (run cm3, 395 s) while MusicWidgets crash-looped ("Too many corpses");
  page freed with a user mapping in another pmap. Not seen again yet. FlameBridge crashes on a flame-files
  PUT whose path has spaces/'@' (put to a simple name and mv over ssh). The scratch for CA's attachment 1 is
  ONE shared growing buffer (a6draw.m scratchTargetVA:), not one per size as stage 7 said.
- Reference repos: pulled (all ios-sweet forks current; UniXNU submodules drivers/NativeAdreno,
  drivers/NativeMetal, kits/AirNir, kits/AdrenoPM4 checked out). For WiFi, port from ios-topaz TopazModem
  (patches 0113-0130) instead of writing CE/HTC/WMI/WPA2 again.

### Rounded display corners (2026-10-21, 18:30) - kernel 0112, default on
The iOS image (1053x2280 fit, 13 px black bars) now has **Apple's continuous corners** (UIBezierPath roundedRect
since iOS 7: 3 cubics + a short line per corner, 1.52866483 r along each edge), anchored to the *image* rect, so
the outline runs from the panel's top edge into the image's side edge without a step. Radius = the iPhone X's
39 pt (117 px) scaled to the fit = **110 panel px** (extent 169 px). `sweetfb_corner_radius=N` (0 off, max 160).
- History: a Codex session (14:46-15:40, in parallel with stage 9/10) wrote **0110** (CPU mask after the copy
  -> flicker) and **0111** (GPU: whole frame into a pool staging image, 2D-engine blend of mattes, full copy to
  the panel; 4th-power superellipse at the panel corners). 0111 worked but the curve was nearly square and
  mostly hidden under the glass, crossed the side bar at an angle, and doubled the present (GPU blit 2.44 ms vs
  1.26 ms). 0111 was uncommitted; committed as-is, then 0112 on top.
- **0112**: `iokit/Kernel/arm/SweetDisplayMask.h` (integer flattening of the curve, per-row coverage with 16
  sub-rows, exact horizontally; within 0.035 px of a float reference), `sweet_gpu.c` (same scaled blit three
  times with the **2D scissor** - GRAS_A2D_SCISSOR_TL/BR 0x840a/b + BLT_CNTL bit 16 - middle rows straight to
  the panel, corner bands to the stage; 4 mattes blended (RB_A2D_PIXEL_CNTL 0x3809c181 = SRC_ALPHA /
  ONE_MINUS_SRC_ALPHA); bands copied to the panel; GPU blit **1.73 ms**), SweetFramebuffer CPU copy masks
  each band row while copying (from the source, no read-back), SweetHID drops contacts that start in a masked
  corner but no longer drops contacts starting in the side bars (Codex's 0111 did: edge swipes).
- Checked (run cm3): `tools/flame-shot` rows are symmetric on all four corners; camera crops look clean, the
  dock is concentric with the bottom corners. Fit vs glass: Google's flame outline (`config_mainDisplayShape`
  in device/google/coral flame overlay, 176 px footprint) is outside our curve below row 3.
- `tools/flame-fresh-run.sh` boots **kernel-0112** now. Note `tools/build-xnu.sh` needs an absolute LOG path.
- Possible trim: every a6_blit_ib re-emits blit_init + flushes (9 blits per present now); one init per IB
  would save most of the +0.47 ms.
- First boot attempt of this session fell back to fastboot within ~10 s (no XNU log); the identical retry
  worked. The phone had been sitting at ABL's "Press power button to resume" (Power pressed during the
  unlocked warning).

### Reference repos fetched (2026-10-21, 18:30)
- New forks of ios-sweet: **ios-topaz** (Redmi Note 12 4G, SM6225): **Wi-Fi works** - TopazModem, patches
  0113-0130: modem PAS boot, GLINK/QRTR, rmtfs/pd-mapper/tftp, WLFW, then **copy engines, HTC/WMI/HTT, scan,
  association, in-kernel WPA2-PSK, Ethernet data path, iOS sees it through wifid**. Our WiFi is parked at
  exactly the step before (WLFW up, next CE/HTC/WMI): read topaz first. **iPadOS-nabu** (Xiaomi Pad 5,
  SM8150-AC: same SoC family and Adreno 640 as flame; no GPU/WiFi yet). **ios-lemonadep** (lemonadep = OnePlus 9 Pro; early, kernel CI stage).
- UniXNU (56 new commits): native Metal on the Adreno 740 (sheng) - submodules drivers/NativeAdreno,
  drivers/NativeMetal, kits/AirNir (AIR -> NIR), kits/AdrenoPM4 on github.com/UniXNU (not checked out).
- ios-sweet: +site docs, mkadt panic region (we have our own since 0098). Only fetched; local branches of
  ios-sweet (3 behind) and UniXNU (56 behind) not fast-forwarded.

### Metal driver, stage 10 (2026-10-21, 15:10) - asynchronous submission (kernel 0109)
- **Kernel 0109**: user client sel 6 = submit without waiting (returns the fence; ring space checked against
  RPTR), sel 7 = wait for a fence (polls without the GPU lock; on timeout logs + `a6_recover`). File server PUT now
  truncates (`vnode_setsize` after open): flame-files put of a smaller file is safe again.
- SweetMetal: the queue submits each command buffer with `submitAsync` and completes it in order on a serial
  dispatch queue after `retire:` (fence wait, then release of kept resources + surface sweep). Blit encoders still
  submit earlier work synchronously. Stream pool and surface cache are @synchronized.
- Result (run mtl16): no faults, all draws on the GPU, but scrolling stays ~21-24 fps: backboardd's threads sample
  idle (pstack: all in mach_msg), so the limit is upstream (app commits / frame pacing via SweetFB vsync), not the
  driver. Next perf look: SweetFramebuffer vsync/swap pacing and CA's frame scheduling.
- **Phone state at stop (17:25):** not on USB at all (no fastboot/adb/USB device); camera shows it moved, screen
  off, cable apparently unplugged. Reconnect, then `tools/flame-fresh-run.sh NAME 1800 sweetmtl=1`.

### Metal driver, stage 9 (2026-10-21, 15:00) - clears, write masks, buffer textures; lock screen still off
- **Load action Clear** was ignored (offscreen targets kept stale contents): `gpuClear:` draws one quad over the
  target with the hand-made fixed_color template (clip-space corners, identity MVP, clear colour as vertex colour).
- **Write masks**: keys now carry ` wm N` when a colour attachment's mask is not RGBA (most `att1` have wm 0, so
  most keys changed once; templates were regenerated). Pipelines whose drawn attachment has mask 0 are not drawn
  (attachment_clear clears only the memoryless attachment). vkdraw takes the mask (8th arg).
- Buffer-backed textures (`newLinearTextureWithDescriptor:offset:bytesPerRow:bytesPerImage:`,
  `newTextureWithDescriptor:offset:bytesPerRow:`): address = buffer + offset, shadow copy when not 64-aligned.
  getBytes:...slice: implemented; getBytes invalidates the CPU cache first.
- Tracing: `touch /var/local/mtl-trace` before (re)starting a process logs creation/uploads/blits/passes of
  textures >= 512 px; the draw dump also lists render passes ("== pass WxH fmt load store draws").
- **Lock screen (open):** the upper wallpaper area is black and the clock sits in a dark box (the box is the
  blurred backdrop of that black area). The wallpaper textures (1271x1823 BGRA8, 1105x1160 RGBA8) are uploaded
  by replaceRegion with all-zero data, i.e. the poster snapshot images CA gets are empty. Suspect PosterBoard /
  PridePosterExtension (Metal pipelines from their own metallibs: ribbon/shadow; no templates until their keys
  show up in mtl-keys.log) or snapshot files rendered in an earlier boot. Home-screen wallpaper is fine.

### Metal driver, stage 8 (2026-10-21, 14:35) - apps render, GPU recovery, 26 fps scrolling
Run mtl15, **kernel 0108** (flame-fresh-run.sh boots it). Home screen, widgets, Settings and Safari render on the
GPU; scrolling Settings ~26 fps (CPU path ~6). Every backboardd draw is on the GPU unless "5s:" lists it.
- **GPU recovery (kernel 0108)**: `a6_recover()` = RBBM_SW_RESET_CMD pulse + CP init again (hw_init registers, SQE,
  ME_INIT) with the zap shader **resumed** (SCM boot svc 0x0a set_remote_state(0, 13); rc 0) instead of reloaded;
  page tables/SMMU kept. Called when a user submit or a present fails. Tested: `mtlk fault` (CP_MEM_WRITE to an
  unmapped VA) -> fault, timeout, "recover: soft reset + CP init -> 0", later submits and the UI fine.
- Kernel 0108 also: completion polling with 20 us sleeps (was 1 ms: every submit took whole ms); 0107: GPU perf
  vote from boot-arg `sweetgpufreq=1..5` (default 4 = 499 MHz; was 257 MHz) - little effect, GPU ~6 ms/frame.
- Lines/points: CA draws edge antialiasing as 1-px lines (stride-16 layout pipelines). Line draws use a separate
  capture with line topology (`<hash>-l.json`, mtl-templates.py makes them for vertex_layout=1 keys; DI_PT 2/3;
  points DI_PT 9 have no templates yet). Non-indexed and instanced draws (CP_DRAW_INDX_OFFSET auto-index) work;
  first vertex != 0 is skipped. air2glsl: dynamic GEPs (offset expressions), powr/tan/... + exp10/min3 macros.
- Other render-target formats: pass prologue rt.info/rt.spfmt/rt.lrzfmt patched from the target's FMT6
  (BGRA8, RGBA8, R8, RGBA16F, BGR5A1, B5G6R5); 16-bit packed texture formats added (Metal 40/43).
- **Per-function metallib paths**: SweetMetal logs `pipeline N lib: VPATH | FPATH`; libraries from memory are
  saved in /var/local/mtl-libs (777). mtl-templates.py fetches each metallib once (work/mtl/libs) and specialises
  from it: SpringBoard's SiriUICore pipelines have templates now.
- **Keys log**: pipeline keys also go to append-only **/var/local/mtl-keys.log** (never truncate it). Loop:
  `tools/flame-ssh 'cat /var/local/mtl-keys.log' > work/mtl/keys-phone.log; cat work/mtl/mtl-*.log
  work/mtl/keys-phone.log > work/mtl/all.log; tools/mtl-templates.py work/mtl/all.log; phone/metal/build.sh;
  tools/flame-metal-swap.sh` (now pushes only templates whose md5 differs; ~240 templates, 18 MB).
- Perf counters in "5s:": encode (our CPU time per 5 s), cache (bytes cleaned), GPU (waiting in submit).
  Cache cleaning now skips IOSurfaces whose seed did not change and BO textures without CPU writes (348 -> 18 ms).
  Next perf steps: asynchronous submission (we wait ~6 ms per frame), cheaper encode (~40 us/draw: ObjC
  dictionaries, 3 KB descriptor sets memset per draw), kernel present (~2-3 ms).
- Gotchas: **flame-files PUT does not truncate** (a smaller file keeps the old tail: rm first or put NAME.new +
  mv; to fix in bsd/dev/sweet_files.c). Restarting backboardd locks the phone (`swipe 540 2270 540 1100 300`).
  mtlk is built separately: `cd phone/metal; ../../work/userland/bin/ios-cc -O2 -o out/mtlk mtlk.c sweetgpu.c
  -framework IOKit; codesign -f -s - out/mtlk`.
- Not done: lock-screen poster (black area + boxed clock seen once after a restart), constexpr sampler decode,
  first-vertex draws, points, posters/widgets (chronod/PosterBoard metallibs: keys appear when they render).

### Metal driver, stage 7 (2026-10-21, 13:55) - complete home screen on the GPU (labels, widget, dock, badges)
`work/gpu/shot-metal-home2.png` / `cam-metal-home2.jpg` (run mtl12, kernel 0106, sweetmtl=1): every backboardd
draw runs on the A640 ("5s: ... 561 draws (561 on the GPU), no template:"), no faults.
- **Framebuffer fetch**: air2glsl reads attachment 0 as texture 15 / attachment 1 as texture 14 (`texelFetch` at
  gl_FragCoord, sampler 23); before such a draw SweetMetal emits CCU colour flush + CACHE_INVALIDATE + WFI.
- **CA's memoryless attachment 1** (load/store DontCare; `dest_index=1` pipelines write it, others fetch it): one
  scratch BGRA8 buffer per target size (SweetGPU scratchTargetVA:); a dest=1 draw retargets RB_MRT[0].BASE
  (0x8825/6) to it, with a CCU flush when switching back. Templates for dest=1 use attachment 1's blend setting.
- **Resource lifetime**: SweetCmdStream `keep:`s every texture/buffer a draw uses until submit (CA releases them
  after encoding; freed BOs were unmapped -> GPU read fault far 200ad7840, which also kills the GPU for the boot).
- flame-metal-swap.sh: the mv's run in a loop on the phone (a long command line closed the SSH connection).
- mtlkeys.py skips old-format log lines; `cat work/mtl/mtl-*.log > work/mtl/all.log` + mtl-templates.py is safe.
- Still to do: GPU fault recovery (soft reset + CP re-init + zap resume SMC; now a fault means a reboot), target
  format 43 (CA probe passes), path_blit (dynamic GEP), non-BGRA8 render targets, constexpr sampler decode, other
  metallibs (SpringBoard Siri/shadow, posters, apps), performance (10 swaps/s idle; measure per pass).

### Metal driver, stage 6 (2026-10-21, 13:35) - THE HOME SCREEN RENDERS ON THE GPU
Icons, the gradient wallpaper and the dock icons are drawn by the A640 through SweetMetal (run mtl11, kernel 0106,
`sweetmtl=1`; `work/gpu/shot-metal-home1.png`, camera `work/gpu/cam-metal-home1.jpg`). Missing: icon labels and
status-bar text, the dock backdrop, badge circles (pipelines with framebuffer fetch or the memoryless attachment
`dest_index=1`, and luma_boost) - see "next" below.
- **Template pipeline, generic** (host): `tools/mtl-templates.py LOG...` reads SweetMetal's log (pipeline keys,
  `tools/mtlkeys.py`), specialises both AIR functions (`airspec.py`, cached in work/mtl/spec), translates them to
  GLSL with **`tools/air2glsl.py`** (new: LLVM IR -> GLSL; constant-offset UBO loads as uvec4 words, phis, acyclic
  CFGs via reach flags, ~40 air.* intrinsics, textures/samplers, struct values), captures one draw on turnip
  (`work/mesa/draw/capture.sh VS FS OUT blend layout vkformat`; vkdraw.c now has sets 2/3, vertex layouts, target
  formats) and exports `phone/metal/pipelines/<sha1(key)[:12]>.json` with the resource map under "res". ~50
  templates so far. Loop: use the phone, `tools/flame-ssh 'cat /var/local/mtl.log' > work/mtl/X.log`,
  `tools/mtl-templates.py work/mtl/X.log`, `tools/flame-metal-swap.sh`.
- **Phone:** templates looked up by key hash at pipeline creation ("no template H for pipeline N" logged once);
  per draw it writes A6XX texture descriptors (set 2, 64-byte slots: FMT6/swap/swizzle/pitch/base, formats in
  sweet_fmt6) and sampler descriptors (set 3; constexpr samplers 16+ = linear clamp for now); textures live in GPU
  memory (BOs; IOSurfaces imported, unaligned-pitch surfaces get a shadow copy refreshed by IOSurface seed);
  blits (copyFromTexture/Buffer, fillBuffer) run on the CPU after submitting earlier GPU work;
  newBufferWithBytesNoCopy = GPU copy synced before use.
- Bugs found on the way: (1) Metal's library returns **one function object per name**, so the constants attached
  to it were overwritten by later specialisations -> wrong keys (vertex shader without texcoords: icons
  transparent, gradient flat). Now each specialisation is a `SweetFnProxy`. (2) flame-files PUT does not truncate:
  flame-metal-swap.sh now writes NAME.new and mv's. (3) Template load took ~1 s (128 KB shader hex): trimmed blobs +
  fast hex decode. (4) Page tables: kernel **0106** adds 8 MB (2048 tables) via `a6_pt_extra()`; SweetMetal releases
  surface imports when only it still holds the surface.
- Debug aids: `touch /var/local/mtl-dump` logs the next 60 GPU draws (template, viewport/scissor, 4 vertices,
  textures with checksums) to mtl.log; the "5s:" line gives GPU vs total draws and the top untemplated pipelines.
- **Next:** framebuffer fetch (fs reads the destination: luma_boost, CA blend modes) and the memoryless second
  attachment (dest_index=1: intermediate in tile memory) -> probably a real second colour attachment in memory
  plus reading it as a texture; non-BGRA8 render targets (R8/RGBA16F blur passes); constexpr sampler decoding;
  dynamic GEP (path_blit). Then other metallibs (SpringBoard/PosterBoard/Weather pipelines).

### Metal driver, stage 5 (2026-10-21, 13:20) - THE A640 DRAWS CA's FRAMES (first GPU-rendered pixels via Metal)
- With `/var/local/mtl-tint` present, SweetMetal recolours templated quads red: the panel showed CA's full-screen
  background quad in red, drawn by the GPU into CA's IOSurface and presented (camera-confirmed, run mtl10). Without
  the flag that quad is opaque black, so the screen is black: only `fixed_color` draws (no texture) run so far.
- Fixes that got here:
  1. rdtemplate.py relocates every capture-time global-buffer address: CP_EVENT_WRITE/CP_MEM_WRITE/CP_REG_TO_MEM
     bodies (`pb` patches) and *every* write of a BASE/ADDR register pair (border colours were set twice; only the
     last was patched). Warns on any other capture VA left in packet bodies.
  2. **SweetMTLCommandBuffer -device** was missing (nil): backboardd's CA logged "commandBuffer is associated with
     a different device" for every frame and never submitted (only SpringBoard did). Now returns the queue's device.
  3. setVertex/FragmentBufferOffset:atIndex: implemented (CA rebinds offsets), bytes vs buffer: last binding wins.
  4. **Kernel 0105** (`work/boot/kernel-0105`, patch 0105-flame-gpu-va-reuse): GPU user VAs come from a free list
     (64 KB granules, freed ranges reused so the ~250 page tables in the arena are reused), `a6_unmap()` clears PTEs,
     the user client unmaps buffers on free and on close and reuses handle slots. Before, every backboardd restart
     leaked all its mappings: after two `flame-metal-swap.sh` the arena was full ("map: no page table").
     flame-fresh-run.sh boots 0105.
- **Logging:** the live UDP log drops lines in bursts (most `pipeline N:` lines never arrived). SweetMetal now also
  appends to **/var/local/mtl.log** when it exists (create with `: > /var/local/mtl.log; chmod 666 ...`).
- CA's compositor uses ~40 specialisations of fixed_vert/fixed_frag (pipeline keys in mtl.log: texcoord_count,
  image_function, blend_function, dest_count, coord_function, vertex_layout ...). airspec.py reduces e.g. the icon
  pipeline (image_function0=29, RG8 texture -> .rrrg) to 59 instructions. Plan (in progress): tools/air2glsl.py
  (specialised AIR -> GLSL, generic), capture with textures/samplers in sets 2/3, phone writes A6xx texture/sampler
  descriptors per draw, templates looked up by a hash of the pipeline key, missing keys logged for a host batch job.

### Metal driver, stage 4 (2026-10-21, 12:40) - first GPU draws submitted, two faults fixed/found
**Where it stopped:** CA's compositor draws (pipeline `fixed_color`) reach the A640 as real command streams
(559 dwords per pass, 1 draw) and fault in the SMMU; CA still completes frames on the CPU side, so nothing drawn
yet. Phone at stop: boot **mtl8** (kernel-0104, `sweetmtl=1`, flamereboot 1800 s from 12:22), screen black as
always with sweetmtl=1. Default boots (no sweetmtl) are unaffected.
- **Next fix (not landed, clear):** the pass template's `CP_EVENT_WRITE` packets (pkt7 0x46, prologue/epilogue
  of pipelines/pass.json) still carry turnip's capture-time timestamp address **0x1_0000_4000** (dw1/dw2, not
  relocated by rdtemplate.py). The GPU writes there -> fault `far 100004000 fsynr0 30011` (bit 4 = write). Fix in
  tools/rdtemplate.py (emit a patch name, e.g. `scratch.lo/hi`, for CP_EVENT_WRITE / CP_MEM_WRITE addresses)
  or in a6draw.m emitItems (point k==0x46 bodies with >=3 dwords at a per-stream scratch dword). Look for other
  capture VAs (0x1_xxxx_xxxx) in pass.json/fixed_color.json the same way (scan dws for value hi == 0x1).
- **Fixed (12:20, in the phone's bundle now):** RB_MRT_PITCH / RB_MRT_ARRAY_PITCH are in 64-byte units
  (capture: 0x48 = 4608 >> 6); a6draw.m wrote bytes -> render-target writes 64x too far (`fsr 2 far 200d28f40`).
  beginGPUPass also refuses targets whose pitch/base is not 64-byte aligned.
- **A GPU fault kills the GPU for the rest of the boot:** after a user-submit fault the next kernel present
  times out, sets `gpu_ready = false` (bsd/dev/sweet_gpu.c), and SweetAccelerator then reports not ready
  (`mtlk` and new backboardd: "no SweetAccelerator"). No GPU reset/recovery yet -> reboot per test
  (`tools/flame-fresh-run.sh NAME 1800 sweetmtl=1`). Worth adding: a6 soft reset + re-init on fault.
- **Logging gotcha:** only one `tools/flame-live.py` can hold the UDP listener. A stale one from an older run
  silently captures the new boot's log into the *old* run's live.txt (I lost time reading a stale mtl6 log).
  `pkill -f flame-live.py` before a new run if anything is left over.
- How the GPU path is built (phone/metal; build.sh compiles SweetMetal.m a6draw.m sweetgpu.c, copies
  pipelines/*.json into the bundle; `tools/flame-metal-swap.sh` pushes binary + pipelines and restarts backboardd):
  - a6draw.m: `SweetGPU` (shared user-client connection, BO alloc, IOSurface import cached per surface ID,
    turnip's global buffer relocated once, pool of 1 MB IB + 1 MB scratch streams), `SweetPipelineTemplate`
    (`pipelines/NAME.json`: shader BO copied, static draw-state groups pre-patched into one BO, dynamic groups
    patched per draw), `SweetCmdStream` (pass prologue/epilogue from `pass.json`, per draw: bindless UBO
    descriptor sets 0/1 in scratch [dw0 = addr lo, dw1 = hi | size16 << 17], dynamic groups,
    CP_SET_DRAW_STATE, CP_DRAW_INDX_OFFSET; `submit` cleans caches and submits synchronously, 1 s timeout).
  - SweetMetal.m: SweetMTLBuffer is GPU memory (contents = BO map, gpuAddress = BO VA, so CA's embedded
    pointers work); render encoder tracks buffers/bytes/viewport/scissor, draws indexed quads with the template
    when the pipeline matches `fixed_color` (fixed_vert/fixed_frag, texcoord_count0/1 = 0, image_count = 0,
    BGRA8); queue submit runs the stream before completing the buffers.
  - Templates come from the host: `work/mesa/draw/capture.sh VS FS OUT [blend]` (turnip + drm-shim, sysmem)
    then `tools/rdtemplate.py` export -> JSON (groups with patch points: shader/global/vbN/bindlessN/vp/scissor/
    rt.*/window.br/a2d.br). Current: work/mesa/draw/cap2/pipe.json = pipelines/fixed_color.json = pass.json.
- After the event-write fix, the order of work: see a solid-colour quad (`tools/flame-shot --stride 4352`),
  then tools/air2glsl.py (AIR -> GLSL for CA's specialisations, incl. framebuffer fetch), textures/samplers
  (sets 2/3), clears, non-indexed draws, blit encoder, templates for every CA pipeline.
- WiFi is parked at "firmware up, no scan" (see WiFi part 1); internet works over USB (below).

### Done overnight (2026-10-21, 01:00-02:00): items 1-3 of the ranked plan
- **Panic backtraces** (kernel 0098): `/pram` + `embedded-panic-log-size` in the ADT (mkadt.py, 512 KB at
  0x1fd000000) un-stuck SavePanicInfo (it spun forever without a panic region); unbuffered panic output now goes
  into the persistent log ring (osfmk/kern/printf.c). `tools/flame-panic.sh RUN` symbolicates (atos, run's kernel).
  `tools/flame-cmd.py panic` panics on purpose.
- **Real time** (kernel 0099): SweetPlatformExpert::getGMTTimeOfDay = PM8150 RTC (SPMI 0x6048, read-only) +
  boot-arg `sweetrtcoff`, from `work/rtc-offset` (`tools/flame-rtc.py` calibrates against the Mac; flame-run.sh adds
  it). Publishing IORTC also removed a **30 s wait on every boot** (IOKitInitializeTime). Phone TZ is Pacific.
- **Stock apps on the home screen**: 32 removable apps were staged but never installed. installd failed three ways:
  clonefile unsupported on HFS+ (kernel 0100: copy fallback in clonefile_internal), extension-point lookups, and
  code signature validation (0xe8008001, no AMFI/trust cache). Workaround that works: the apps live in
  **/Applications** (root:wheel, from `work/staged_system_apps.tar`; backup on the phone in
  /var/local/staged-backup), placeholders removed, LaunchServices DB deleted and rebuilt by running
  `runmigrator /System/Library/DataClassMigrators/00LaunchServicesMigrator.migrator` (lsd itself waits for
  "seeding" forever otherwise). Home screen = Apple's default layout, two pages; Weather launches.
- **New phone tools** (phone/tools, in /var/local/bin): `psx [NAME]` / `psx -k NAME` (processes, kill),
  `pstack PID` (thread backtraces + shared cache slide; symbolicate with `ipsw dyld a2s` on addr - slide),
  `runmigrator BUNDLE` (DataMigration plugin by hand, wrapper entitlements), `expt` (extension point catalog),
  `lsreg` (LSApplicationWorkspace; its calls return nothing so far).
- **Remote UI** (kernel 0101): `tools/flame-cmd.py tap X Y` / `swipe X1 Y1 X2 Y2 MS` (panel pixels) inject touches
  through SweetHID. Unlock: `swipe 540 2270 540 1100 300`.

### Internet over the USB link (2026-10-21, 08:00) - Safari browses
- **Mac:** `tools/mac-share.sh` (root): IP forwarding + one pf NAT rule in anchor com.apple/sweet-share,
  192.168.7.0/24 out the default interface. Not macOS Internet Sharing (it would re-address the link). From
  Claude: `osascript -e 'do shell script "<abs path>/tools/mac-share.sh" with administrator privileges'`
  (the `!` prefix cannot prompt for sudo). Rerun after a Mac reboot/network change; `... off` undoes it.
- **Phone:** `netsvc` (phone/tools, SCDynamicStore write entitlement; SC functions declared by hand, the iOS SDK
  marks them unavailable) publishes ue0 as a service: IPMonitor makes it primary (router 192.168.7.1), DNS
  1.1.1.1/8.8.8.8. **Rerun `netsvc` after every boot** (nothing persists it yet; next: a launchd job).
- `nettest [HOST] [URL]`: DNS, TCP, HTTPS via NSURLSession (HTTP 200 from apple.com's success page).
- `ping 192.168.7.2` from the terminal fails: Local Network privacy, not the link (forwarded traffic is fine).
- **Safari pages stay blank** (loads fine: URL bar, HTTP 200). Found/fixed on the way: kernel **0104** (planar
  IOSurfaces report summed bytes-per-row/element; CA logged "unexpected bytes-per-row" for 'b3a8'), and WebKit's
  GPU process (DOM rendering needs Metal) is off: `prefs -g|com.apple.mobilesafari WebKit2GPUProcessForDOMRendering
  bool NO` (phone/tools/prefs = `defaults` for iOS). Still blank with WebContent drawing 'w30r' surfaces itself
  (SpringBoard's w30r surfaces display fine, so format/IPC are OK): likely accelerated CG drawing needs Metal.
  Untested: `...AcceleratedDrawingEnabled bool NO` (set under WebKit*/WebKit2*/com.apple.Safari.ContentPage
  GroupIdentifier.WebKit2* keys, global + mobilesafari) - the relaunch tap missed Safari. Decision: go to Metal.

### Black wallpaper fixed (2026-10-21, 07:50): app extensions now register (kernel 0103 + ADT MACs)
Wallpaper and widgets (Batteries widget on the home screen) work. The chain, each found with the new tools:
- PosterBoard asserted `!_stateLock_switcherConfiguration` (-[PBFPosterExtensionDataStore
  _stateLock_setupSwitcherConfigurationForFirstTime], source line 3431): its first extension update came minutes
  late, after a client had already built the switcher configuration through the lazy getter.
- It came late/empty because LaunchServices had 189 apps but 1 extension record. Two causes:
  1. **installd took 5 s per bundle**: MobileGestalt's WifiAddress (old-style UniqueDeviceID) reads
     `IODeviceTree:/chosen` `mac-address-wifi0`, else waits 5 s for an IONetworkController (sdio/wlan/
     marconi-wifi). Fix: tools/mkadt.py adds mac-address-wifi0/bluetooth0/ethernet0 (locally administered
     02:50:49:58:45:4c/4d/4e; wifi = the one wland gives the WLAN firmware). Rebuild work/boot/flame-hi.adt.
  2. **Every .appex was rejected** by MobileInstallation inside lsd ("Extension cache does not have an entry
     for <point>"): ExtensionFoundation reads the signed extension point cache only after asking AMFI
     (IOService "AppleMobileFileIntegrity", selector 6, cdhash) whether it is in the trust cache. No AMFI in
     our kernel -> no cache in sandboxed lsd. Fix: **kernel 0103** `iokit/Kernel/arm/SweetAMFI.cpp`, a stand-in
     service answering "trusted" (other selectors logged + unsupported; boot-arg sweetamfi=0 disables).
- After both: `lsreg -r` (once; the database persists) -> 515 extension records, all 10 poster providers.
- New tools (phone/tools, /var/local/bin): **`oslog [-t SECS] [-l] [PROC...]`** streams the unified log
  (there is no `log` on iOS; private data is on via /Library/Preferences/Logging/com.apple.system.logging.plist),
  `exq` (EXQ_POINTS=1: extension records per point; EXQ_LIST=<point>), `lsreg -p APPEX` (plugin registration
  status code). `tools/flame-fresh-run.sh NAME [SECS]` (was flame-wifi-run.sh) boots kernel-0103.
- Screenshots with GPU present need `tools/flame-shot --stride 4352 OUT.png`.

### WiFi, part 1: the modem runs the WLAN firmware (2026-10-21, 05:30) - `wland up` on the phone
`phone/wifi` (build.sh -> /var/local/bin/wland, all from userland over /dev/pmem; kernel 0102 for PMEM_RPMH).
`wland up [SECS]` (0 = daemon) from a fresh boot: SMP2P item, modem boot, GLINK, QRTR, rmtfs, pd-mapper, TFTP.
Result: the modem boots, stays up (no err-fatal), loads wlanmdsp.mbn and publishes **WLFW (QMI 0x45)**.
- Files on the phone: /var/local/fw/modem = radio modem.mdt/bNN + modemr.jsn + from vendor.img (factory image,
  `debugfs -R "dump /firmware/X"`, work/vendor/fw): wlanmdsp.mbn, modemuw.jsn, bdwlan-flame.bin.
  /var/local/fw/rmtfs = backup/{modemst1,modemst2,fsg,fsc}.img (IMEI etc.: phone only, never a repo).
- Layers (Linux file each one follows): smem.c (smem.c), SMP2P in wland.c (smp2p.c), boot_modem (q6v5_pas.c),
  glink.c + link.c (qcom_glink_smem.c/native.c), QRTR in link.c (af_qrtr.c + ns), qmi.c (qmi_encdec wire format),
  rmtfs.c (andersson/rmtfs + downstream msm_sharedmem: 2 MB, hyp_assign HLOS+MSS), pdmapper.c (andersson/pd-mapper,
  table from the .jsn files), tftp.c (andersson/tqftpserv).
- What each fix taught (all were needed):
  1. Our SMP2P out item (428) must exist **before** the modem boots, else it never posts "ready". SMP2P in =
     435; the modem's entry is "slave-kernel": bit 0 err-fatal, 1 ready, 2 handover. Clear it before a restart
     (wland does), or the old "ready" fools the wait.
  2. SSR ack: mirror the modem's RESTART_DONE (in flags bit 0) into our RESTART_ACK (bit 1) after every restart.
  3. Without **rmtfs** the modem err-fatals ~40 s after boot (no SFR string in SMEM 421 - none at all).
  4. QRTR control packets are always 20 bytes; RESUME_TX = {cmd, client.node, client.port} - a wrong layout
     stalls every modem client after ~8 packets (QRTR flow control).
  5. GLINK intent ids are per channel: only keep IPCRTR's. Big packets need one-shot intents (~30 ms per
     RX_INTENT_REQ round trip), so TFTP blocks are capped at 1024 (fit the reusable 1152-byte intents).
- Restart the modem: `wland shutdown; wland up`. Logs: `wland up 60 > /var/local/wland.log`; WLAND_VERBOSE=2
  traces every intent/TX/RX_DONE.
- **WLFW works (06:55): FW READY, wlan config + mission mode accepted** (wlfw.c, ath10k qmi.c order). Chip
  0x30224, fw WLAN.HL.3.2.0.c9-00554. What it took:
  6. WLAN power before the modem boots (`wland wlan-power`): RPMh VRM ldoa1 752 mV, ldoa7 1800, ldoc2 1304,
     ldoc11 3104 (HPM, enable), rfclka2 on, and **the QDSS clock: QMP "{class: clock, res: qdss, val: 1}"**
     (mainline sm8150 wifi clocks). Without QDSS the firmware takes an exception when the cal report starts
     calibration: PFR "EX:wlan_process:0x1:WLAN RT:0x78:PC=0xb01976e8" (pd-mapper prints PD failure reasons).
  7. qmi_buf was 4 KB: the 6 KB BDF segments silently lost their data TLV (error 48). Now 8 KB, overflow loud.
  8. MSA (0x8bc00000, 1 MB) stays assigned across modem restarts: never take it back while WLAN runs (the SMC
     never returns). Same for rmtfs: one 2 MB buffer per phone boot (/var/local/fw/rmtfs/.buffer), else after a
     few restarts hyp_assign hangs in TZ. Both hangs wedge a CPU: reboot the phone (tools/flame-fresh-run.sh).
  9. BDF = vendor /firmware/bdwlan-flame.bin in 6144-byte segments; MAC address (0x33) sent before the cal report
     (locally administered 02:50:49:58:45:4c).
- `tools/flame-fresh-run.sh NAME [SECS]`: fresh run (kernel-0102, -G), waits for SSH. Use 1800 s: each modem
  restart costs ~70 s (most of it the 3.7 MB wlanmdsp.mbn over TFTP).
- Next: copy engines at 0x18800000 + HTC + WMI scan (ath10k snoc.c/ce.c/htc.c/wmi-tlv.c), from userland,
  polled; CE rings in pm_alloc memory (WLAN DMA is apps SMMU stream 0x640: check whether it is bypassed).

### Ranked plan (2026-10-21, 01:00) - work top-down, update as items land
State: iOS 16.7.16 boots to the home screen on flame; touch, 8 cores, USB Ethernet + SSH + file server; GPU
up in the kernel (296 ms), every frame scaled to fit by the A640 (kernel 0096/0097, `flame-run.sh -G`);
Metal loads our SweetMetal.bundle; the kernel GPU user client works from userland (`mtlk`: submit 0.03 ms).
Missing: real time, WiFi/BT/cellular, audio, sensors, camera, most stock apps, GPU-rendered UI.

Ranked by leverage (value to the user and to later work / effort):
1. ~~**Panic backtraces in the RAM log**~~ DONE (tooling, small): the last 6 panics printed no backtrace, so each cost a
   guess. Every later driver benefits.
2. ~~**Real time from the PM8150 RTC**~~ DONE (small): the date is 1969; TLS (any internet over WiFi), logs, lock screen,
   certificate checks and many daemons need it. SPMI access already exists (sweet_spmi.c).
3. ~~**Install the 32 staged stock apps**~~ DONE (small-medium, very visible): /private/var/staged_system_apps has
   Calculator, Clock, Notes, Weather, Maps, Calendar, Mail, Music, Files, Health, ... never installed (no
   /var/containers/Bundle/Application): the first-boot data migration that installs them did not run.
4. **WiFi** (large; the user's top wish: scan nearby APs, then join): WCN3990-class WLAN in the SoC (Linux
   ath10k_snoc, "qcom,wcn3990-wifi"), firmware runs on the modem DSP: boot MSS (PAS/MBA, like the zap shader),
   QRTR/QMI over SMEM+GLINK to the WLFW service, then ath10k copy engines + WMI for scans. iOS side: wifid
   expects IO80211Family; first a scan tool, then a minimal IO80211 interface for Settings.
5. **Metal driver for Core Animation** (large, performance): queue/command buffer/encoders over the user
   client, AIR->NIR->ir3 for CA's shaders, render passes as A6xx streams.
6. Stock-app hardware, as apps need it: audio (WCD9340 via ADSP/SLIMbus), sensors (SLPI), camera, BT,
   battery/charging accuracy, brightness, vibration, GPS, cellular.
Tooling items folded in as needed: faster kernel iteration, panic symbolication (`ipsw` + kernel nm).

### Metal driver, stage 2 (2026-10-21, 08:30) - command queue works, next: library/pipelines
- SweetMetal.m: the logging forwarder is generic (`route_class(cls, "BaseClass", protocols)` + `ROUTED("Base")`
  per class: unimplemented base/protocol selectors log first use as `MTL[pid name]: Class -sel (base|MISSING)`).
  SweetMTLCommandQueue (_MTLCommandQueue; device/completionQueue are the driver's, the base lacks them) and
  SweetMTLCommandBuffer (_MTLCommandBuffer): submitCommandBuffers:count: -> didSchedule..., then the base's
  completeCommandBuffers:count: (buffers complete at once, nothing encoded yet).
- Loop without rebooting: `tools/flame-metal-swap.sh` (build, swap binary, restart backboardd); boot with
  `tools/flame-fresh-run.sh NAME 1800 sweetmtl=1` (third arg = extra boot-args). Log: logs/runs/NAME/live.txt.
- **Stage 3 (09:20): CA renders frames through SweetMetal** (nothing drawn into pixels yet: black screen with
  sweetmtl=1). Needed: device factories for textures (IOSurface-backed or CPU), buffers, depth-stencil/sampler
  states, pipeline states (record the descriptor), render/blit encoders (_MTLCommandEncoder), no pipeline
  archives (`newPipelineLibraryWithFilePath:` -> nil), `MTLFailureTypeSetErrorModeType(t, 5 nslog)` for t 0..4
  in the bundle's constructor (MPS "device is not supported" aborted SpringBoard), and two CA preferences
  (global, `prefs -g KEY bool NO`): **CAEnableDeepFramebuffer** (else the framebuffer is 'b3a8' = BGRA10_XR,
  "Unexpected framebuffer format b3a8", and CA renders nothing) and CAEnableFramebufferCompression/
  CAEnableIOSurfaceCompression (probably not needed). CA's probes then real passes: 1125x2436 BGRA8 iosurface
  targets, ~3-10 draws per pass.
- **Shader set for the home screen** (`pipeline N:` lines): fixed_vert_lph_spc + fixed_frag_lph_cph (blend on/
  off) = the compositor, luma_boost_lph, home_affordance_base_lph, attachment_clear_frag_lph, path_blit_vert,
  std_vert1, and the downsample/narrow_blur family. fixed_frag_lph_cph is an uber shader (104 KB AIR)
  specialised by function constants (fc_image_function0/1, fc_coord_function0/1, fc_filter0/1,
  fc_color_opcodes0/1, fc_color_inputs0/1, fc_plane_count, fc_texcoord_count, fc_blend_function,
  fc_destination_function, fc_image_count, fc_extended_range, ...). `tools/metallib.py LIB [NAME OUT.bc]` lists/
  extracts functions (research/gpu/QuartzCore.metallib), `/opt/homebrew/opt/llvm/bin/llvm-dis` reads them.
- Function constants of CA's fixed_* pipelines are **empty**: it uses the unspecialised uber shader, whose
  configuration comes per draw at runtime (setFragmentBytes/buffers). So: one compositor shader + a few small ones.
- Per-draw arguments (first 12 draws logged as `draw N:` + indented lines): every draw is an indexed quad
  (6 u16 indices 0,1,2,2,1,3); state persists between draws (Metal semantics). fixed_*: vertex buffers 1-3 and
  fragment buffers 1-2 are offsets into shared 256 KB MTLBuffers, fragment texture 3 (icons: 96x96 fmt 30;
  luma_boost: 128x1 R8 LUT); blend One / OneMinusSourceAlpha (premultiplied). The data holds float positions
  (512.0, 1152.0, ...), half colours (3c00 = 1.0) and **64-bit buffer gpuAddress values** (0x1_6fb08b10 = our
  current CPU pointer): CA's shaders follow pointers, so GPU VAs must equal what -gpuAddress returns.
- **Design (decided 2026-10-21, 10:30)** - SweetMetal draws through turnip-generated state:
  1. CA specialises with function constants (descriptor.constantValues); Metal's library cannot without a
     compiler backend, so SweetMetal's library wrapper returns the plain function with the values attached
     (associated object), for sync and async calls (else "Metal failed to specialize fragment function").
     Present as an Apple family (Metal's default answers): non-Apple families made CA ask for
     fixed_frag_lpf_cpf, absent on iOS. Consequence: fc_framebuffer_fetch=1 (blending in the shader).
  2. Host: `tools/airspec.py LIB FUNC OUT.ll name=value...` specialises + optimises (fixed_frag 13k lines ->
     ~40 instructions). AIR metadata (!air.vertex/!air.fragment) gives each argument's role and index.
  3. Host: translate the small IR to GLSL (todo: tools/air2glsl.py; generic CFG via a switch-in-loop),
     binding convention: set 0 binding N = vertex Metal buffer N (UBO), set 1 = fragment buffers, set 2 =
     textures, set 3 = samplers; Metal y-up clip space -> flip.
  4. VM: `work/mesa/draw/capture.sh VS FS OUT [blend]` = turnip (TU_DEBUG=sysmem, noop drm-shim) draws one
     quad with work/mesa/draw/vkdraw.c (vertex layout = CA's: binding 1 stride 48, loc0 float4 @0, loc1/2
     float2 @16/24, loc3 half4 @32); `tools/fd cffdump` decodes. First capture: work/mesa/draw/cap1 (fixed_vert/
     fixed_frag by hand: mvp * position, colour).
  5. What the capture shows: one CP_SET_DRAW_STATE with ~30 groups. Address-bearing: group 1 (SP_VS_BASE +
     CP_LOAD_STATE6 of the VS), 8 (SP_PS_BASE + FS), 9 (VFD_VERTEX_BUFFER[n].BASE/SIZE), 11 (SP_GFX_/
     HLSQ_BINDLESS_BASE[set] -> descriptor memory, 64-byte slots), 12 (CP_LOAD_STATE6 ST6_UBO SS6_BINDLESS:
     UBO base/size per stage). The rest is per pipeline, address-free -> lift verbatim. Before draws: preamble
     (CP_SET_AMBLE), sysmem pass setup (RB_MRT[0] format/pitch/base, scissors, window offsets, MSAA off,
     RB_RENDER_CNTL...), after: flushes.
  6. Phone (todo): MTLBuffers/IOSurface textures in GPU memory (user client sel 1/3; -gpuAddress = GPU VA, so
     CA's embedded pointers work), command stream per command buffer: preamble + per pass setup + per draw
     CP_SET_DRAW_STATE (static template groups + generated 9/11/12, shader code copied to a GPU buffer) +
     CP_DRAW_INDX_OFFSET, submit at commit (sel 4), complete. Templates per pipeline from the host.
- Older note on the shader path choice: (a) generic AIR (LLVM IR) -> SPIR-V
  (rename air.* intrinsics, llvm-spirv) -> Mesa spirv_to_nir -> ir3, or (b) hand-written GLSL for the few
  specialisations CA uses, compiled by Mesa in the VM. Then textures/buffers in GPU memory (user client),
  render encoder -> A6xx draw streams (from turnip captures), present.
- Earlier, where CA stopped (backboardd crash-looped with sweetmtl=1): newDepthStencilStateWithDescriptor: (nil),
  newLibraryWithURL:error: (QuartzCore.metallib via _MTLDevice), newPipelineLibraryWithFilePath:error:,
  `-targetDeviceInfo` -> crash (Apple's compiler path). Next: our own SweetMTLLibrary/Function (_MTLLibrary/
  _MTLFunction: keep the AIR bitcode per function), depth-stencil/sampler state objects, render pipeline state
  (AIR -> NIR -> ir3 via Mesa), then textures from IOSurfaces, buffers, render encoder -> A6xx draw streams.

### Metal driver: first contact (2026-10-21, 00:15)
- **GPU present works** (kernel **0096** = 0095 + SweetAccelerator; built with `-mcpu=cortex-a55+crypto`, see
  below): bring-up 296 ms at boot, every frame is the whole iOS frame scaled to 1053x2280 (status bar and home
  indicator visible), ~3.0 ms per present (surface cache clean + blit at 257 MHz). Run: `flame-run.sh NAME -k
  work/boot/kernel-0096 -G`. Touch follows the fit (not yet tried by the user).
- **How Metal finds a driver** (Metal.framework, disassembled with `ipsw dyld disass`): `MTLRegisterDevices` iterates
  `IOServiceMatching("IOAcceleratorES")`; `getMetalPluginClassForService` loads
  `/System/Library/Extensions/<MetalPluginName>.bundle`, takes class `MetalPluginClassName` (or the principal class),
  requires a subclass of `_MTLDevice`; `[[cls alloc] initWithAcceleratorPort:service]`; `MTLAddDevice` asserts
  conformsToProtocol MTLDevice **and MTLDeviceSPI**, then initLimits / initFeatureQueries.
- **Kernel:** `iokit/Kernel/arm/SweetAccelerator.cpp` (class IOAcceleratorES + SweetAccelerator, IOResources
  personality in KernelConfigTables.cpp), published only with boot-arg **`sweetmtl=1`**.
- **Userland:** `phone/metal/SweetMetal.m` -> `/System/Library/Extensions/SweetMetal.bundle`
  (`tools/flame-metal-install.sh`; root-fs binaries are platform binaries, so backboardd loads it). Stage 1 is a
  logging device: all 585 selectors of _MTLDevice + MTLDevice + MTLDeviceSPI go through forwardInvocation:, first use
  logged to /dev/console as `MTL[pid name]: -selector (_MTLDevice | MISSING)`.
- **Result (run mtl3):** backboardd -> CAMetalContextCreate -> MTLCreateSystemDefaultDevice -> our device; device
  registration passes (featureProfile and maxBufferLength missing, return 0); then `-newCommandQueue` ->
  `_MTLDevice newCommandQueueWithDescriptor:` crashes (NULL): a driver must supply its own _MTLCommandQueue subclass
  (base: initWithDevice:/initWithDevice:maxCommandBufferCount:, commandBufferWithDescriptor:, submitCommandBuffers:count:,
  completeCommandBuffers:count:). backboardd crash-loops with sweetmtl=1: don't use it for normal boots.
- **Shaders:** QuartzCore.metallib's 141 functions are LLVM bitcode modules (wrapper magic 0x0b17c0de) that Homebrew
  `llvm-dis` reads as plain LLVM IR (triple air64-apple-ios16.7.0), e.g. std_vert0_lpf = mat4 * position. So the
  translation path is AIR (LLVM IR) -> NIR -> ir3 (Mesa, in the VM), for the functions/constants CA requests.
- Faster loop than a reboot: replace the bundle (put to a temp name, `mv` over ssh) and `killall backboardd`.
- **Plan:** (1) kernel user client for SweetAccelerator: buffers (IOBufferMemoryDescriptor mapped to the GPU and the
  task), IOSurface import, submit + fence; (2) queue/command buffer classes; (3) textures/buffers; (4) AIR->NIR->ir3
  for the requested functions; (5) render encoder -> A6xx draw streams (from turnip captures of equivalent draws).

### GPU in the kernel: frames scaled to fit by the A640 (2026-10-20, 23:30, in progress)
- **Shared core** `bsd/dev/sweet_a6xx.c/.h` (kernel patch 0095): bring-up, SMMU banks (TCR2 SEP fix, 39-bit VA),
  ring/IB submission, 4K page mapping, and `a6_blit_ib()` = turnip's 2D blit rebuilt from the capture (A640 init
  registers + RB/GRAS/TPL1 A2D registers + CP_BLIT SCALE). The userland tool builds the same file
  (`phone/gpu/build.sh` compiles it from the xnu tree); `gpu test blit` checks it against a bilinear reference
  (worst 1.0), `gpu test fb` puts the fitted test frame on the panel (photo `work/gpu/cam-fb.jpg`). The blit takes
  ~5.9 ms on the GPU at 257 MHz (`GPU_TIME=1`).
- **Kernel service** `bsd/dev/sweet_gpu.c` (boot-arg `sweetgpu=1`; `flame-run.sh -G` adds it and the firmware):
  firmware in the ADT (`mkpayload.py --gpufw research/gpu/fw` -> /chosen `gpufw,<file>`), bring-up in a thread
  (**296 ms**, works), per-surface GPU mappings (4 slots at 4 GB + n*16 MB, pages from getPhysicalSegment after
  prepare()), cache clean of the surface, blit into the panel memory.
- **SweetFramebuffer** `presentGPU()`: one full-frame BGRA layer -> GPU blit fitted (1053x2280 at x 13); first time
  switches the panel to stride 4352 (`sweet_gpu_panel_stride`: VIG0/VIG1 YSTRIDE0 + CTL0 flush, live under
  autorefresh) and clears it. Falls back to the CPU copy (reasons logged once: "GPU present not used: ...").
  Touch follows the fit (`sweet_display_fit_x/w`, SweetHID.cpp).
- **Found:** the kernel's `-march=armv8.2-a+crypto` let clang emit **EOR3 (SHA3)**, undefined on A76/A55: panic
  "Undefined kernel instruction ... instr=ce1044c6" in a6_blit_ib (gpu9). makedefs/MakeInc.def now says
  `+crypto+nosha3+nosm4`; full rebuild (old objects in build/xnu.obj/DEVELOPMENT_ARM64_SWEET.old-0095b). Check new
  kernels with `xcrun llvm-objdump -d K | grep -E "\s(eor3|bcax|rax1|xar)"`.
- iOS surfaces must be prepare()d (wired) before getPhysicalSegment (gpu8: no GPU frames, silent fallback).

### GPU: Mesa command streams replay on the A640 (2026-10-20, 21:25)
- **Capture:** turnip (Mesa Vulkan) + the noop drm-shim in the Lima VM (`work/mesa/build-vk.sh` -> `~/mesa-vk-install`;
  local patch `work/mesa/drm-shim-a640.patch` adds the A640 to the shim). `limactl shell --workdir $PWD iosroot sh
  work/mesa/capture.sh PROG.c OUTDIR` runs a Vulkan program with `FD_GPU_ID=640 FD_RD_DUMP=enable,full`; the .rd it
  writes is **gzip-compressed** despite the name (gunzip before use). Example: `work/mesa/vkblit.c` = scaled,
  linear-filtered blit 1125x2436 -> 1053x2280 at x 13 in 1080x2280 (iOS frame -> panel, aspect kept),
  capture `work/gpu/cap-blit/blit.rd` (6 buffers from iova 0x100000000, one 184-dword stream with CP_SET_DRAW_STATE,
  CP_SET_AMBLE, CP_BLIT).
- **`gpu replay FILE.rd`**: each buffer gets its own /dev/pmem allocation mapped at its iova (4K pages, tables from an
  arena in the pool), then the streams are submitted. **Result: the blit ran on the phone; 60000 sampled channel
  values within 0.9 of a bilinear reference, side bars untouched** (`work/gpu/blit-result.png`).
- **SMMU fix:** TCR2.SEP must be 7 (upstream); at 0 VA bit 31 selects TTBR1 (off), so every GPU VA >= 2 GB faulted
  at level 0 (found with `gpu vatest [VA...]`). The GPU bank now uses T0SZ 25 (39-bit VA) too.
- Init now also has the rest of a6xx hw_init for the A640 (PC_DBG_ECO_CNTL 0x180000, UBWC/NC mode regs for highest
  bank bit 15, CP_PERFCTR_CP_SEL0). An IB *fetch* fault wedges the CP (needs a reboot; no GPU reset path yet); a
  faulting data write does not.
- **Next:** turn the captured blit into a parameterized stream (`tools/fd rddecompiler`, or read the register
  writes out of cffdump), then the kernel service: bring-up in XNU + per-frame GPU blit of the swapped IOSurface
  into the panel framebuffer (replacing SweetFramebuffer's CPU copy and its crop).

### GPU debug tools (2026-10-20, 21:00) - the GPU now runs from an SSH shell
Kernel **0094** (`xnu/patches/0094-flame-pmem-files-read.patch`, `work/boot/kernel-0094d`) + `phone/gpu/`:
- **`/dev/pmem`** (`bsd/dev/sweet_pmem.c`, root 0600, boot-arg `sweetpmem=0` removes it): pread/pwrite at
  offset = PA (managed DRAM via the physmap with cache clean+invalidate around each access; MMIO and
  carve-outs through 2 MB device windows, 32-bit accesses only), ioctls ALLOC (contiguous, zeroed, stays
  until FREE), FREE, LIST, SMC (resumes QCOM_SCM_INTERRUPTED like Linux; without that the zap shader's
  INIT_IMAGE returned rc 1). Two traps found: ml_io_unmap of an ml_io_map mapping panics (permanent entry),
  and a 2 MB ml_io_map panics (io_submap too small); windows use ml_io_map_unmappable.
- **File server `READ path off len`** (any file or device): `tools/flame-files.py read /dev/pmem PA LEN OUT`.
- **`pmem`** on the phone: `r/w/m/dump/load/alloc/free/list/smc` (`phone/gpu/pmem_tool.c`).
- **`gpu`** on the phone (`phone/gpu/gpu.c`, the shim's gpuprobe.c on a 16 MB pool from /dev/pmem):
  `gpu up [-p]` = power, GMU boot, HFI, GX, SMMU banks, SQE, ME_INIT, zap, CP_MEM_WRITE test (once per boot,
  ~2 s); `gpu test paint` (GPU draws the band into a buffer: every pixel checked, "0 wrong pixels (exact)";
  then on the panel); `gpu status`; `gpu dump [FILE]`; `gpu alloc NAME SIZE` (prints the GPU VA);
  `gpu read/write NAME`; `gpu submit FILE.bin` (IB from raw dwords; writes `/var/local/gpu/last.rd`; on a
  hang/fault writes `hang-N.txt` and clears the SMMU fault); `gpu clear`. Every log line also goes to
  /dev/console ("GPU: ..."), so the RAM rescue keeps it if the SoC hangs. State: /var/local/gpu/state.
- Mac side: `tools/flame-gpu-install.sh` (builds, installs pmem/gpu + firmware, syncs);
  `tools/pm4.py IN.txt OUT.bin` (PM4 assembler, opcode/register names from Mesa's XML: `pkt7 CP_MEM_WRITE
  lo(@out) hi(@out) 1`, `pkt4 CP_SCRATCH[2] 5`); `tools/fd cffdump X.rd` and `tools/fd crashdec -f
  hang-N.txt` (Mesa freedreno tools built in the Lima VM, `work/mesa/build.sh`, source
  `reference-repos/mesa`; crashdec names the fault address, finds the ring/IB and walks to the packet);
  `tools/flame-shot OUT.png` (framebuffer as an exact PNG, 1.2 MB/s, 8 s per shot; `--pa/--w/--h/--stride`
  for any surface).
- Loop: `tools/flame-run.sh NAME -k work/boot/kernel-0094d -s 900 -a "sweetusb=2 sweetusbframes=0"`, wait
  for `tools/flame-ssh`, `tools/flame-gpu-install.sh`, `tools/flame-ssh 'gpu up'`. Files written through the
  file server are lost on a panic unless synced (the install script syncs).
- Findings: a translation fault makes the GPU drop later writes (fence never lands) until the context
  bank's FSR is cleared; FSR 0x400 alone is just the format field, not a fault. The panel framebuffer is
  redrawn by the kernel's 60 Hz copy of the iOS frame (early in boot it holds the console text; once
  SpringBoard runs, `tools/flame-shot` shows the home screen: `work/gpu/shot-now.png`), so GPU output must
  be checked in its own buffers.
- Example: `work/gpu/t1.txt` (a write to unmapped VA 0x30000000), `work/gpu/hang-6.txt` and its decode.
- Next: port the bring-up into a kernel service (IOAccelerator-shaped), first real draw (2D blit engine or
  an ir3 shader), and the logging AGX stub to learn backboardd's user-kernel interface.

### Tools (2026-10-20 evening) - use these
- **`tools/flame-run.sh NAME [-k KERNEL] [-s SECS] [-a "ARGS"] [-p POKE] [-g REGEX] [-f FILM]`**: one experiment
  end to end (payload + image, fastboot boot, wait, rescue boot, TWRP, pull + decode, summary). Output in
  `logs/runs/NAME/` (xnu.txt, shim.txt, rescue.txt, cmdline, kernel). Leaves the phone in TWRP.
- **Timestamps**: every kernel log line in the ring starts `[sss.mmm]` (19.2 MHz timer).
- **RAM log rescue**: XNU mirrors its console ring uncached at PA 0x1fe000000 (from SweetPlatformExpert start);
  the next shim boot copies the tail to UFS (`tools/flame-log-pull.sh rescue OUT`). flame-run.sh does a rescue
  boot (`work/boot/shim-probe.img`) automatically. Recovers the lines a crash never flushed.
- **Poke scripts** (`bsd/dev/sweet_poke.c`, `mkpayload.py --poke FILE` -> ADT /chosen/sweet-poke): `@SECS`,
  `r ADDR [N]`, `w ADDR VAL`, `m ADDR MASK VAL`, `p ADDR MASK VAL MS`, `s MS`; output lines `POKE:`. Example:
  `tools/poke/apps-smmu-dump.txt`.
- **USB Ethernet + live log** (boot-arg `sweetusb=2`, kernel 0089+): Mac interface "Sweet iOS (USB Ethernet)"
  (en17), configure once: `networksetup -setmanual "Sweet iOS (USB Ethernet)" 192.168.7.1 255.255.255.0`
  (no sudo needed). `tools/flame-live.py [OUT] [--grep RE]` prints the kernel console live (UDP 5140).
- **Command server** (kernel 0090): `tools/flame-cmd.py 'r 0x... N'`, `-f script`, `reboot` (UDP 5141 on
  192.168.7.2; results come back in the live log). The Mac never gets an ARP reply from the phone (open
  issue), so flame-cmd falls back to the subnet broadcast 192.168.7.255, which works. `reboot` = PSCI reset ->
  fastboot in ~2 s; flame-run.sh then rescues and pulls as usual. With `-a sweetusb=2` flame-run.sh also
  records the live log to `logs/runs/NAME/live.txt` (stop it before using flame-cmd: one UDP listener).
- The shim's start.S uses adrp+add now (the image passed 1 MB with the GPU firmware + rescue buffer).

### SSH into iOS works (2026-10-20, 20:00)
- **`tools/flame-ssh ['cmd']`**: root shell in iOS (dropbear 22 via FlameBridge 127.0.0.1:2222, key
  `work/userland/ssh/id_ed25519`). `uname -a` = `Darwin iPhone 22.6.0 ... iPhone10,3`.
- **FlameBridge** (`tools/flamebridge/FlameBridge.app`, start with `open`): macOS Local Network privacy blocks
  unicast to 192.168.7.2 from this terminal (Ghostty has no NSLocalNetworkUsageDescription, so macOS never
  asks; "No route to host" even for the home router). FlameBridge has the key and the user allowed it; it
  relays TCP 127.0.0.1:L -> 192.168.7.2:R from `tools/flamebridge/ports.txt` (2222->22, 2223->2223) and UDP
  127.0.0.1:5142 -> :5141 (commands). Rebuilding the app may reset the permission: edit ports.txt instead.
- **File server** (kernel 0092+, `bsd/dev/sweet_files.c`, TCP 2223): `tools/flame-files.py put|get|mkdir|link|rm|chmod`.
  vfs wrappers `sweet_vfs_mkdir/symlink/unlink` at the end of bsd/vfs/vfs_syscalls.c.
- **Kernel 0093**: command server `sync`, and `reboot` syncs first.
- Userland (`work/userland`, scripts `tools/flame-install-ssh.sh`): dropbear (single-user build, root only,
  pubkey only; IOSONPIXEL_SINGLE_ROOT skips its "non-multiuser kernel" check), dash (configure with
  `ac_cv_func_strchrnul=no`: the iOS 27 SDK has strchrnul, iOS 16 does not -> weak import -> PC 0 crash;
  check new binaries with `nm -m X | grep "weak external"`), toybox (macos_defconfig, GNU sed, iOS SDK lacks
  sys/disk.h: patched). Built with `work/userland/bin/ios-cc`, signed `codesign -s -`. Installed in
  /var/local/bin, /bin/sh -> dash, /var/root/.ssh/authorized_keys, /var/root/.profile.
- launchd: new jobs must be keyed under **/System/Library/LaunchDaemons/** in
  /System/Library/xpc/launchd.plist (a /Library/LaunchDaemons key was ignored). Original saved as
  launchd.plist.orig on the phone and work/userland/launchd.plist.orig.
- ARP: the phone pads frames to 60 bytes and broadcasts ARP replies (en17's lladdr 02:00:00:00:00:00 differs
  from the 02:00:5e:77:ee:01 it uses on the wire). `arp -an` shows nothing without root on this macOS.
- Known: toybox `ps` needs /proc (no output); next: Darwin ps/top, a sync before reboots in scripts.

### USB result (18:10)
The apps SMMU blocks USB DMA after ABL unless stream 0x140 is matched: S2CR bypass is refused by the
hypervisor (reads back as fault), banks with page tables gave translation faults, sharing the UFS bank
stalled the thread. **What works is ABL's own way** (found with the poke dump): a free SMR -> our own bank
with CBAR 0x1f000, CBA2R 1, **SCTLR 0xe0 (translation off)**. Then the Mac enumerates "Sweet iOS (USB
Ethernet)", the CDC-ECM driver attaches and the live log streams (~3 KB/s, 148 KB in 45 s). The "stall"
seen first was the phone backing off while nothing listened on 5140. Open: no ARP reply reaches the Mac.

- **Newest (16:10):** the Adreno 640 runs our command streams from the shim (GMU boot, HFI, zap shader,
  SQE, ring) and has drawn pixels on the panel. See "Frame rate measured, GPU probe" below. iOS itself:
  8 cores at full clock, touch works, 8-25 fps in the UI (software rendering).
- **State (older):** iOS 16.7.16 userland boots on flame, with launchd, ~400 daemons, backboardd and SpringBoard building
  the lock screen, and no panic in 20 min. The screen shows garbage (a white band, dark blue, pink noise),
  and backboardd stops presenting after 21 frames. PosterBoard (the lock-screen wallpaper) traps and is
  restarted. Details are in the dated sections below.
- **Phone:** (morning note; now it ends each flame-run in TWRP) left in TWRP, plugged in. userdata holds root image v1, modified by the rw boots. If it needs
  a reflash, use the `adb exec-in dd` path, never `fastboot flash userdata`.
- **Next tasks:** GPU (see "GPU debug tools" above). Still open from earlier: the PosterBoard trap
  (`logs/20261020/ios3.txt`, CRASH lines), no ARP reply from the phone. (cpus=8, touch, USB: done.)
- **Loop:** `tools/flame-twrp.sh` (always), `tools/flame-log-pull.sh xnu|shim OUT`, film with
  `tools/watch-boot.sh DIR SECS IMG`. Boot image to reuse: `work/boot/shim-boot-0084h.img`.
- **Gotchas found today:** `fastboot boot` dies at the "unlocked" warning after ~10 s means the Power key
  reads as held (check `klog`). TWRP bounces back to fastboot means both slots are unbootable (the helper
  fixes this). If the Bash permission classifier errors out, ask the user to run the command with `!`.

## Where things stand

The approach is ios-sweet's: a bare-metal shim booted with `fastboot boot` acts as iBoot and starts a
patched XNU (xnu-8796, "SWEET" machine config, all drivers built into the kernel). That XNU runs the
unmodified iPhone X iOS 16.7.16 userland from an HFS+ image in `userdata`.

Done and checked on the phone:
- **Kernel build:** ios-sweet patches 0001-0081 plus flame 0082-0083 build **locally** with Xcode 27.1,
  a few minutes per rebuild (`tools/build-xnu.sh`). No GitHub CI needed.
- **Shim:** boots under Google's ABL. This needed three fixes the Redmi didn't (see NOTES.md, "Google ABL boot image rules").
- **Unattended probe loop:** the shim dumps hardware state to the end of `userdata`, sets the PMIC
  reboot reason, resets into fastboot by itself, and TWRP reads the dump back.
  It worked once end to end: `logs/shim-probe1.txt`.
- **Display:** ABL leaves the command-mode panel **auto-refreshing** from a framebuffer at
  `0x9c400000`, 1080x2280, stride 4320, a8r8g8b8. That's the Redmi's pixel format, so ios-sweet's
  display path should carry over.
- **Partition backups:** per-device partitions (persist, modemst1/2, fsg, fsc, ...) and LUNs sdb-sdf are in
  `backup/`, SHA-256 verified against the phone.
- **iOS 16.7.16 (20H392) IPSW parts:** in `downloads/ipsw/` (rootfs 087-86698-044.dmg with its `.mtree`,
  SystemOS 087-86593-047.dmg, AppOS 087-86715-047.dmg, DeviceTree, kernelcache). Extracted D22AP ADT in
  `research/ipsw-x/d22ap.adt`.

Not done yet (as of 2026-10-20): a correct picture on screen, touch, USB link, SMP. The XNU log and the iOS
root image are done (see below).

## 2026-10-20, 14:30: ALL 8 CORES AT FULL CLOCK (read first)

- `cpus=8` works on flame with no code change (ios-sweet 0065/0078 + mkadt's 8 CPU nodes): PSCI CPU_ON rc=0 and
  "came alive" for MPIDR 0x000-0x700, `SWEET CPU: 8/8 CPUs initialized` (`logs/20261020/s8.txt`).
- Kernel **0086** (`xnu/patches/0086-flame-cpufreq.patch`, also carries the 0085 s_ahb halt-check fix):
  `bsd/dev/sweet_cpufreq.c` writes each OSM cluster's DCVS_PERF_STATE_DESIRED (0x920) to its top all-core
  LUT entry. ABL had left the A76 clusters at index 0 (**710 / 825 MHz**); now A55 1785, A76 2419, prime
  2841 MHz (`logs/20261020/f1.txt`). Boot-arg `flamecpufreq=0` keeps ABL's setting, `=N` caps at N MHz.
  Voltage (CPR) and thermal (LMh) stay in hardware; heat under long runs not yet checked.
- Current image for the user: `work/boot/shim-boot-0086u.img` (cpus=8, 20 min).
- GPU: backboardd matches `AGXAccelerator` / `IOAcceleratorES` and finds none, so CA renders in software.
  See the GPU section below for the plan being scoped.

### Frame rate measured, GPU probe 1 (2026-10-20, 15:45)

- Kernel **0087** (`xnu/patches/0087-flame-vsync-fps.patch`): vsync ticks re-arm from fixed deadlines before
  the panel copy (they used to re-arm 16.7 ms after it, so "60 Hz" ran slower with jitter), and every 5 s
  `SWEET FB: 5s: N swaps (X fps), M panel copies, avg copy U us`. Image `work/boot/shim-boot-0087.img`.
- Result (`logs/20261020/fps1.txt`, user swiping home pages and scrolling Settings, cpus=8 full clocks):
  **8-25 fps while interacting, peaks 38**; the panel copy costs ~2.3 ms. So the limit is Core Animation's
  software renderer; no-copy scanout is not worth doing. The GPU is the fix.
- **GPU probe 1** (`work/boot/shim-gpuprobe.img`, `SHIM_CFLAGS=-DSHIM_GPUPROBE`, `shim/gpuprobe.c`,
  log `logs/20261020/gpuprobe1.txt`): GCC gpu clocks on, GPUCC CX GDSC on (`f8222000`), gmu_clk_src on XO,
  ahb/cxo/cx_gmu on; GMU registers read (fw_version reg `4ad21cd3` = stale, rpmh_state `f`); **ITCM and DTCM
  take writes**; **Adreno SMMU registers readable** (CR0 `230406`, IDR0 `4c017e06`, IDR1 `20000005`, IDR2
  `5555`): no hypervisor fault on reads. Next: SMMU context bank (or bypass) for the GMU stream, then GMU
  firmware boot + HFI.
- **GPU probe 2: GMU BOOTED** (`work/boot/shim-gpuprobe2.img`, logs `logs/20261020/gpuprobe2-4.txt`). The
  shim programs Adreno SMMU SMR0 = `84000005` (GMU stream 5, mask 0x400) -> CB0 (S1, AArch64 descriptors,
  T0SZ 32, tables at 0x1f0000000), maps icache 0x4000 / dcache 0x44000 (AP=00: privileged, executable;
  AP=01 there gave a permission fault on instruction fetch at 0x4dc0), dummy 0x60000000, log 0x60001000,
  HFI 0x60005000. RSCC is at **0x2c6a000 + 0x23000** for the A640 (0x2c50000 is A8xx only and hangs the
  bus). Then RSCC/PDC sequencer setup, 7 firmware blocks (fw 2.0.1.5), HFI table, CM3 reset release:
  **init result 0xBABEFACE, HFI_CTRL_STATUS 1**, no SMMU fault, GMU log filling. The RSC wake handshake
  (RSCC_CONTROL_REQ bit 1) does not ACK; ignored so far. Next: HFI messages (init, fw version, perf/bw tables,
  start), GX power-up by the GMU, then read GPU registers (RBBM), SQE + zap.
- **GPU probe 3: GX POWERED, GPU REGISTERS RESPOND** (`work/boot/shim-gpuprobe3.img`, log
  `logs/20261020/gpuprobe6.txt`). After the GMU boot the shim reads cmd-db aux levels (`gfx.lvl`: 0 16 48 64
  128 192 224 256 320 384 416; `cx.lvl`; `mx.lvl`), builds arc votes like a6xx_gmu_rpmh_arc_votes_init, and
  sends over HFI (all acked, error 0; acks are matched by the seqnum in ret_header, the reply id is the
  request id): PERF_TABLE (GPU 257/345/427/499.2/585 MHz, GMU 200 MHz), BW_TABLE (a640_build_bw_table),
  CORE_FW_START, START, GX_BW_PERF_VOTE (index 1 = 257 MHz), then OOB GPU_SET (H2G bit 30, ack bit 31):
  **GX GDSC `f8222800` (on), RBBM_STATUS 1, CP_SCRATCH_REG0 write/read OK**. The RSCC wake handshake still
  never ACKs and did not matter. Next: SQE microcode (a630_sqe.fw), zap shader via SCM PAS (id 13),
  GPU stream 0 in the SMMU, ringbuffer, CP_INIT, then a CP_MEM_WRITE to prove execution.
- **GPU probe 5: THE GPU EXECUTES COMMANDS** (`work/boot/shim-gpuprobe5.img`, log `logs/20261020/gpuprobe11.txt`).
  Order (as a6xx hw_init): GPU stream (SMR1 = `84010000`, id 0 mask 0x401) -> CB1 with its own tables (AP=01,
  unprivileged), hw_init register subset, SQE microcode (a630_sqe.fw minus its first dword) at GPU VA
  0x10000000 via **CP_SQE_INSTR_BASE = 0x830/0x831** (0x816 is the A8xx offset; the register database lists
  both, check `variants=`), ring at 0x10010000, CP_RB_CNTL 0x0800020c, CP_SQE_CNTL 1, CP_ME_INIT -> idle,
  **then** the zap shader via SCM PAS (init image / mem setup / auth all 0), CP_SET_SECURE_MODE 0,
  CP_MEM_WRITE -> **target memory = 0xcafef00d**, no SMMU fault.
- **GPU probe 6: GPU PIXELS ON THE PANEL** (`work/boot/shim-gpuprobe6.img`, log `logs/20261020/gpuprobe12.txt`,
  photo `logs/20261020/photos/first-gpu-pixels.jpg`): the framebuffer (0x9c400000) mapped into the GPU as
  2 MB blocks at GPU VA 0x20000000; one gradient row by CP_MEM_WRITE, 599 more by CP_MEMCPY (0x75). The CPU
  never touches the framebuffer. Next GPU steps: port this bring-up into XNU as a kernel service, then a
  first real draw (2D blit engine or a triangle with an ir3 shader from Mesa), then the Metal driver work.
- **USB networking (kernel 0088, `sweetusb=2`), 16:15-17:20, NOT working yet.** `bsd/dev/sweet_usb.c` now has
  flame GCC offsets (master 0xf010, cfg-noc-axi 0xf078, aggre 0xf07c, clkref 0x8c008), no QUSB2 reads, femto
  HS PHY dump at **0x088e2000** (primary port; 0x88e3000 is the 2nd controller), periodic DWC3 state dumps.
  Findings: ABL leaves DWC3 3.110a in device mode (SSP), PHY powered (utmi0 01, common0 88, ctrl2 06);
  with RUN_STOP the Mac sends SOFs (DSTS SOFFN counts, HS) **but DWC3 DMA fails** (GSTS bit 4 bus error,
  no events). Cause: the apps SMMU (qcom,qsmmu-v500 at 0x15000000, 94 SMRs, 56 CBs, sCR0 USFCFG=1) has no
  SMR for the USB stream 0x140 after ABL exits -> the first TRB fetch is an unmatched-stream global fault
  -> SoC reset. Tried: (1) S2CR bypass: reads back type 2 (fault), refused by the hypervisor; (2) own S1
  bank (cb55, then cb4), identity 2 MB block, tables read back right, NC walks: **translation fault at the
  first TRB** every time; (3) pointing 0x140 at the UFS bank (cb1): SoC reset. Next: read the downstream
  qsmmu-v500 driver (coral kernel drivers/iommu/arm-smmu.c: skip-init, 3-lvl tables, TBU at 0x15182000,
  ACTLR, possibly nested S2 by the hypervisor) or capture what Linux programs for cb/SMR of 0x140 from
  TWRP (read the SMMU registers under Linux while USB is up). DMA buffer option `sweetusbdma=PA`.
- User wants networking: plan is USB device-mode CDC-ECM (port ios-sweet 0072-0077 to flame's DWC3 + SNPS
  femto PHY), which also gives a live kernel log. Wi-Fi and USB-host Ethernet are much harder.

### GPU plan (scoping, 2026-10-20)

The Adreno 640 hardware is already documented by Mesa (freedreno/turnip, ir3 compiler) and Linux msm DRM; the
hard part is iOS's side. Plug-in point: an IOAccelerator-family kernel service (what backboardd matches) whose
properties name a userland Metal driver bundle, as AGXMetalA11.bundle does for the A11. Phases:
1. Kernel: Adreno bring-up in XNU (GPUCC/GMU power via RPMh, SQE/GMU firmware, zap shader via SCM, GPU SMMU,
   ringbuffer), proven first in the shim (read the GPU ID, run a trivial command stream).
2. Find exactly which Metal API surface and which shaders QuartzCore's Metal renderer (CA::OGL::MetalContext)
   uses; CA ships a fixed shader set, so those can be translated once (AIR -> NIR -> ir3) instead of a general compiler.
3. A minimal Metal driver bundle covering CA's subset: compositing on the GPU. Apps that use Metal directly come later.
Before that: no-copy scanout (display pipe reads iOS's IOSurfaces placed in the 36 MB splash carve-out). Lower
value than first thought: CA's software renderer reads back the target, and scanout memory must be uncached.

GPU phase 1 checklist (reference: mainline `reference-repos/linux-msm/drivers/gpu/drm/msm/adreno/a6xx_gmu.c`,
`a6xx_gpu.c`, DT `arch/arm64/boot/dts/qcom/sm8150.dtsi` gpu@2c00000 / gmu@2c6a000 / gpucc@2c90000 /
adreno_smmu@2ca0000; firmware from stock vendor.img in `research/gpu/fw/`: a640_gmu.bin, a630_sqe.fw,
a640_zap.mdt/.b0x; never commit these):
1. GPUCC: CX GDSC on; clocks gpu_cc_ahb, cx_gmu (200 MHz), cxo; GCC ddrss_gpu_axi, gpu_memnoc_gfx. Read a GMU
   register back (CX domain only).
2. Adreno SMMU (MMU-500, stream 0x400 GMU, 0x401 GPU): bypass or identity context bank. Unknown: whether
   Qualcomm's hypervisor traps these registers.
3. GMU: PDC sequencer + RSC setup (a6xx_gmu_rpmh_init), firmware blocks into ITCM/DTCM, HFI queues in memory,
   boot, HFI init/perf table (GX levels from cmd-db `gfx.lvl`), GMU powers GX.
4. Zap shader through SCM PAS (id 13), SQE microcode, CP init, ringbuffer; first proof: CP_MEM_WRITE of a
   magic value to memory.

## 2026-10-20, 14:00: TOUCH WORKS

- Kernel **0085** (`xnu/patches/0085-flame-fts-touch.patch`, `work/boot/kernel-0085`, boot image
  `work/boot/shim-boot-0085c.img`): new `bsd/dev/sweet_fts.c` drives the ST FTM5 over GENI SPI SE4. It votes RPMh
  L14A 1.8 V and L17A 2.856 V (`sweet_rpmh.c` now allows them), votes the wrap0 GCC clocks (s_ahb is HW-gated:
  its halt bit is skipped like Linux), muxes GPIO 51-54 to qup4, takes the bus from the SLPI (GPIO 126 low),
  pulses reset (GPIO 127), waits for the controller-ready event, reads chip id `36 39` fw 3.0, sends sense on
  (`A0 00 FF`), then polls the FIFO (`87`, one dummy byte) on IRQ GPIO 122 low. Coordinates are panel pixels
  (0..1079, 0..2279), passed straight to `sweet_hid_touch`. SweetHID now maps for the 2280-row panel.
- The user tested: tap, swipe-up unlock, home screen, apps open, corners: "working flawlessly".
- Boot-args: `flametouch=0` off, `flametouchdelay=N` (default 20 s), `flametouchlog=1` logs every event.
- One boot (0085a, first try) reset at ABL's "unlocked" warning before the shim ran (klog: PS_HOLD, not
  KPDPWR); a plain retry worked. Probably a button press during handling.
- Logs: decode the XNU ring with `tools/flame-log-decode.py RING.bin OUT.txt`.

## 2026-10-20, 13:20: iOS LOCK SCREEN ON THE PANEL

- **The "garbage" was the shim, not iOS.** `sde_autorefresh()` wrote CTL_START while ABL's INTF1 autorefresh
  was already on. That wedges the pipe: the panel keeps a half-drawn frame with pink noise on top and never
  updates again. Proven with `work/boot/shim-disptest.img` (`SHIM_CFLAGS=-DSHIM_DISPTEST`, `shim/disptest.c`):
  phases A (leave autorefresh alone), B (autorefresh off + manual kick per frame) and C (autorefresh back on, no
  kick) all update; D (autorefresh + kick) freezes. The fix is in `shim/sde.c`: never kick in autorefresh mode.
- **Boot 0084x** (`work/boot/shim-boot-0084x.img`, kernel 0084 unchanged, `sweetfb_noconsole=1 sweetfb_hz=30
  flamereboot=300`): XNU's console scrolls, then the **iOS 16 lock screen**: black wallpaper (PosterBoard
  crashes), red date "Wednesday, December 31" (no RTC: epoch) and a ticking clock, camera button. Photo:
  `logs/20261020/photos/first-lockscreen-0084x.jpg`. backboardd keeps presenting (the clock advances).
- Older notes below about pink noise and "21 swaps" are explained by this; ignore those leads.

## 2026-10-20, 10:20: iOS USERLAND RUNS ON FLAME

Boot **0084r** (`work/boot/shim-boot-0084r.img`) on root image v1 (`work/iosroot/iosroot-v1.img`, written from
TWRP with `adb exec-in dd` and checked with SHA-256 per GiB). Log: `logs/20261020/ios2.txt`.
- The sequence was hfs mounting iOSRoot (journal initialized), launchd (libignition level 1, the shared cache
  found), the boot tasks (mount, data-protection, FIPS POST all passed, keybagd with the system keybag
  loaded, usermanagerd), then ~300 daemons, backboardd (AppleCLCD, 1125x2436 IOSurfaces, `SWEET FB: swap
  2..7`) and SpringBoard. There was no panic. The run ended at `flamereboot=300`.
- The screen showed a uniform blue with a thin white strip at the earpiece end, most likely the first iOS
  frame(s). The sampler shows SpringBoard's main thread waiting. Only 7 swaps happened in 5 minutes on 1 CPU.
- Next: **0084h** (`work/boot/shim-boot-0084h.img`) adds ios-sweet's home-screen args `sweetfb_noconsole=1
  sweetfb_hz=30` and uses `flamereboot=1200`. Later: `cpus=8` (patch 0078; flame MPIDRs are in the ADT),
  touch (ST FTS over GENI SPI), USB.
- **0084h result (10:18 boot, filmed 11 min: scratchpad `ios3/boot.mp4`, not kept):** the screen shows a white
  band at the USB end, dark blue in the middle, and **changing pink noise over the top half**. Something keeps
  writing there, perhaps CPU-rendered IOSurfaces that are not initialized, or a wrong stride/offset in the
  `sweetfb_hz=30` swap path. The phone ends at `flamereboot=1200` (about 10:39) and resets into fastboot by
  itself. **First thing next session:** `tools/flame-twrp.sh`, `tools/flame-log-pull.sh xnu
  logs/20261020/ios3.bin`, decode it (python in the 0084a/ios2 notes), and look for `SWEET FB: swap` lines,
  SpringBoard crashes, and the SMP samples.
- **0084h log** (`logs/20261020/ios3.txt`, 246 KB, ran the full 20 min): no panic. Only 2 crashes:
  driverkitd (sig 9) and **PosterBoard** (sig 5 = trap, pc 0x1d35f8268, slide 0x11e80000, so unslid about
  0x1c1778268 in the shared cache; launchd restarts it). ios-sweet's notes never mention PosterBoard. It renders
  the iOS 16 lock-screen wallpaper, so it is a prime suspect for the garbage on screen. SpringBoard builds the
  lock screen (SBCoverSheetPrimarySlidingViewController, CATransformLayers, its own small IOSurfaces) and keeps
  running. backboardd made only **21 swaps**, all in the first minutes (display off/on once in between), then
  none. Next: symbolicate PosterBoard's backtrace against the 20H392 shared cache (blacktop `ipsw dyld
  a2s`), and dump the swapped surface (or the framebuffer) to UFS to see what iOS actually drew.
- The diag shim (`work/boot/shim-diag.img`) once showed pink noise instead of its text after a Linux boot.
  Same family of problem? Check what the display pipe really scans out (the probe shim logs it).
- Lima VM `iosroot` is stopped. Start it with `limactl start iosroot`.
- The Power key looked stuck from about 09:14 to 10:00 (see below). If boots die at the "unlocked" warning
  after about 10 s, check `klog` for `reboot_longkey`.

## 2026-10-20 update

**TWRP blocker solved, and XNU 0084 boots to "mount root" on flame** (`logs/20261020/xnu0084a.txt`). The sequence
was VM, IOKit, platform expert and WDT pets, AppleCLCD 1080x2280, CPU0, BSD, network, `SWEET UFS: "userdata" ...
read-write`, keys, `BSD root: ufsroot0`. Then `hfs_ValidateHFSPlusVolumeHeader: unknown Volume Signature : 0`
repeated, because userdata was empty. `flamereboot=90` then stopped the pets and the WDT reset the phone into
fastboot.

What the TWRP problem actually was:
1. **Slot exhaustion.** Every boot takes a retry from the active slot and nothing marks one successful. With
   both slots unbootable, ABL still accepts `fastboot boot`, but TWRP's first-stage init cannot mount its slot
   partitions and reboots into fastboot after about 14 s. Fix: `fastboot set_active a` before each TWRP boot.
   `tools/flame-twrp.sh` does this and waits for adb.
2. **Volatile state that survived warm resets.** After the slot was reset, TWRP hung on the Google splash
   until a long Power+VolDown press, which is a PMIC hard reset. ABL left PS_HOLD at WARM_RESET (`0x85a=1`).
   The shim now sets HARD_RESET (7) like Linux does (`set_hard_reset()`; log line `ps_hold reset: ctl 1 -> 7`).
   The bootloader reboot reason still survives.
- Partitions compared with `backup/SHA256SUMS`: only expected changes (backup GPT slot attributes, modemst EFS
  sync, klog/metadata/misc/persist).
- **0084** maps the early framebuffer block as Device-GRE, not Normal-NC, so the CPU never reads into pil_slpi
  speculatively. Patch `xnu/patches/0084-flame-early-fb-device-memory.patch`. Next boots also use `sweetusb=0`.

Tools:
- **Camera:** an iPhone films the Pixel. AVFoundation device "iPhone Xs Camera".
  `tools/camsnap/camsnap OUT.jpg "iphone xs" 1` takes one frame. `tools/watch-boot.sh DIR SECS [IMG]` films a
  boot. If frames come out black or blurred, the iPhone camera session paused (lock screen); ask the user.
- `work/boot/shim-showlog.img` (`SHIM_CFLAGS=-DSHIM_SHOWLOG`) pages the stored shim log and the XNU ring tail on
  screen, so logs can be read without Linux. It is built but not yet run on the phone.
- **iOS root build:** Lima VM `iosroot` (Ubuntu 26.04, vz, project mounted read-write at the same path), with
  apfs-fuse built from source (`-include cstdint`), hfsprogs and img2simg. OrbStack's kernel has no hfsplus, so
  it can't be used. Run `limactl shell iosroot bash -c '... tools/mkiosroot.sh ...'`.
  Output in `work/iosroot/` (log `mkiosroot-v1.log`).
- Root-boot image: `work/boot/shim-boot-0084r.img` (0084, cmdline with `rootrw=1 flamereboot=300
  ufslogsecs=2`). Next: tweaks (`sweet-root-tweaks.py` in the VM), `img2simg`, `fastboot -S 512M flash
  userdata X.simg`, then boot 0084r while filming.

### Writing userdata, and the 09:15-09:45 reset cycle (2026-10-20)

- `fastboot -S 512M` fails on flame ("No enough buffer"; max-download-size is 256 MiB). `fastboot -S 240M flash
  userdata iosroot-v1.simg` sent all 50 chunks, then **ABL hung writing chunk 50** for 25+ min. That chunk is
  long zero FILLs plus the HFS+ alternate header. Don't flash userdata through ABL. Write it from TWRP instead:
  `adb exec-in 'dd of=/dev/block/sda15 bs=4194304' < IMG`, then compare against
  `work/iosroot/iosroot-v1.gib.sha256` (SHA-256 per GiB).
- After that, the phone reset every ~10-15 s, even on battery and even in fastboot. **`klog`** (the
  by-name/klog partition, readable from TWRP with `strings`) holds XBL/ABL's own log with reset forensics:
  `reset message: reboot_longkey`, `gcc_reset_status=PMIC_RESIN:PMIC_ABNORMAL_RESIN`, PON history
  `WARM RST=0x80:KPDPWR_N`, then `abnormal: warm. Force reset...`. So the PMIC saw the **Power key as held**,
  and its long-press warm reset fired over and over. It stopped after the user clicked the buttons, and the
  user reached TWRP through the Recovery menu entry. Watch for a sticky Power key. Read `klog` first after any
  unexplained reset.
- `fastboot erase userdata` did not stop the cycle (the userdata contents were not the cause).
- TWRP has no `bootctl`, so keep using `set_active a` before TWRP boots (`tools/flame-twrp.sh`, which also
  waits for ABL to re-enumerate).

## (Solved 2026-10-20) The blocker: TWRP no longer boots

**Symptom:** `fastboot boot downloads/twrp/twrp-3.7.1_12-0-flame.img` gives "Booting OKAY". The phone leaves
fastboot and comes back to fastboot 14-15 s later, with no adb. This happened 4 times in a row and is
reproducible.

**When it started:** right after the first XNU boot (`work/boot/shim-boot-0083a.img`, 22:05:49). That boot
also came back to fastboot after about 14 s. Before it, TWRP booted fine every time, including right after
probe runs.

**What the timing means:** about 15 s is simply ABL's time to come back to fastboot after an early reset; a probe
run (shim work under 1 s) takes the same 15 s. So TWRP (and probably XNU) dies within seconds of
starting, not at 14 s. Don't read meaning into the 14.

**Ruled out:**
- The watchdog: a probe build that turns the watchdog *off* didn't help.
- The corrupted f2fs on `userdata`: I erased `userdata` (`fastboot erase userdata`, about 22:18) and it didn't help either.
  Android user data is gone. It had to go for the iOS root anyway, but I did it on a guess.

**Still works:** the probe shim runs and returns to fastboot. I just can't read its logs without TWRP.

**Hypotheses, in the order to test:**
1. **Leftover volatile state.** Since the XNU run, every reset has been a warm reset, so DRAM, IMEM
   cookies and PMIC registers survive. A crash, or an XPU fault from XNU, may have left a crash or
   download-mode cookie in IMEM (`dload_type@146bf01c`, `mem_dump_table@146bf010`, `rst_info@146bfd38`)
   that changes how the next Linux boot behaves.
   **Test: cold power cycle.** In the fastboot menu, press Volume down to "Power off" and press Power, wait
   10 s, then hold Power + Volume down. Then retry `fastboot boot` of TWRP.
2. **TWRP-specific.** Try `fastboot reboot`, which boots TWRP from `boot_a` by the normal path. Also try
   `fastboot boot stock/boot.img` (stock Android kernel; Android itself can't finish booting, but USB
   enumeration shows whether the kernel lives).
3. **Something written outside `userdata`.** Unlikely: every write path is bounds-checked to `userdata`, and the
   PMIC write is only `SOFT_RB_SPARE`. Once a shell works again, compare partitions and LUNs against
   `backup/SHA256SUMS`.

**Probable cause of the XNU reset (separate from the TWRP problem):** the early console maps the whole 32 MB block
`0x9c000000-0x9e000000` as Normal-NC (`start.s` and `sweet_early_map_final` in `arm_vm_init.c`).
On flame that block also covers the tail of `pil_slpi_region` (`0x9ab00000-0x9c300000`). Speculative
reads into an XPU-protected carve-out can reset a Qualcomm SoC. On the Redmi the framebuffer *was* the block
start. **Fix before the next XNU boot:** map only the framebuffer pages (an L3 table for that block, or
Device-nGnRE outside the FB range).

**Make this class of problem impossible:** give the shim its own USB device mode (ABL leaves the DWC3 at
`0xa600000` running; ios-sweet's `bsd/dev/sweet_usb.c`, patches 0072-0075, has the ep0 and bulk code), so
logs can be read with no Linux at all. Probably worth doing right after unblocking.

## Phone state at stop

- In **fastboot**, USB to the Mac, battery 4354 mV. Leaving it in fastboot overnight is fine.
  Powering it off from the fastboot menu instead *is* test 1 above.
- `boot_a` = TWRP 3.7.1_12-0 (flashed by me 2026-10-19). `boot_b` = stock. Active slot a. `userdata` erased.
- PMIC PON reason was set to "bootloader" by the last shim run, so the next reset lands in fastboot.
- Restore stock at any time: `images/flame-tp1a.221005.002.b2/flash-all.sh` (see RECOVERY.md).

## How to run things

```sh
# kernel (incremental; first-time full build was blacktop build.sh, see work/build-try1.log)
tools/build-xnu.sh work/build-NNNN.log   # -> work/dxb/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet
# new kernel patch: edit work/dxb/xnu (a git repo, HEAD = baseline + 0082 + 0083), then
#   (cd work/dxb/xnu && git diff > ../../../xnu/patches/NNNN-flame-*.patch && git commit -qam NNNN)

# ADT, payload, boot image (build.sh needs ABSOLUTE paths: it cd's into shim/)
python3 tools/mkadt.py work/boot/flame-hi.adt --highmem
python3 tools/mkpayload.py work/boot/kernel-0083 work/boot/flame-hi.adt work/boot/payload-X.bin --cmdline "..."
shim/build.sh $PWD/work/boot/payload-X.bin $PWD/work/boot/shim-boot-X.img
shim/build.sh                               # probe-only image: shim/out/shim-boot.img

# run + read back
fastboot boot work/boot/shim-boot-X.img     # waits for the reset into fastboot
fastboot boot downloads/twrp/twrp-3.7.1_12-0-flame.img
tools/flame-log-pull.sh shim logs/X.txt     # shim log (64 blocks before the XNU ring)
tools/flame-log-pull.sh xnu logs/X.bin      # XNU persistent console ring (last 1025 blocks)
```

The 0083a cmdline: `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 ufsrw=1
unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin launchd_unsecure_cache=1 wdt=-1 flamereboot=90`

Shim behaviour (current `shim/shim.c`):
1. Logs CPU, PSCI, PMIC, UFS and SDE state.
2. Sets the PON reason to bootloader.
3. In probe mode, turns the watchdog off and resets with PSCI.
4. With a payload, arms the watchdog (30 s, tick rate assumed 32765 Hz, **unverified**) and jumps to XNU.

XNU patch 0083 pets the watchdog from `SweetPlatformExpert::start`: `flamewdt=0` turns it off,
`flamereboot=N` stops the pets after N s to bound a run.

## After unblocking: road to the home screen

1. Fix the framebuffer block mapping (above), then boot XNU again until the persistent log shows the boot reaching
   "waiting for root". The XNU ring in `userdata` is written once the UFS driver is up (`ufsrw=1`).
2. **iOS root image on macOS without sudo.** Ownership matters.
   - `hdiutil create` a case-sensitive journaled HFS+ image of about 16G, attach it (owners are ignored), and
     `ditto --nopreserveHFSCompression` from the attached APFS DMGs. XNU's HFS has no decmpfs
     compressors, so files must be stored uncompressed.
   - Add ios-sweet's extra links and files (`tools/mkiosroot.sh`).
   - Rewrite owner, mode and flags in the HFS+ catalog records from `Firmware/087-86698-044.dmg.mtree`
     (cryptexes: root:wheel), then check with `fsck_hfs`.
   - Fallback: a Linux VM (lima) running ios-sweet's scripts as root.
3. Root tweaks (`tools/sweet-root-tweaks.py`), then `fastboot flash userdata`,
   then boot with `rootrw=1`.
4. **Touch:** a new driver for ST FTS (ftm5) on GENI **SPI** SE4 `0x890000`. It needs:
   - RPMh votes for L14A (1.8 V) and L17A, and TLMM GPIO 122 (irq) and 127 (reset).
   - The Redmi Goodix driver is off by default (patch 0083); never enable it here: it would put 3.2 V on L11C.
5. The display is presented as 1125x2436 and the 2280-row panel crops 78 rows at the top and bottom (the status bar and
   home indicator are partly cut). Downscale later if it matters.
6. Then a USB live log (ios-sweet 0072-0077), `cpus=8` (flame: 4x A55 + 3x A76 + 1x A76 prime), battery.

## Gotchas

- macOS asks "Allow accessory to connect?" for every new USB identity (TWRP, any future shim USB
  device). Until someone clicks Allow, `ioreg -p IOUSB` shows the phone `!registered` and adb sees nothing.
- `grep` in this shell is ugrep and `\b` patterns misbehave; use `/usr/bin/grep`.
- TWRP debugfs SDE access: `echo "2000 1e0" > /sys/kernel/debug/dri/0/debug/sde_off`
  (hex, no `0x`), then `cat .../sde_reg`. Disable idle power collapse first (`encoder27/idle_power_collapse`).
- Nothing is in git for the project itself, and nothing has been pushed anywhere. Keep `backup/`, `dtb/`, `stock/`,
  `downloads/`, `work/` and `logs/` out of any future repo: they hold serials, IMEI data and Apple files.
