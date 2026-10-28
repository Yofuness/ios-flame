#!/bin/bash
# Build the sunfish xnu kernel in the local macOS VM (~/VMs/macos) instead of GitHub CI.
# Mirrors .github/workflows/xnu.yml: blacktop/darwin-xnu-build @ DXB_SHA, xnu/patches, ARCHS sed, xcodes stub,
# Xcode 15.0.1, MAKEFLAGS=-k. The VM keeps ~/xnubuild/dxb between runs (instead of actions/cache): after a good
# build the next run compares the patch hashes with ~/xnubuild/dxb/.vm-patchsums: unchanged -> straight to make;
# only new patches appended -> git apply just those to dxb/xnu; otherwise re-patch the persistent prep/xnu (reset in
# place) and rsync only changed files, so make rebuilds just those.
# dSYM and CTF are skipped by default (BUILD_DSYM=0 DO_CTFMERGE=0 on the kernel make line; saves ~14 s); --full
# builds them as CI does. The VM is left running: back-to-back builds reuse it, pass --stop (or run
# ~/VMs/macos/vm-stop.sh) at the end of a session.
# Usage: tools/vm-build-xnu.sh NN [--clean] [--full] [--stop]   -> work/kernelNN/ (same layout as gh run download -n xnu-kernel)
set -euo pipefail
NN=${1:?usage: $0 NN [--clean] [--full] [--stop]}; shift
CLEAN=0; STOP=0; FULL=0
for a in "$@"; do case $a in --clean) CLEAN=1;; --full) FULL=1;; --stop) STOP=1;; *) echo "unknown arg $a" >&2; exit 2;; esac; done
PORT=$(cd "$(dirname "$0")/.." && pwd)
VM=${VM_DIR:-$HOME/VMs/macos}
OUT=$PORT/work/kernel$NN
JOBS=${VM_JOBS:-}   # empty = build.sh default; set e.g. VM_JOBS=6 if the 8 GB VM swaps
T0=$(date +%s)
vs() { "$VM/vssh" -o ConnectTimeout=5 "$@"; }

if ! vs true </dev/null 2>/dev/null; then
  "$VM/vm-start.sh"
  for i in $(seq 60); do vs true </dev/null 2>/dev/null && break; sleep 5; done
  vs true </dev/null || { echo "VM ssh not answering (OpenCore picker? vmctl.py key right; key ctrl+ret)" >&2; exit 1; }
fi

# patches in, fresh each run (removed patches must not linger)
vs 'rm -rf ~/xnubuild/patches-in && mkdir -p ~/xnubuild/patches-in' </dev/null
tar -C "$PORT/xnu/patches" -cf - . | vs 'tar -C ~/xnubuild/patches-in -xf -'

vs "CLEAN=$CLEAN FULL=$FULL JOBS='$JOBS' /usr/local/bin/bash -s" <<'REMOTE' || FAIL=1
set -euo pipefail
export PATH=/usr/local/bin:$PATH
DXB_REPO=https://github.com/blacktop/darwin-xnu-build
DXB_SHA=663906528d1cd5ea8fde7de6f16de73db9511549
export MACOS_VERSION=13.5 KERNEL_CONFIG=DEVELOPMENT MACHINE_CONFIG=SWEET MAKEFLAGS=-k
XC=/Applications/Xcode_15.0.1.app
export DEVELOPER_DIR=$XC/Contents/Developer
[ "$(xcode-select -p)" = "$DEVELOPER_DIR" ] || sudo xcode-select -s "$XC"
[ -x /usr/local/bin/xcodes ] || { printf '#!/bin/sh\nexit 0\n' | sudo tee /usr/local/bin/xcodes >/dev/null; sudo chmod +x /usr/local/bin/xcodes; }
cd ~/xnubuild
LOG=~/xnubuild/build.log
: > "$LOG"
stamp() { echo "VMSTAMP $1 $(date +%s)" >> "$LOG"; }
stamp start
INC=0
[ "$CLEAN" = 0 ] && [ -f dxb/.vm-ok ] && INC=1
if [ $INC = 0 ]; then
  rm -rf dxb; git init -q dxb
  git -C dxb fetch -q --depth 1 "$DXB_REPO" "$DXB_SHA"
  git -C dxb checkout -q FETCH_HEAD
fi
cd dxb
rm -f .vm-ok
# (name, hash) of every patch/script, scripts first; compared with the last good build's list
NEWSUMS=$( (cd ~/xnubuild/patches-in && { ls *.sh 2>/dev/null; ls *.patch; } | while read -r f; do echo "$f $(shasum -a 256 "$f" | cut -c1-64)"; done) )
OLDSUMS=$(cat .vm-patchsums 2>/dev/null || true); rm -f .vm-patchsums
MODE=full
if [ $INC = 1 ] && [ -n "$OLDSUMS" ]; then
  if [ "$OLDSUMS" = "$NEWSUMS" ]; then MODE=same
  elif [ "$(printf '%s\n' "$NEWSUMS" | head -n "$(printf '%s\n' "$OLDSUMS" | wc -l)")" = "$OLDSUMS" ]; then MODE=append
  else MODE=repatch; fi
elif [ $INC = 1 ]; then MODE=repatch; fi
# pristine build.sh and patches dir, then ours on top (as CI's fresh checkout)
git checkout -q -- build.sh patches; git clean -fdq patches
cp ~/xnubuild/patches-in/*.patch ~/xnubuild/patches-in/*.sh patches/
perl -pi -e 's/ARCHS="x86_64 arm64e" VALID_ARCHS="x86_64 arm64e"/ARCHS="x86_64 arm64e arm64" VALID_ARCHS="x86_64 arm64e arm64"/' build.sh
grep -q 'VALID_ARCHS="x86_64 arm64e arm64"' build.sh
# build.sh hardcodes make -j8 (= the VM vCPUs); VM_JOBS=N lowers it if 8 GB swaps
[ -n "$JOBS" ] && perl -pi -e "s/make install -j8/make install -j$JOBS/" build.sh
if [ $MODE = same ]; then
  echo "patch set unchanged: make only"
elif [ $MODE = append ]; then
  echo "patches appended: applying only the new ones to dxb/xnu"
  (cd xnu && printf '%s\n' "$NEWSUMS" | tail -n +"$(( $(printf '%s\n' "$OLDSUMS" | wc -l) + 1 ))" | while read -r f h; do
    case $f in *.patch) ;; *) echo "new script $f: not supported on the append path" >&2; exit 1;; esac
    git apply --check ~/xnubuild/patches-in/"$f" && git apply ~/xnubuild/patches-in/"$f" && echo "  applied $f"
  done)
elif [ $MODE = repatch ]; then
  echo "patch set changed: re-patching the persistent prep/xnu"
  P=~/xnubuild/prep; mkdir -p "$P"; rm -f "$P/patches"
  [ -d "$P/xnu/.git" ] || { rm -rf "$P/xnu"; cp -a xnu "$P/xnu"; }
  (cd "$P/xnu" && git checkout -q -- . && git clean -fdqx)
  ln -s "$PWD/patches" "$P/patches"
  (
    WORK_DIR=$P CODEQL=0 KDKROOT=${KDKROOT:-/nonexistent}
    running() { echo "$*"; }; warning() { echo "$*"; }; error() { echo "$*" >&2; }
    eval "$(sed -n '/^patches() {/,/^}/p' build.sh)"
    patches
  )
  rsync -rlc --no-times --delete --exclude .git --itemize-changes "$P/xnu/" xnu/ | grep -v '^\.' | head -50 || true
fi
if [ $INC = 1 ]; then
  perl -pi -e 's/^    patches$/    true # patched by the cache step/' build.sh
  rm -rf build/xnu.obj/kernel.* fakeroot/System/Library/Kernels/kernel.*
else
  echo "full build"
fi
echo "vm-build: patch path $MODE" | tee -a "$LOG"
# no dSYM/CTF unless --full (command-line make vars beat MakeInc.def's :=)
[ "$FULL" = 1 ] || perl -pi -e 's/("\$\{CTF_TOOL_OVERRIDES\[@\]\}") SDKROOT=macosx TARGET_CONFIGS/$1 BUILD_DSYM=0 DO_CTFMERGE=0 SDKROOT=macosx TARGET_CONFIGS/' build.sh
[ "$FULL" = 1 ] || grep -q 'BUILD_DSYM=0 DO_CTFMERGE=0' build.sh
stamp patched
if ! bash build.sh >> "$LOG" 2>&1; then
  stamp built; echo "BUILD FAILED; errors:"; grep -E 'error:|\*\*\*' "$LOG" | head -15; echo "--- tail $LOG"; tail -30 "$LOG"; exit 1
fi
stamp built
rm -rf ~/xnubuild/out && mkdir -p ~/xnubuild/out
cp -R fakeroot/System/Library/Kernels/. ~/xnubuild/out/
(cd ~/xnubuild/out && shasum -a 256 kernel.* > SHA256SUMS 2>/dev/null) || true  # kernel.*.link is a dir, as in CI
ls ~/xnubuild/out/kernel.development.sweet >/dev/null
printf '%s\n' "$NEWSUMS" > .vm-patchsums
touch .vm-ok
stamp done
awk '/^VMSTAMP/{t[$2]=$3} END{printf "vm-build: patch %ds, make %ds, post %ds, total %ds\n", t["patched"]-t["start"], t["built"]-t["patched"], t["done"]-t["built"], t["done"]-t["start"]}' "$LOG"
REMOTE

if [ "${FAIL:-0}" = 1 ]; then echo "build failed after $(( $(date +%s) - T0 )) s" >&2; exit 1; fi
mkdir -p "$OUT"
vs 'tar -C ~/xnubuild/out -cf - .' </dev/null | tar -C "$OUT" -xf -
(cd "$OUT" && sha256sum -c SHA256SUMS)
ls -la "$OUT"
echo "wall time (with upload/download): $(( $(date +%s) - T0 )) s"
[ $STOP = 1 ] && "$VM/vm-stop.sh"
exit 0
