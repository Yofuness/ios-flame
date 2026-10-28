#!/bin/bash
# macOS (Sonoma) VM for xnu kernel builds, after kholia/OSX-KVM's OpenCore-Boot.sh.
#   mac-vm.sh [install|resume]   install = also attach the recovery image (BaseSystem.img);
#                                resume = restore the RAM state saved by vm-stop.sh (state.zst) instead of booting
# 8 vCPUs (8 cores, 1 thread; pinned to host E-cores 4-11 by vm-pin.py), 8 GB RAM; screen on VNC 0.0.0.0:5901 (password in credentials); QMP at ./qmp.sock, HMP at ./mon.sock; SSH on localhost:2222.
# Start it only for builds and shut it down after (the PC has 16 GB).
set -e
# VM data (disk, state, keys, credentials) lives outside git, in $MACOS_VM_DIR
D=${MACOS_VM_DIR:-$HOME/VMs/macos}
K=$D/../OSX-KVM
inst=()
[ "${1:-}" = resume ] && inst=(-incoming "exec:zstd -dcq $D/state.zst")
[ "${1:-}" = install ] && inst=(-device ide-hd,bus=sata.3,drive=InstallMedia -drive id=InstallMedia,if=none,file="$K/BaseSystem.img",format=raw)
exec qemu-system-x86_64 -name macos-build -enable-kvm -m 8192 \
  -cpu Skylake-Client,-hle,-rtm,kvm=on,vendor=GenuineIntel,+invtsc,tsc-frequency=2496000000,vmware-cpuid-freq=on,+ssse3,+sse4.2,+popcnt,+avx,+aes,+xsave,+xsaveopt,check \
  -machine q35 -smp 8,cores=8,threads=1,sockets=1 \
  -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0 \
  -device isa-applesmc,osk="ourhardworkbythesewordsguardedpleasedontsteal(c)AppleComputerInc" \
  -drive if=pflash,format=raw,readonly=on,file="$K/OVMF_CODE_4M.fd" \
  -drive if=pflash,format=raw,file="$D/OVMF_VARS.fd" \
  -smbios type=2 -device ich9-ahci,id=sata \
  -drive id=OpenCoreBoot,if=none,snapshot=on,format=qcow2,file="$K/OpenCore/OpenCore.qcow2" \
  -device ide-hd,bus=sata.2,drive=OpenCoreBoot "${inst[@]}" \
  -drive id=MacHDD,if=none,file="$D/mac_hdd.qcow2",format=qcow2,discard=unmap \
  -device ide-hd,bus=sata.4,drive=MacHDD \
  -netdev user,id=net0,hostfwd=tcp:127.0.0.1:2222-:22 -device virtio-net-pci,netdev=net0,id=net0,mac=52:54:00:c9:18:27 \
  -device vmware-svga -display none -vnc 0.0.0.0:1,password=on \
  -pidfile "$D/qemu.pid" -qmp unix:"$D/qmp.sock",server,nowait -monitor unix:"$D/mon.sock",server,nowait
