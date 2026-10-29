# Credits and licences

This repository is a fork of ios-sweet and keeps its [MIT License](LICENSE). The flame additions are under
the same licence (Copyright (c) 2026 the ios-flame contributors), with these exceptions:

- `xnu/patches/` modify Apple's XNU, which is under the **Apple Public Source License 2.0**. The patched
  files stay under the APSL 2.0.
- Files that came from other projects keep their own licence, listed below.

Nothing from Apple's iOS, Google's images or Qualcomm's firmware is in this repository; see the README for
what you need to fetch yourself.

## Projects this builds on

- **ios-sweet** (dipa-studio, <https://github.com/dipa-studio/ios-sweet>, MIT): the XNU port to the Redmi
  Note 10 Pro this is forked from. Patches 0001-0081, the shim and most of `tools/` started there.
- **XNU** (Apple, APSL 2.0): xnu-8796.141.3.
- **ios-topaz** (MakrSas, <https://github.com/MakrSas/ios-topaz>): the Redmi Note 12 4G fork of ios-sweet.
  - `phone/wifi/radio.c` is a userland port of its TopazModem radio (its patches 0123-0130).
  - Our patch 0123 contains its 0900 (en0 + Apple80211 for wifid) and 0128 (IOKit quiesce).
  - ios-topaz's CREDITS.md puts the HackiOS-derived kernel parts under the APSL 2.0.
- **HackiOS** (OOSEX GANG, <https://github.com/oosexgang/hackios-xnu-kernel>, APSL 2.0): the origin of the
  Apple80211/wifid code above (`redwood_net`, `redwood_apple80211`, `redwood_sta`, `RedwoodWLAN`,
  `MiniIONetworkStack`), the keystore wrap/unwrap and the 10-bit surface formats in 0123/0124.
- **UniXNU** (<https://github.com/UniXNU/UniXNU>, BSD-2-Clause-Patent at the top level):
  - `phone/metal2/NativeBridgeMetalFrontend.mm`: NativeMetal, MIT, patched for iOS 16.
  - `phone/metal2/bridge/` (NativeMetalBridge/NativeMetalObjects): NativeMetal, MIT; see `bridge/LICENSE`.
  - `phone/turnip/NativeAdrenoABI.h`: NativeAdreno, MIT.
  - The Mesa patches that `tools/build-turnip-ios.sh` applies; fetched, not copied.
- **metal2vulkan** (<https://github.com/steelbrain/metal2vulkan>, GPL-2.0-or-later OR LGPL-3.0-or-later):
  fetched at UniXNU's pin by `tools/build-m2v-bridge.sh`. `phone/metal2/bridge/patches/` are patches to it and
  fall under its licence. A built SweetMetal2.bundle links it statically.
- **Mesa / Turnip** (<https://gitlab.freedesktop.org/mesa/mesa>, MIT): the Vulkan driver for the Adreno 640,
  fetched by `tools/build-turnip-ios.sh`. `phone/turnip/patches/` are patches to it.
- **Linux**: `phone/wifi/` follows the drivers named in each file's header: ath10k (ISC) and the Qualcomm SMEM,
  SMP2P, GLINK, QRTR and remoteproc drivers (GPL-2.0). It also follows Bjorn Andersson's rmtfs, pd-mapper and
  tqftpserv for those protocols. The FTS touch driver (patch 0085) was written from Google's `fts_touch`
  module (GPL-2.0) and the phone's device tree.
- **akshaynexus/iOSPlus10** (<https://github.com/akshaynexus/iOSPlus10>, MIT): `phone/wifi/bluetool_stub.c` is adapted
  from their `negroni-bluetool.c`; btbridge's H4 shim rules follow their bluetoothd findings (patches 0333/0336/0337).
- **DenialOfGravity/ios-redfin** (<https://github.com/DenialOfGravity/ios-redfin>, MIT, Pixel 5 port): btbridge's
  Command Disallowed / Unknown Command rewrites and bluetool-stub's wait for the bridge and btaddress echo follow
  their `phone/bt/btd.c` and `bluetool.c` (rules re-implemented, no code copied).
- **blacktop/darwin-xnu-build** (<https://github.com/blacktop/darwin-xnu-build>, MIT): the XNU build.
- Fetched and built by the user, not in git:
  - Google's factory image, AOSP `mkbootimg` and `avbtool` (Apache 2.0)
  - TWRP
  - dropbear (MIT), toybox (0BSD) and dash (BSD)
