#!/bin/bash
# Build the sunfish xnu kernel in the local macOS VM (~/VMs/macos) instead of GitHub CI.
# Mirrors .github/workflows/xnu.yml: blacktop/darwin-xnu-build @ DXB_SHA, xnu/patches, ARCHS sed, xcodes stub,
# Xcode 15.0.1, MAKEFLAGS=-k. The VM keeps ~/xnubuild/dxb between runs (instead of actions/cache): after a good
# build the next run re-patches a pristine copy and rsyncs only changed files, so make rebuilds just those.
# Usage: tools/vm-build-xnu.sh NN [--clean] [--stop]   -> work/kernelNN/ (same layout as gh run download -n xnu-kernel)
set -euo pipefail
NN=${1:?usage: $0 NN [--clean] [--stop]}; shift
CLEAN=0; STOP=0
for a in "$@"; do case $a in --clean) CLEAN=1;; --stop) STOP=1;; *) echo "unknown arg $a" >&2; exit 2;; esac; done
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

vs "CLEAN=$CLEAN JOBS='$JOBS' /usr/local/bin/bash -s" <<'REMOTE' || FAIL=1
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
INC=0
[ "$CLEAN" = 0 ] && [ -f dxb/.vm-ok ] && INC=1
if [ $INC = 0 ]; then
  rm -rf dxb; git init -q dxb
  git -C dxb fetch -q --depth 1 "$DXB_REPO" "$DXB_SHA"
  git -C dxb checkout -q FETCH_HEAD
fi
cd dxb
rm -f .vm-ok
# pristine build.sh and patches dir, then ours on top (as CI's fresh checkout)
git checkout -q -- build.sh patches; git clean -fdq patches
cp ~/xnubuild/patches-in/*.patch ~/xnubuild/patches-in/*.sh patches/
perl -pi -e 's/ARCHS="x86_64 arm64e" VALID_ARCHS="x86_64 arm64e"/ARCHS="x86_64 arm64e arm64" VALID_ARCHS="x86_64 arm64e arm64"/' build.sh
grep -q 'VALID_ARCHS="x86_64 arm64e arm64"' build.sh
# build.sh hardcodes make -j8 (= the VM vCPUs); VM_JOBS=N lowers it if 8 GB swaps
[ -n "$JOBS" ] && perl -pi -e "s/make install -j8/make install -j$JOBS/" build.sh
if [ $INC = 1 ]; then
  echo "incremental: re-patching cached source"
  P=~/xnubuild/prep; rm -rf "$P" && mkdir -p "$P"
  cp -a xnu "$P/xnu"
  (cd "$P/xnu" && git checkout -q -- . && git clean -fdqx)
  ln -s "$PWD/patches" "$P/patches"
  (
    WORK_DIR=$P CODEQL=0 KDKROOT=${KDKROOT:-/nonexistent}
    running() { echo "$*"; }; warning() { echo "$*"; }; error() { echo "$*" >&2; }
    eval "$(sed -n '/^patches() {/,/^}/p' build.sh)"
    patches
  )
  rsync -rlc --no-times --delete --exclude .git --itemize-changes "$P/xnu/" xnu/ | grep -v '^\.' | head -50 || true
  perl -pi -e 's/^    patches$/    true # patched by the cache step/' build.sh
  rm -rf build/xnu.obj/kernel.* fakeroot/System/Library/Kernels/kernel.*
else
  echo "full build"
fi
if ! bash build.sh > "$LOG" 2>&1; then
  echo "BUILD FAILED; errors:"; grep -E 'error:|\*\*\*' "$LOG" | head -15; echo "--- tail $LOG"; tail -30 "$LOG"; exit 1
fi
rm -rf ~/xnubuild/out && mkdir -p ~/xnubuild/out
cp -R fakeroot/System/Library/Kernels/. ~/xnubuild/out/
(cd ~/xnubuild/out && shasum -a 256 kernel.* > SHA256SUMS 2>/dev/null) || true  # kernel.*.link is a dir, as in CI
ls ~/xnubuild/out/kernel.development.sweet >/dev/null
touch .vm-ok
REMOTE

if [ "${FAIL:-0}" = 1 ]; then echo "build failed after $(( $(date +%s) - T0 )) s" >&2; exit 1; fi
mkdir -p "$OUT"
vs 'tar -C ~/xnubuild/out -cf - .' </dev/null | tar -C "$OUT" -xf -
(cd "$OUT" && sha256sum -c SHA256SUMS)
ls -la "$OUT"
echo "wall time: $(( $(date +%s) - T0 )) s"
[ $STOP = 1 ] && "$VM/vm-stop.sh"
exit 0
