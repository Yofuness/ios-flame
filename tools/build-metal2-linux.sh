#!/bin/bash
# Build the metal2 artifact (SweetMetal2.bundle = Turnip + metal2vulkan bridge, m2vtest, vktest, v2test) on Linux,
# same steps and order as .github/workflows/metal2.yml (which runs tools/build-turnip-ios.sh,
# tools/build-m2v-bridge.sh, phone/metal2/build.sh, phone/turnip/build.sh on macOS; this is their Linux twin).
# Out: work/metal2-linux/out/ (same layout as the CI artifact). Clones cached in work/metal2-linux/src; the Mesa and
# metal2vulkan trees are reset and re-patched only when a patch file changes. Build trees in work/metal2-linux/build.
# Needs clang, ld64.lld, llvm-ar/strip/nm/as, dsymutil, ldid, meson deps (python3, ninja, bison, flex),
# glslangValidator, ~/.cargo/bin/cargo with toolchain 1.98.0 + target aarch64-apple-ios; SDK: $IOS_SDK (tools/linux-ios-cc).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
W=$ROOT/work/metal2-linux
WF=$ROOT/.github/workflows/metal2.yml
MESA_COMMIT=$(sed -n 's/^ *MESA_COMMIT: *//p' "$WF")
UNIXNU_COMMIT=$(sed -n 's/^ *UNIXNU_COMMIT: *//p' "$WF")
M2V_COMMIT=$(sed -n 's/^ *M2V_COMMIT: *//p' "$WF")
SDK=${IOS_SDK:-$ROOT/research/sdk/theos-sdks/iPhoneOS16.5.sdk}
export IOS_SDK=$SDK
J=${J:-8}
CARGO=${CARGO:-$HOME/.cargo/bin/cargo}
SIGN=$ROOT/tools/linux-ios-sign
SRC=$W/src B=$W/build OUT=$W/out
mkdir -p "$SRC" "$B" "$W/bin"
rm -rf "$OUT"; mkdir -p "$OUT/phone/metal2/out" "$OUT/phone/turnip/out" "$OUT/work/turnip" "$OUT/work/m2v-bridge"

echo "== toolchain"
ln -sf "$ROOT/tools/linux-ios-cc" "$W/bin/ios-cc"
printf '#!/bin/sh\nIOS_CLANG=clang++ exec "%s" "$@"\n' "$ROOT/tools/linux-ios-cc" > "$W/bin/ios-c++"
chmod +x "$W/bin/ios-c++"
for t in ar ranlib nm strip; do ln -sf "$(command -v llvm-$t)" "$W/bin/aarch64-apple-darwin-$t"; done
export PATH="$W/bin:$(dirname "$CARGO"):$PATH"	# rustup's rustc, not a system one without the iOS target
CC=$W/bin/ios-cc
[ -x "$W/venv/bin/meson" ] || { python3 -m venv "$W/venv" && "$W/venv/bin/pip" install -q meson mako pyyaml packaging; }

echo "== Mesa source"
[ -d "$SRC/mesa/.git" ] || git clone -q --filter=blob:none --no-checkout https://gitlab.freedesktop.org/mesa/mesa.git "$SRC/mesa"
[ -d "$SRC/unixnu/.git" ] || git clone -q --filter=blob:none https://github.com/UniXNU/UniXNU.git "$SRC/unixnu"
git -C "$SRC/unixnu" checkout -q "$UNIXNU_COMMIT"
UPATCHES=$(python3 -c "import json,sys;print(' '.join(json.load(open(sys.argv[1]))['patches']))" \
	"$SRC/unixnu/scripts/data/adreno-compiler-inputs.json")
stamp=$( (echo "$MESA_COMMIT $UNIXNU_COMMIT"; for p in $UPATCHES; do cat "$SRC/unixnu/$p"; done
	cat "$ROOT"/phone/turnip/patches/*.patch) | sha256sum | cut -c1-16)
if [ "$(cat "$SRC/mesa.stamp" 2>/dev/null)" != "$stamp" ]; then
	git -C "$SRC/mesa" checkout -q -f "$MESA_COMMIT"
	git -C "$SRC/mesa" clean -fdq
	for p in $UPATCHES; do echo "apply $p"; git -C "$SRC/mesa" apply --whitespace=nowarn "$SRC/unixnu/$p"; done
	for p in "$ROOT"/phone/turnip/patches/*.patch; do echo "apply ${p#$ROOT/}"; git -C "$SRC/mesa" apply --whitespace=nowarn "$p"; done
	echo "$stamp" > "$SRC/mesa.stamp"
fi

echo "== Turnip"
T=$B/turnip
mkdir -p "$T"
flags="'-target', 'arm64-apple-ios16.0', '-isysroot', '$SDK', '-fstack-protector-strong'"
cat > "$T/ios16.cross" <<EOF
[binaries]
c = '$W/bin/ios-cc'
cpp = '$W/bin/ios-c++'
objc = '$W/bin/ios-cc'
ar = '$(command -v llvm-ar)'
strip = '$(command -v llvm-strip)'
pkg-config = '/usr/bin/false'
[host_machine]
system = 'darwin'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
[properties]
needs_exe_wrapper = true
[built-in options]
c_args = [$flags]
cpp_args = [$flags]
objc_args = [$flags]
c_link_args = [$flags]
cpp_link_args = [$flags]
objc_link_args = [$flags]
EOF
opts=("-Dtu-darwin-abi-dir=$ROOT/phone/turnip")	# array: the repo path has spaces
opts+=(-Dgallium-drivers= -Dvulkan-drivers=freedreno -Dfreedreno-kmds=darwin
 -Dplatforms=macos -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled
 -Dllvm=disabled -Dshared-glapi=disabled -Dxmlconfig=disabled -Dshader-cache=disabled -Dtools= -Dbuild-tests=false
 -Dvideo-codecs= -Dvulkan-layers= -Dlibunwind=disabled -Dlmsensors=disabled -Dvalgrind=disabled -Dexpat=disabled
 -Dzstd=disabled -Dbuildtype=release -Db_ndebug=true)
(
export PATH="$W/venv/bin:$PATH"
unset CFLAGS CXXFLAGS CPPFLAGS LDFLAGS CC CXX
if [ -f "$T/build-ios/build.ninja" ]; then
	meson configure "$T/build-ios" "${opts[@]}" >/dev/null
else
	meson setup "$T/build-ios" "$SRC/mesa" --wrap-mode=nodownload --cross-file "$T/ios16.cross" "${opts[@]}" \
		> "$T/configure-ios.log" 2>&1 || { tail -30 "$T/configure-ios.log"; exit 1; }
fi
ninja -j$J -C "$T/build-ios" src/freedreno/vulkan/libvulkan_freedreno.dylib > "$T/build-ios.log" 2>&1 ||
	{ grep -E "error:|FAILED" "$T/build-ios.log" | head -30; exit 1; }
)
TURNIP=$T/build-ios/src/freedreno/vulkan/libvulkan_freedreno.dylib
llvm-nm -gU "$TURNIP" | grep -q " T _vkGetInstanceProcAddr" || { echo "no vkGetInstanceProcAddr export" >&2; exit 1; }
llvm-nm -m "$TURNIP" | grep "weak external" || true
cp "$T"/*.log "$OUT/work/turnip/"

echo "== metal2vulkan bridge"
[ -d "$SRC/metal2vulkan/.git" ] || git clone -q https://github.com/steelbrain/metal2vulkan.git "$SRC/metal2vulkan"
stamp=$( (echo "$M2V_COMMIT"; cat "$ROOT"/phone/metal2/bridge/patches/*.patch) | sha256sum | cut -c1-16)
if [ "$(cat "$SRC/metal2vulkan.stamp" 2>/dev/null)" != "$stamp" ]; then
	git -C "$SRC/metal2vulkan" checkout -q -f "$M2V_COMMIT"
	git -C "$SRC/metal2vulkan" clean -fdq
	for p in "$ROOT"/phone/metal2/bridge/patches/*.patch; do echo "apply ${p#$ROOT/}"; git -C "$SRC/metal2vulkan" apply --whitespace=nowarn "$p"; done
	echo "$stamp" > "$SRC/metal2vulkan.stamp"
fi
M=$B/m2v-bridge
mkdir -p "$M"
llvm-as -o "$M/runtime_examples.air" "$SRC/metal2vulkan/crates/metal2vulkan-validation/fixtures/public/examples/runtime_examples.ll"
llvm-as -o "$M/function_constant.air" "$SRC/metal2vulkan/crates/metal2vulkan-validation/fixtures/public/kernel_function_constant_gate_fold.ll"
# Whole-program optimization of the bridge. Off (quicker rebuilds): M2V_LTO=off.
if [ "${M2V_LTO:-fat}" = off ]; then LTO=off CGU=16; else LTO=fat CGU=1; fi
cat > "$M/Cargo.toml" <<EOF
[package]
name = "native-metal-bridge"
version = "0.1.0"
edition = "2024"
rust-version = "1.98"
[workspace]
[lib]
crate-type = ["staticlib"]
path = "$ROOT/phone/metal2/bridge/NativeMetalBridge.rs"
[dependencies]
metal2vulkan = { path = "$SRC/metal2vulkan/crates/metal2vulkan", features = ["ash"] }
metal2vulkan-runtime = { path = "$SRC/metal2vulkan/crates/metal2vulkan-runtime" }
libc = "0.2"
sha2 = { version = "0.10", features = ["asm"] }
[profile.release]
panic = "unwind"
debug = 1
lto = "$LTO"
codegen-units = $CGU
EOF
# cc-rs (sha2 asm) and the linker: clang for iOS instead of xcrun
NMB_FIXTURE_AIR=$M/runtime_examples.air NMB_FC_AIR=$M/function_constant.air IPHONEOS_DEPLOYMENT_TARGET=16.0 \
	SDKROOT=$SDK CC_aarch64_apple_ios=$CC CXX_aarch64_apple_ios=$W/bin/ios-c++ AR_aarch64_apple_ios=llvm-ar \
	CARGO_TARGET_AARCH64_APPLE_IOS_LINKER=$CC \
	"$CARGO" +1.98.0 build -j$J --release --manifest-path "$M/Cargo.toml" --target aarch64-apple-ios \
	--target-dir "$M/target" > "$M/build-ios.log" 2>&1 || { grep -E "^(error|warning: unused)" -A6 "$M/build-ios.log" | head -60; exit 1; }
BRIDGE=$M/target/aarch64-apple-ios/release/libnative_metal_bridge.a
cp "$M"/*.log "$OUT/work/m2v-bridge/"

echo "== SweetMetal2.bundle"
P=$ROOT/phone/metal2
MO=$OUT/phone/metal2/out
BU=$MO/SweetMetal2.bundle
mkdir -p "$BU"
"$W/bin/ios-c++" -x objective-c++ -std=c++17 -fobjc-arc -O2 -g -Wall -Wno-deprecated-declarations -Wno-unused-function -bundle \
	-o "$BU/SweetMetal2" "$P/SweetMetal2.mm" "$P/NativeBridgeMetalFrontend.mm" -x none "$BRIDGE" -Wl,-dead_strip -lc++ \
	-framework Foundation -framework Metal -framework IOSurface -framework IOKit -framework CoreFoundation 2>&1 |
	grep -v "was built for newer 'iOS' version" || true
[ -f "$BU/SweetMetal2" ] || exit 1
# clang -g runs dsymutil itself when it links for a Darwin target
mv "$BU/SweetMetal2.dSYM" "$MO/" 2>/dev/null || dsymutil "$BU/SweetMetal2" -o "$MO/SweetMetal2.dSYM" 2>/dev/null || true
llvm-strip -x "$BU/SweetMetal2"
cp "$P/Info.plist" "$BU/Info.plist"
cp "$TURNIP" "$BU/libvulkan_freedreno.dylib"
llvm-strip -x "$BU/libvulkan_freedreno.dylib"
"$SIGN" "$BU/libvulkan_freedreno.dylib"
"$SIGN" "$BU"	# ldid signs the main executable and writes _CodeSignature/CodeResources
"$CC" -O2 -Wall -o "$MO/m2vtest" "$P/m2vtest.c" "$BRIDGE" -Wl,-dead_strip 2>&1 | grep -v "was built for newer" || true
llvm-strip -x "$MO/m2vtest" && "$SIGN" "$MO/m2vtest"
llvm-nm -m "$BU/SweetMetal2" | grep "weak external" | grep -v "dynamically looked up" || true
cp "$TURNIP" "$MO/"

echo "== Turnip tests (vktest, v2test)"
TP=$ROOT/phone/turnip
TO=$OUT/phone/turnip/out
: > "$TO/shaders.h"
for s in fill.comp:fill_comp tri.vert:tri_vert tri.frag:tri_frag; do
	glslangValidator -V --target-env vulkan1.2 --vn "${s#*:}" -o "$TO/${s#*:}.h" "$TP/shaders/${s%%:*}" >/dev/null
	cat "$TO/${s#*:}.h" >> "$TO/shaders.h"
done
"$CC" -O2 -Wall -I "$TO" -I "$SRC/mesa/include" -o "$TO/vktest" "$TP/vktest.c"
"$SIGN" "$TO/vktest"
cp "$TURNIP" "$TO/"
"$SIGN" "$TO/libvulkan_freedreno.dylib"
"$W/bin/ios-c++" -x c++ -std=c++17 -O2 -I "$TP" -o "$TO/v2test" "$TP/v2test.cpp" -lc++ -framework IOKit -framework CoreFoundation || true
[ -f "$TO/v2test" ] && "$SIGN" "$TO/v2test" || true
ls -la "$BU" "$MO" "$TO"
