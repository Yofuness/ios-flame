#!/bin/bash
# Build the ios-userland artifact on Linux, same steps and order as .github/workflows/userland.yml.
# Out: work/userland-linux/out/ (like the CI artifact). Downloads cached in work/userland-linux/src.
# Needs clang, ld64.lld, llvm-ar/ranlib/nm, ldid; SDK from $IOS_SDK (see tools/linux-ios-cc).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W=$ROOT/work/userland-linux
DROPBEAR_VER=$(sed -n 's/^ *DROPBEAR_VER: "\(.*\)"/\1/p' "$ROOT/.github/workflows/userland.yml")
DASH_VER=$(sed -n 's/^ *DASH_VER: "\(.*\)"/\1/p' "$ROOT/.github/workflows/userland.yml")
TOYBOX_VER=$(sed -n 's/^ *TOYBOX_VER: "\(.*\)"/\1/p' "$ROOT/.github/workflows/userland.yml")
J=${J:-8}
mkdir -p "$W/src" "$W/bin"
rm -rf "$W/out" "$W/build"; mkdir -p "$W/out" "$W/build"
ln -sf "$ROOT/tools/linux-ios-cc" "$W/bin/ios-cc"
for t in ar ranlib nm strip; do ln -sf "$(command -v llvm-$t)" "$W/bin/aarch64-apple-darwin-$t"; done
export PATH="$W/bin:$PATH"
OUT=$W/out
fetch() {	# URL -> cached file path
	local f=$W/src/${1##*/}
	[ -s "$f" ] || curl -fsSL -o "$f" "$1"
	echo "$f"
}
cd "$W/build"

echo "== dropbear"
tar xjf "$(fetch https://matt.ucc.asn.au/dropbear/releases/dropbear-$DROPBEAR_VER.tar.bz2)"
( cd dropbear-$DROPBEAR_VER
cat > localoptions.h <<'EOT'
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_SVR_PAM_AUTH 0
#define DROPBEAR_SVR_PUBKEY_AUTH 1
#define DROPBEAR_SVR_MULTIUSER 1
#define DROPBEAR_X11FWD 0
#define DEFAULT_PATH "/var/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
#define DEFAULT_ROOT_PATH "/var/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
#define DSS_PRIV_FILENAME "/private/etc/dropbear/dropbear_dss_host_key"
#define RSA_PRIV_FILENAME "/private/etc/dropbear/dropbear_rsa_host_key"
#define ECDSA_PRIV_FILENAME "/private/etc/dropbear/dropbear_ecdsa_host_key"
#define ED25519_PRIV_FILENAME "/private/etc/dropbear/dropbear_ed25519_host_key"
EOT
./configure --host=aarch64-apple-darwin CC=ios-cc --disable-zlib --disable-syslog \
	--disable-lastlog --disable-utmp --disable-utmpx --disable-wtmp --disable-wtmpx \
	--disable-loginfunc --disable-pututline --disable-pututxline --disable-harden >configure.log
make -j$J PROGRAMS="dropbear dropbearkey scp dbclient" MULTI=1 >make.log
cp dropbearmulti "$OUT/" )

echo "== dash"
tar xzf "$(fetch http://gondor.apana.org.au/~herbert/dash/files/dash-$DASH_VER.tar.gz)"
( cd dash-$DASH_VER
# iOS 16 lacks strchrnul (newer SDKs weak-import it -> PC 0 crash)
./configure --host=aarch64-apple-darwin CC=ios-cc CC_FOR_BUILD=cc ac_cv_func_strchrnul=no >configure.log
make -j$J >make.log
cp src/dash "$OUT/" )

echo "== toybox (failure allowed, like CI)"
tar xzf "$(fetch https://landley.net/toybox/downloads/toybox-$TOYBOX_VER.tar.gz)"
( cd toybox-$TOYBOX_VER
# iOS SDK has no sys/disk.h (macOS block-device ioctls used by portability.c)
mkdir -p ios-inc/sys
printf '#include <sys/ioctl.h>\n#include <stdint.h>\n#define DKIOCGETBLOCKSIZE _IOR(%s, 24, uint32_t)\n#define DKIOCGETBLOCKCOUNT _IOR(%s, 25, uint64_t)\n' "'d'" "'d'" > ios-inc/sys/disk.h
make macos_defconfig HOSTCC=cc >make.log
# Not on Darwin: toybox would pick GNU ld flags and strip options
CC=ios-cc HOSTCC=cc CFLAGS="-Iios-inc -Wno-deprecated-declarations" LDOPTIMIZE=-Wl,-dead_strip STRIP=llvm-strip make -j$J toybox >>make.log 2>&1
cp toybox "$OUT/" ) || echo "WARNING: toybox failed (see $W/build/toybox-$TOYBOX_VER/make.log)"

echo "== wland + phone tools"
( cd "$ROOT"
sh phone/wifi/build.sh
cp phone/wifi/out/wland phone/wifi/out/adspboot "$OUT/"
bash phone/tools/build.sh || echo "WARNING: some phone tools failed"
# every Mach-O phone/tools/build.sh made (a fixed list missed new tools such as fcopy)
for f in phone/tools/out/*; do
	[ -f "$f" ] && [ -x "$f" ] && file -b "$f" | grep -q Mach-O && cp "$f" "$OUT/"
done; true )

echo "== check and sign"
cd "$OUT"
for b in *; do
	if llvm-nm -m "$b" | grep -q "weak external"; then
		echo "WARNING: $b has weak imports:"; llvm-nm -m "$b" | grep "weak external"
	fi
	case $b in dash|dropbearmulti|toybox) "$ROOT/tools/linux-ios-sign" "$b";; esac  # others keep their entitlements
	file "$b"
done
sha256sum * > SHA256SUMS
