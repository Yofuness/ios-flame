# Adreno 618 (sunfish) vs flame's A640 — GPU bring-up notes

Sources: sunfish live DT (`recon/live.dts`), Google sunfish kernel (LineageOS android_kernel_google_msm-4.14
lineage-20: drivers/gpu/msm/adreno_a6xx_gmu.c, kgsl_hfi.c, kgsl_gmu.c; drivers/clk/qcom/gcc-sdmmagpie.c),
our core `bsd/dev/sweet_a6xx.{c,h}` + `sweet_gpu.c` (A640, kernels 0095–0122).

## Addresses
| Item | A640 (flame, in code) | A618 (sunfish) | Source |
|---|---|---|---|
| Chip ID (`A6_CHIP_ID`) | 0x06040001 | **0x06010800** | DT qcom,chipid |
| GPU regs (`A6_GPU_BASE`) | 0x02c00000 | **0x05000000** | DT kgsl-3d0 reg |
| GMU | base+0x6a000 | 0x0506a000 (same offset) | DT gmu |
| PDC cfg / seq | 0x0b290000 / 0x0b490000 | same | DT gmu reg |
| GPUCC (`A6_GPUCC`) | 0x02c90000 | **0x05090000** | DT qcom,gpucc (sdmmagpie) |
| GPUCC CX/GX GDSCR | +0x106c / +0x100c | same (0x509106c / 0x509100c) | DT gdsc |
| GPU SMMU (`A6_SMMU`) | 0x02ca0000 | **0x05040000** | DT kgsl-iommu |
| SIDs | GPU 0 (mask 0x401), GMU 5 (mask 0x400) | GPU 0/2, GMU 4/5 | DT iommus |
| Zap region (`ZAP_REGION`) | 0x9a915000 | **0x98f95000** (0x2000) | DT pil_gpu_mem |
| Zap PAS id / name | 13 / a640_zap | 13 / **a615_zap** | DT kgsl-hyp |
| GCC gpu_cfg_ahb | 0x71004 | 0x71004 | gcc-sdmmagpie.c |
| GCC ddrss_gpu_axi | 0x71154 | **0x4452c** | gcc-sdmmagpie.c |
| GCC gpu_memnoc_gfx | 0x7100c | 0x7100c | |
| GCC gpu_snoc_dvm_gfx | 0x71018 | 0x71018 | |
| Splash FB (`A6_FB_PA`) | 0x9c400000 | 0x9c000000 (done in 0125) | |

## Firmware (restore/fw/vendor)
a618_gmu.bin (32 KB), a630_sqe.fw (SQE, shared by a6xx), a615_zap.mdt/.b00–.b02.

## GMU / HFI behaviour differences (Google kgsl)
- PDC TCS1/TCS3 command 1 data: A618 **0x2**, A640 0x3 (TCS1 cmd1 is 0x0 for both). Resource addr 0x30090 both.
- `qcom,gpu-quirk-hfi-use-reg` on A618: GPU DCVS via GMU registers (`GMU_DCVS_NOHFI`), not HFI.
- HFI start: if HFI major < 2 → **v1 DCVS table**, BW table, then **H2F_MSG_TEST** (quirk). If ≥ 2 → v2 table,
  feature ctrl, CORE_FW_START (A640 path). H2F_MSG_START only on A640/A680.
- A618 GPU max 825 MHz (pwrlevel 0 = 0x312c8040).

## Plan
M2: firmware into the GPU fw dir; M3: constants (addresses above) + GCC axi offset; M4: GMU boot +
HFI version read → pick v1/v2 path; M5: ring NOP; M6 Turnip (A618 supported upstream); M7 compositor.

## Status 08:30 — BLOCKED at HFI start (M4b), 3-attempt rule hit
What works on the A618 now (patches 0129–0135): GCC/GPUCC clocks, CX GDSC, GMU legacy firmware load,
**GMU boot (init result 0xbabeface)**, GFX rail OOB (acked), SPTPRAC on, **GX GDSC on**.
Blocker: right after `GMU_HFI_CTRL_INIT=1` the GMU CM3 faults (GMU2HOST bit 23 HFI_IRQ_CM3_FAULT),
HFI status stays 0, no message is ever acked. Tried: legacy HFI queue ids (0131), INIT/v1 table/TEST
order (0132), packed chip id + gfx rail + SPTPRAC + GBIF/WFI (0133), 4-queue dword-sized table (0134),
kgsl VA order HFI@0x60001000 (0135).
Diagnostic (0136): GMU SMMU bank FSR 0x400 (format bits only), FAR 0 → **not** a translation fault;
the CM3 faults internally.
Exactly what's needed next (in order of likelihood):
1. RPMh/PDC/RSCC sleep sequences for A618: compare our `rpmh_init` (A640 values from flame) with kgsl
   `_load_gmu_rpmh_ucode` + `a6xx_gmu_rpmh_init` line by line (RSCC seq words, PDC seq words, TCS
   setup) — the legacy firmware likely runs these on HFI init.
2. Read the CM3 fault context: legacy fw writes a crash dump to the `dbg_buffer` given in H2F_MSG_INIT
   (we pass 0) — map a DUMPMEM region at kgsl's VA and pass it, then dump it via /dev/pmem.
3. GMU registers kgsl snapshots on fault (A6XX_GMU_CM3_* / AO_HOST_INTERRUPT_STATUS) for the fault PC.
Separately, CP `me_init` times out (hwfault c0000) — expected while the GMU/GX vote isn't up; revisit after.

### 09:10 update — next-step #1 checked, eliminated
Our RSCC (seq mem 0xA7A506A0…0x0020E8A8, PDC_SEQ_START 0x4520, match 0x4510/0x4514), PDC seq
(0xFEBEA1E1…0x002081FC), TCS1/TCS3 (with A618 cmd1 data 2) and PDC enable (SEQ_START 0, ENABLE
0x80000001) match kgsl `_load_gmu_rpmh_ucode` exactly. Step #2 (dbg_buffer) can't help: the fault is
before H2F_MSG_INIT. Remaining path: dump GMU DTCM/ITCM after the fault (kgsl a6xx_gmu_snapshot does this)
into low RAM and decode the CM3 exception frame (PC/LR → firmware offset). Multi-hour; not today.

### 09:45 — CM3 fault decoded (0138 TCM dump)
The 0138 boot (k20) died before the console caught up (watchdog → fastboot), but the copy landed in RAM:
a warm-reset boot of k14 (k21) read it back from 0xE2010000.
- DTCM 0x41e00: crash block, magic `0xdead5aab`; EXC_RETURN 0xfffffff9, SP 0x43f10.
- Exception frame at 0x43f18: r0 0, r1 0x400d8, r2 0, **r3 0x60001000**, r12 0, **lr 0x227f, pc 0x19a8**, xpsr 0x61000200.
- a618_gmu.bin: 0x2270 spins until HFI_CTRL_INIT is set, then calls 0x19a0, which loads the HFI
  queue-table pointer (0x60001000) and does `ldr r4, [r3, #8]` at **0x19a8 → bus fault**.
- So the GMU cannot read its HFI memory through the SMMU. The A618 legacy fw runs from ITCM (no SMMU
  needed), which is why GMU boot succeeds; the HFI table is its first SMMU-translated access.
- Suspect: our SMR write doesn't stick. `smmu: smr0 0 -> cb0` on every sunfish boot (readback 0),
  flame read back `84000005`. CB registers (CBAR) do stick. DT: GMU kernel CB SID 5, user CB SID 4,
  `#iommu-cells = 1` (no mask), plus `attach-impl-defs` (0x6000=0x2378 … 0x6b68) we never write.
- No CB fault (0136) fits: an unmatched stream gives a *global* fault (sGFSR), not a CB fault.
Next: 0139 logs sCR0/sGFSR/sGFSYNR0/sGFAR and SMR/S2CR 0–7 after the fault.

### 09:55 — 0139 inconclusive; GPU parked (3-attempt rule on diagnostics)
- k22 (0139): log stopped after `legacy: gfx rail … acked`; the phone was back in fastboot ~2 min after boot
  (well before flamereboot=600) → reading SMMU global registers (sCR0/sGFSR/SMR 1–7) right after the fault
  probably resets the SoC. The RAM trail held only older lines (no diag output).
- New lead in every sunfish log: `gcc gpu_memnoc_gfx = 80000221 (still halted)` and `gpu_snoc_dvm_gfx … (still
  halted)`. DT: gcc_gpu_memnoc_gfx_clk is **the kgsl SMMU's clock**. If it is really off, SMR writes are dropped
  (readback 0) and the GMU's memory path is dead → exactly the 0x19a8 bus fault.
Next session, in order:
1. Find why gcc_gpu_memnoc_gfx stays halted on sdmmagpie (gcc-sdmmagpie.c: its parent/vote, halt_check type;
   kgsl enables it via the SMMU node's clock + vdd-supply). Fix → expect SMR readback 0x84000005.
2. Write the DT `attach-impl-defs` pairs (0x6000=0x2378 … 0x6b68=0x9a82a382) before programming the SMR.
3. Re-run HFI; the 0138 TCM dump (decoded above) shows whether the fault moves.

### 11:10 — HFI solved
1. SMR mask must be 0 (DT #iommu-cells = 1); the hypervisor drops SMRs with mask 0x400 (0141).
2. Legacy HFI v1: no 0xfafafafa 4-dword padding of queue messages (0144).
Also mapped GMU SID 4 (0142). Bring-up 297 ms; DCVS + OOB acked; GPU composites the UI.
Debug method that found both: copy GMU DTCM to low RAM (0xE2010000) after the fault, read it via
/dev/pmem (warm reset keeps RAM), decode the CM3 exception frame + crash block (magic 0xdead5aab at 0x41e00).
