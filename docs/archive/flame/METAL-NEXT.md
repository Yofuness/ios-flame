# Metal: where it stands and what comes next (investigation, 2026-10-21 night)

Read-only investigation, written while another session works on WiFi. Nothing in the repo, the kernel tree
or the phone was changed. Scratch material: metal2vulkan clone in the Claude scratchpad
(`.../scratchpad/dl/m2v`, commit a2c440b; UniXNU pins 720ef1ce, 66 commits older).

## TL;DR

The current SweetMetal design draws CA's frames well, but it cannot reach "works like a phone" on its own:
**every pipeline is compiled on the Mac** (AIR -> GLSL -> turnip capture in the Lima VM -> template JSON
pushed to the phone). So a pipeline the host has not seen draws nothing. On top of that, the encoder has
no depth/stencil, no MRT, no compute, no culling/winding/depth bias, no vertex textures, and no fences or
heaps. Third-party apps, MapKit (VectorKit), RenderBox widgets, CoreImage/Photos and WebKit's GPU process
all need those.

UniXNU (sheng, Adreno 740) has already solved the same problem with a stack that fits us:
**Metal frontend (ObjC) -> metal2vulkan runtime (Rust: AIR -> SPIR-V, full Metal API on Vulkan) ->
Mesa Turnip built for iOS with a "darwin" kernel backend -> kernel user client.** Every shader is compiled
on the device, and a pipeline cache persists across boots. Mesa supports the A640 directly
(`GPUId(640)`, a6xx gen2). What we would have to write is mostly the kernel ABI they expect, plus the
iOS 16 differences.

**Recommendation:** port that stack (plan below). Keep the template path working until the port draws the
home screen, then delete it. Do two small kernel fixes first; they help both paths (section "Phase 0").

## 1. What exists (code facts)

- `phone/metal/SweetMetal.m` (2072 lines): device (subclass of `_MTLDevice`), queue/command buffer
  (subclasses of `_MTLCommandQueue`/`_MTLCommandBuffer`), textures (BO, IOSurface import, shadow copies
  for unaligned pitches), buffers (BOs; `gpuAddress` = GPU VA, so CA's embedded pointers work), render
  encoder (one colour attachment; CA's memoryless att1 = one shared scratch, `scratchTargetVA:`), blit
  encoder **on the CPU** (it submits earlier GPU work synchronously first), library wrapper (function
  constants ride on a `SweetFnProxy`; Metal's own library has no compiler backend).
- `phone/metal/a6draw.m` (785): `SweetPipelineTemplate` (static and dynamic draw-state groups with named
  patch points), `SweetCmdStream` (pass prologue/epilogue from `pipelines/pass.json`, per draw: bindless
  descriptor sets 0-3 in scratch, `CP_SET_DRAW_STATE`, `CP_DRAW_INDX_OFFSET`), async submit + `retire:`.
- 410 templates in `phone/metal/pipelines`, keyed by sha1(pipeline key)[:12].
- Kernel `iokit/Kernel/arm/SweetAccelerator.cpp`: IOAcceleratorES + user client with scalar selectors
  0 info, 1 alloc (kernel picks the VA), 2 free, 3 IOSurface import by ID, 4 sync submit, 5 cache flush,
  6 async submit, 7 wait fence. **One GPU address space shared by every process**
  (`sweet_gpu.c` user_va_alloc 0x2_0000_0000..0x7f_0000_0000; ~2300 4K page tables, so about 4.6 GB can be
  mapped at once). CPU mappings are **cacheable**, so userland cleans caches before every submit.
- Kernel submit (`sweet_a6xx.c:742`): `CP_INDIRECT_BUFFER, CP_WAIT_FOR_IDLE, CP_MEM_WRITE fence`.
  There is no cache flush event with the fence and no CCU invalidate at the start (msm uses
  `CP_EVENT_WRITE CACHE_FLUSH_TS` as the fence write). SweetMetal's templates flush in their own epilogue,
  but Turnip expects the kernel to do it.
- hw_init already matches what Turnip needs: GMEM range 0x100000-0x1fffff (1 MiB), UCHE trap base
  0x1fffffffff000, UBWC highest bank bit 15 (`sweet_a6xx.c:960-978`). Mesa's table has `GPUId(640)`.

## 2. Gaps seen in practice (logs/runs/yt3, mtl-0113)

- MISSING/"no signature" selectors: `setDepthStencilState:` (chronod/RenderBox, MapKit, PridePoster),
  `setStencilReferenceValue:`, `setCullMode:`, `setFrontFacingWinding:`, `setDepthBias:slopeScale:clamp:`,
  `setDepthClipMode:`, `setTriangleFillMode:`, `setVertexBuffers:offsets:withRange:`,
  `setFragmentBuffers:offsets:withRange:`, `setVertexTexture:atIndex:`, `setVertexSamplerState:atIndex:`,
  `setBlendColorRed:green:blue:alpha:`, `updateFence:afterStages:`, device `newFence`,
  `maxThreadsPerThreadgroup`, `featureProfile`, `registryID`, `newTextureLayoutWithDescriptor:isHeapOrBufferBacked:`,
  texture `setPurgeableState:`.
- MapKit SnapshotService: `6 render passes (0 on the GPU)`: pass-not-on-gpu (depth attachment / format).
- The "no signature" lines (hundreds) are probably a data race: `routed_invoke` mutates the global
  `seen` NSMutableSet and reads `types` without a lock while MapKit encodes on several threads
  (SweetMetal.m:188-199). That can corrupt the set; worth a lock if the template path lives on.
- HANDOFF's open glitches (Control Center slider static frame, Today widgets = RenderBox MRT with an
  RG16F accumulator plus depth/stencil, Siri chip 1D textures, MapKit) all come from the missing features above.
- YouTube logged no MTL lines at all: it probably gets no Metal device (or cannot write the logs).
  Check this first when resuming: which processes get a device? (`oslog` on YouTube; a tiny test app.)

## 3. The reference: UniXNU's native Metal (reference-repos/UniXNU, all submodules checked out)

Docs: `docs/drivers/native-adreno.md`, `docs/devices/sheng.md`. Chain:
`NativeMetal plugin -> metal2vulkan (AIR->SPIR-V) -> Turnip (SPIR-V->NIR->IR3) -> darwin KMD -> NativeAdreno`.

- **metal2vulkan**: github.com/steelbrain/metal2vulkan (GPL-2.0+ OR LGPL-3.0+, Rust >= 1.98, ~262k lines
  in the translator). The translator plus `metal2vulkan-runtime`, which provides the whole Metal object
  model on ash/Vulkan: render, compute, blit, heaps, events, fences, ICBs, depth/stencil, MSAA resolve,
  memoryless, framebuffer fetch. Framebuffer fetch needs rasterization-order attachment access; Turnip
  has `EXT/ARM_rasterization_order_attachment_access` and `KHR_dynamic_rendering_local_read`
  (reference-repos/mesa tu_device.cc:237/386/419). See docs/RUNTIME-PORTING.md, METAL-COVERAGE.md,
  REFUSALS.md in the clone.
- UniXNU pins metal2vulkan **720ef1ce** + 5 patches in `drivers/NativeMetal/bridge/patches/` (0004 folds
  QuartzCore uber-shader constants before inlining; 0005 caches module decodes). The bridge,
  `bridge/NativeMetalBridge.rs` + `NativeMetalObjects.rs` (2027 lines), exposes a C ABI
  (`bridge/NativeMetalObjects.h`, the `nmb_*` functions). Build: `scripts/build-native-metal-bridge.py`
  (cargo +1.98.0, staticlib, aarch64-apple-ios).
- **Turnip darwin KMD**: `patches/mesa/turnip-darwin-kmd.patch` (1437 lines; new `tu_knl_darwin.cc`).
  Mesa pin c823a83e (`scripts/data/adreno-compiler-inputs.json`); our reference-repos/mesa is shallow and
  does not contain it. Other mesa patches there: ir3-darwin-build-id, turnip-builtin-shader-pack,
  turnip-empty-scissor, turnip-graphics-cache-feedback, ir3-portable-asm-roundtrip. Build:
  `scripts/build-turnip-darwin.py --target ios` (meson cross file, `-Dfreedreno-kmds=darwin`, NDEBUG
  release: debug builds made 58 pipelines take 68 s). The patch also gives Turnip an exported
  `vkGetInstanceProcAddr` (iOS has no loader), stubs CTR_EL0 (EL0 reads SIGILL on Darwin), drops
  `pthread_condattr_setclock`, and adds `VK_EXT_external_memory_host` for IOSurface imports.
- **Frontend**: `drivers/NativeMetal/NativeBridgeMetalFrontend.mm` (1667 lines). Only the device subclasses
  `_MTLDevice`; every other object is its own NSObject class (it does not subclass `_MTLCommandQueue`
  etc. as we do). `NativeAdrenoMetalPlugin.mm`: "publish" mode gives backboardd the device;
  `NativeAdrenoMetal.clients` lists other processes that get their own device.
- **Their numbers (SM8550)**: cold boot 103 Metal pipelines in 40 s (worst 1.4 s); later boots load
  exported caches (`m2vpack`, Turnip pipeline cache, priming cache). Expect slower on the SD855:
  **a persistent cache is mandatory**, and so is a host-side pre-fill (UniXNU's "compiler peer":
  the same Turnip source built for macOS gives matching cache keys, ir3-darwin-build-id patch).
- **Kernel ABI** they need: `drivers/NativeAdreno/NativeAdrenoABI.h` (v1, struct methods): Query,
  GetDeviceInfo, BoAlloc, BoMap (**client picks the IOVA**), BoFree, BoCpuMap (token for
  IOConnectMapMemory64, **write-combined**), BoCpuUnmap, Submit (up to 16 IBs, kIOReturnBusy when the
  job slots are full), WaitFence (ns timeout, 0 = poll), BoImport (pin the caller's pages: IOSurface base
  address), Get/SetMetadata, GetTimestamp, GetStatus, Create/CloseQueue. Each connection gets its own VA window
  (render server 1 GiB at 0x2000_0000, clients 256 MiB). The user client is named `AGXDeviceUserClient`
  with open type 0x4147, plus a selector "tunnel" for sandbox method filters (`dw_probe_selector`).
  Reference implementation: NativeAdrenoUserClient.cpp, NativeAdrenoMemory.cpp (importTask,
  makeWriteCombined via pmap_set_cache_attributes VM_WIMG_WCOMB), NativeAdrenoSubmit.cpp.

## 4. Two kernel fixes worth doing first (Phase 0; small; help the current path too)

1. **The write-combine panic, root cause found.** `osfmk/arm/pmap/pmap.c:6884`:
   `pmap_sync_page_attributes_phys(ppnum_t pp) { flush_dcache((vm_offset_t) (pp << PAGE_SHIFT), ...) }`
   shifts a 32-bit `ppnum_t`, so PA 0x1f87e8000 (pn 0x7e1fa) becomes 0xf87e8000. That is exactly the
   "phystokv_range: illegal PA 0xf87e8000" panic noted in SweetAccelerator.cpp:236-241. Fix: `ptoa(pp)`.
   Then `clientMemoryForType` can hand out kIOMapWriteCombineCache mappings: userland stops cleaning
   caches per submit (sg_cache_flush of every touched BO) and stops invalidating before getBytes. Turnip
   assumes WC BOs. Check other `<< PAGE_SHIFT` on ppnum_t in pmap.c/caches.c the same way.
2. **Submit wrapper like msm's a6xx_submit**: at the start `CP_EVENT_WRITE PC_CCU_INVALIDATE_COLOR/DEPTH`
   (+ CACHE_INVALIDATE); at the end the fence written by `CP_EVENT_WRITE CACHE_FLUSH_TS` (addr, value)
   instead of WFI + CP_MEM_WRITE; then `CP_WAIT_MEM_GTE`/`CP_WAIT_FOR_ME` as Linux does. Check against
   reference-repos/linux-msm drivers/gpu/drm/msm/adreno/a6xx_gpu.c `a6xx_submit()`.

## 5. Plan for the port (A640, iOS 16.7)

### Phase 1: host only (no phone; can start while WiFi runs)
1. Toolchain: `rustup toolchain install 1.98.0`, `rustup target add aarch64-apple-ios --toolchain 1.98.0`;
   a Python venv with meson, mako, pyyaml, packaging; `brew install glslang` (not installed now; no
   meson/mako either). Deployment target **iOS 16.0** everywhere (UniXNU uses 17.0), and after each build
   `nm -m X | grep "weak external"` (the strchrnul lesson).
2. **Translator check (cheapest proof)**: build the metal2vulkan CLI for macOS and run it on
   `research/gpu/QuartzCore.metallib` (iOS 16.7, triple air64-apple-ios16.7.0) for the fixed_vert/fixed_frag
   specializations in `work/mtl/keys-phone.log` (5028 lines; `tools/mtlkeys.py` parses them), plus RenderBox's and
   SiriUICore's metallibs in `work/mtl/libs`. Device profile: take `vulkaninfo --json` from the Lima VM
   (`iosroot`, turnip + drm-shim, FD_GPU_ID=640) and write a Khronos profile. If iOS 16 AIR fails to parse,
   that is the first problem to solve.
3. Same VM: run metal2vulkan-runtime on Linux Turnip/drm-shim (noop submits) to time pipeline creation per
   CA key and to check that every key compiles. That gives a refusal list before touching the phone.
4. Mesa: clone the UniXNU pin into work/, apply turnip-darwin-kmd.patch (+ build-id/builtin-pack/
   empty-scissor/cache-feedback), build `libvulkan_freedreno.dylib` for arm64-apple-ios16.0 (WSI off; no
   CAMetalLayer). Build the bridge staticlib for aarch64-apple-ios.

### Phase 2: kernel (SweetAccelerator v2, a new user-client type next to today's)
- Implement NativeAdreno ABI v1 (section 3) so that `tu_knl_darwin.cc` is used unchanged:
  - GetDeviceInfo: chip_id 0x06040001 (the darwin KMD computes gpu_id 640 from it; remove its 740 special
    case), gmem_bytes 0x100000, gmem_base 0x100000, uche_trap_base 0x1fffffffff000, highest_bank_bit 15,
    ubwc fields as `sweet_a6xx.c:976-978` programs them (UBWC 3, AMSBC), flags ClientIova,
    va_start/va_size = this connection's window.
  - **Per-connection VA windows** (Turnip picks addresses itself, so a shared table would let processes
    collide): e.g. backboardd 4 GiB, others 1 GiB each, inside 0x2_0000_0000..0x7f_0000_0000. Later:
    per-process page tables (CP_SMMU_TABLE_UPDATE, as msm does) for isolation.
  - BoMap at the client's IOVA = `a6_map_pages` at a given VA (today `sweet_gpu_map_user` allocates one);
    BoCpuMap = clientMemoryForType token with WC (needs Phase 0 fix 1); BoImport =
    `IOMemoryDescriptor::withAddressRange(addr, len, kIODirectionInOut, task)` + prepare (iOS pages are 16K,
    the GPU's 4K: `a6_map_pages` already takes page_size); Submit with N IBs + Phase 0 fix 2; WaitFence
    in ns; Busy when the ring is full.
  - Sandbox: our "Sandbox" MAC policy is permissive (bsd/kern/sweet_amfi.c), so the AGX naming and the
    selector tunnel are probably not needed. Keep the KMD's direct mode (env TU_DARWIN_SERVICE, open type 0).
- Keep selectors 0-7 for the current SweetMetal until the switch.

### Phase 3: userland
- New bundle (or SweetMetal v2): frontend = NativeBridgeMetalFrontend.mm adapted to iOS 16 (our SweetMetal.m
  is the reference for what iOS 16's CA calls: the `-device` on command buffers, `newPipelineLibraryWithFilePath:`
  -> nil, `MTLFailureTypeSetErrorModeType`, CAEnableDeepFramebuffer=NO, Apple-family answers so CA picks
  `_lph` shaders, nil device when the GPU cannot be opened). Turnip dylib inside the bundle; the shader
  cache dir in /var/local (or the process's cache dir) must be writable for backboardd.
- Presentation stays as it is: CA renders into IOSurfaces, SweetFramebuffer presents with the kernel blit.
  Import CA's IOSurfaces via BoImport (host pointer, page aligned; SweetSurface memory is an
  IOBufferMemoryDescriptor) or add a BoImportSurface(id) built on today's sel 3.
- Order: mtlk-style smoke test (Turnip clear + readback, from ssh) -> backboardd only (home screen) ->
  measure cold compile -> cache export/import across boots -> host pre-fill -> other processes
  (SpringBoard, posters, chronod/RenderBox widgets, MapKit) -> third-party apps -> WebKit GPU process back
  on (`WebKit2GPUProcessForDOMRendering`) for Safari.

### Phase 4: performance and polish
- Scrolling sits at 21-26 fps and backboardd idles, so the limit is pacing. SweetFramebuffer only ever sends
  notification type 5 (vsync); `kSweetFBSwapWait` is a no-op; a swap shows at the next 60 Hz tick
  (`SweetFramebuffer.cpp:257-330`). Find out what IOMobileFramebuffer userland waits for between swaps
  (disassemble IOMobileFramebuffer.framework from the shared cache: swap-complete/"swap done" callbacks)
  and send swap-complete when the blit finishes.
- GMEM (tiled) rendering, LRZ and UBWC come with Turnip for free; sysmem is all we have now.
- Video (YouTube): H264SW writes through a wrong plane pointer (HANDOFF: check
  IOSurfaceGetBaseAddressOfPlane vs the 0104 planar layout). CA then needs biplanar 420v/420f sampling;
  metal2vulkan has two-plane/YCbCr paths (`texture/surface_two_planes.rs`, `ycbcr*.rs`).

## 6. If the port must wait: fixes in the current template path, in order of value

1. Phase 0 fixes 1 and 2 (WC mappings, kernel flushes).
2. Lock `seen`/`types` in the logging forwarder (data race, multi-threaded encoders).
3. State setters that are cheap to carry as keys: cull mode, winding, depth bias, fill mode, blend colour,
   `setVertex/FragmentBuffers:offsets:withRange:`, vertex textures/samplers, `newFence`/`updateFence:` as no-ops.
4. Depth/stencil attachments + `setDepthStencilState:` (RB_DEPTH_* in templates; capture with a depth
   attachment) and real MRT for RenderBox (RG16F att1). This is the point where the template approach
   starts duplicating Turnip, which is the argument for the port.

## 7. Open questions to settle early
- Does metal2vulkan read iOS 16.7 AIR (QuartzCore MTLB v2.7)? -> Phase 1 step 2 answers it.
- Cold-compile time of CA's ~100-400 pipelines on the A76 cores, and backboardd's memory (jetsam limit) with
  Turnip + the Rust runtime loaded.
- Which processes get a Metal device today, and why YouTube logs nothing.
- Whether `pmap_set_cache_attributes(VM_WIMG_WCOMB)` on 16K pages under our kernel has other traps
  (NativeAdreno runs 4K pages; note its "int-147" lesson: the GPU must map BOs non-cacheable when the CPU
  view is WC).
