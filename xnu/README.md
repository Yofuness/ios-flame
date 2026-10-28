# XNU for sweet and flame

`patches/` is one series on xnu-8796.141.3 (the iOS 16 generation):

- `0001`-`0081`: ios-sweet (Redmi Note 10 Pro). History in [docs/archive/sweet/NOTES.md](../docs/archive/sweet/NOTES.md).
- `0082`-`0124`: flame (Pixel 4). One line per patch in [NOTES.md](../docs/archive/flame/NOTES.md#flame-patches), the details in
  [HANDOFF.md](../docs/archive/flame/HANDOFF.md). 0090, 0092-0093 and 0101 have no file of their own: they are inside 0089, 0091 and
  0100. 0082 is needed with Xcode 16 or newer (iig emits `IORPC.kernelContent`).

The flame series applies with `git apply` in order on top of the ios-sweet baseline and gives the tree that built
kernel 0124 (checked 2026-10-22).

## Local build (flame)

1. Run [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build) (MIT) once with
   `MACOS_VERSION=13.5`, `MACHINE_CONFIG=SWEET`, `KERNEL_CONFIG=DEVELOPMENT` and these patches in its patch dir
   (the same steps as `.github/workflows/xnu.yml`), checked out at `work/dxb`.
2. After that, `tools/build-xnu.sh /abs/path/build.log` rebuilds incrementally in a few minutes with the installed
   Xcode -> `work/dxb/build/xnu.obj/DEVELOPMENT_ARM64_SWEET/kernel.development.sweet`.
3. A new patch: commit the change in `work/dxb/xnu` (a git repo whose HEAD is baseline + applied patches), then
   `git diff HEAD~1 > xnu/patches/NNNN-flame-NAME.patch`.

## CI (ios-sweet)

Built on GitHub's macOS arm64 runners by `.github/workflows/xnu.yml`, using blacktop/darwin-xnu-build at a pinned
commit.

- `MACOS_VERSION=13.5` -> xnu-8796 (same generation as iOS 16.6/16.7, the last iOS for iPhone 8).
- `patches/*.patch` are copied into the build script's patch dir and applied to the xnu tree with `git apply`.
- Output: `kernel.<config>.vmapple` (later: our own SWEET machine config) as a workflow artifact.

Fetch the latest ios-sweet build: `gh run download -R dipa-studio/ios-sweet -n xnu-kernel -D xnu/out`. The
flame patches are built locally; the workflow has not been run with them.
