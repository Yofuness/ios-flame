# macOS build VM (kernel builds without GitHub runners)

A macOS 14 (Sonoma) VM under QEMU/KVM on the Linux PC, with Xcode 15.0.1 (the same as the CI runner), used by
`tools/vm-build-xnu.sh`. The scripts live here; the VM's data lives outside git in `$MACOS_VM_DIR`
(default `~/VMs/macos`): the disk `mac_hdd.qcow2`, `OVMF_VARS.fd`, the saved state `state.zst`, the SSH key
(`id_ed25519`, `known_hosts`) and `credentials` (mode 600: the `builder` login password and the VNC password).
`~/VMs/macos` holds symlinks to these scripts, and `~/VMs/OSX-KVM` (kholia/OSX-KVM) gives OpenCore, OVMF and the
recovery image.

| Script | What it does |
|---|---|
| `mac-vm.sh [install\|resume]` | QEMU: 8 vCPUs as 8 cores x 1 thread, 8 GB, Skylake-Client with `tsc-frequency=2496000000` (invtsc blocks migration otherwise), VNC 0.0.0.0:5901 with a password, SSH on localhost:2222, QMP/HMP sockets, `qemu.pid` |
| `vm-start.sh` | resume from `state.zst` if present (5-15 s), else boot and pick Macintosh HD in the OpenCore picker (~30 s); sets the VNC password, pins the vCPUs, sets the guest clock |
| `vm-stop.sh [--shutdown]` | hibernate (QEMU migrates RAM + devices to `state.zst`, 5-11 s), or a real shutdown |
| `vmstate.py save\|wait` | the hibernate / resume steps over QMP |
| `vm-pin.py` | vCPU threads 1:1 on the host E-cores 4-11, other QEMU threads on the P-cores 0-3 |
| `vssh CMD` | SSH into the VM as `builder` (passwordless sudo) |
| `vmctl.py shot\|click\|key\|type\|typepw` | screenshots and input over QMP (for the GUI-only steps) |

Setup from scratch (2026-10-28, about 2 h, mostly the macOS download): clone kholia/OSX-KVM to `~/VMs/OSX-KVM`,
`fetch-macOS-v2.py -s sonoma`, `qemu-img convert BaseSystem.dmg -O raw BaseSystem.img`,
`qemu-img create -f qcow2 mac_hdd.qcow2 128G`, copy `OVMF_VARS-1920x1080.fd` to `OVMF_VARS.fd`, symlink these
scripts into `$MACOS_VM_DIR`, `mac-vm.sh install`, then in Recovery: Terminal `diskutil eraseDisk APFS
'Macintosh HD' GPT disk2`, Reinstall macOS, the setup assistant (local account `builder`, no Apple ID), Remote
Login + passwordless sudo, Xcode CLT, jq/gum/ninja/cmake/bash 5.2 into /usr/local (Homebrew no longer supports
Intel), and Xcode 15.0.1 from developer.apple.com (needs an Apple ID) into `/Applications/Xcode_15.0.1.app`.

Apple's licence only allows macOS on Apple hardware; the user chose to run it here.
