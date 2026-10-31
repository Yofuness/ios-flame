#!/bin/bash
# Host tests of the bridge's allocator (0116, phone/metal2/bridge/NativeMetalAlloc.rs), on and off.
# Serial: some tests look at this thread's free lists and the shared slot pool.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$ROOT/work/metal2-alloc-test
mkdir -p "$T"
cat > "$T/Cargo.toml" <<TOML
[package]
name = "native-metal-alloc"
version = "0.1.0"
edition = "2024"
[workspace]
[lib]
path = "$ROOT/phone/metal2/bridge/NativeMetalAlloc.rs"
[dependencies]
libc = "0.2"
TOML
CARGO=${CARGO:-$HOME/.cargo/bin/cargo}
for profile in "" --release; do
	"$CARGO" +1.98.0 test -q $profile --manifest-path "$T/Cargo.toml" -- --test-threads=1
	M2V_FAST_ALLOC=0 "$CARGO" +1.98.0 test -q $profile --manifest-path "$T/Cargo.toml" -- --test-threads=1
done
