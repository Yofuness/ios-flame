/*
 * camrx: camera stage c1, the CSID-lite 0 test pattern generator (no PHY, no IFE DMA, no SMMU). Register spec:
 * research/notes/camera-stage-c.md (tags redfin/DT/Linux/guess), sequences from ios-redfin 0921 (csid_reset,
 * csid_config, paths_down).
 *   camrx [--dry-run] status        read-only: PLL2, GDSC, the RCGs/CBCRs below, refgen, CSID-lite (if clocked)
 *   camrx [--dry-run] clocks on|off cphy_rx 384, ife_lite_csid 384, ife_lite 320 MHz (all PLL2) + their branches
 *   camrx [--dry-run] tpg [SECONDS] CSID reset, TPG 640x480 RAW10 colour bars into RDI0, poll IRQ status every 100 ms
 * Stage c2 (sensor data through CSIPHY0; Linux excerpts: research/notes/camera-stage-c2-linux.md):
 *   camrx [--dry-run] phy on [SETTLE]  csi0phytimer 300 MHz (PLL0 even /2; needs camclk pll0 on) + csiphy0/timer
 *                                      branches, CSIPHY0 v1.2 4-lane D-PHY init (redfin 0922 csiphy_up)
 *   camrx [--dry-run] phy off          CSIPHY0 IRQ masks 0 + reset (redfin csiphy_down), then the two branches off
 *   camrx [--dry-run] rx arm           CSID reset, RX from CSIPHY0 (4 lanes), RDI0 VC 0 DT 0x2b; left running
 *   camrx rx report                    read-only: RX/RDI0 counters + IRQ status (not cleared), CSIPHY0 status
 *   camrx [--dry-run] rx stop          RDI0 off, RX off, masks 0
 * Stage d (one frame through IFE-lite RDI0 WM0 into RAM; spec research/notes/camera-stage-d.md):
 *   camrx [--dry-run] capture OUTFILE [--stream-cmd "CMD"] [--alloc32]  one process for the whole DMA: buffer =
 *       display carveout slot1 PA 0x9cb00000 + 0xB00000 (zero-copy slot 1, unused without sweetfb_zerocopy; XNU has
 *       no RAM below 4 GB, cam10), zeroed through /dev/pmem first; --alloc32: 8 MB PMEM_ALLOC32 instead. SMMU (cam13): --trap = the 3 DT IFE SMRs
 *       (0x900/0x460 0x880/0x460 0x820/0x440) into one free bank with M=1 + EPD0|EPD1 (every access faults, the
 *       bank reports the SID), --bypass --sid X = one mask-0 SMR into an identity bank + (cam16) the other DT IFE
 *       SIDs (non-overlapping SMRs) into a trap bank; refused if any valid SMR overlaps (saved + restored on
 *       every exit), CSID rx arm, IFE reset + WM0 frame
 *       mode, runs CMD (default: camid mode 8 stream), WM0 off at the first buf done, readbacks, guard check
 *       (+ where), frame -> OUTFILE (fsync'd), cleanup: WM0 off, CSID down, IFE reset, identity SMR -> trap bank,
 *       2 s, late-write check, SMMU restore, PMEM_FREE (--alloc32 only). cam18: faults in our banks are sampled
 *       every 10 ms (FAR + SID, FSR cleared); --xlate = SMR 0x8c0/0x400 -> a translating bank that maps only
 *       0x9ca00000..0x9d600000 identity (6 x 2 MB), anything else a translation fault (terminated, sampled). Needs clocks on + phy on (+ camid's cpas_ahb/camnoc_axi).
 * Camera e1: camrx [--dry-run] stream SECONDS OUTPREFIX [--stream-cmd "CMD"]  capture's preconditions, SMMU (cam21:
 *       exact SMRs 0x8c0 + 0xcc0 -> bypass, other IFE SIDs -> trap) and cleanup; a 2-slot ring in carveout slot1,
 *       polled every 1 ms for RDI0 SOF (IFE irq0 bit 27) / WM0 buf done (BUS irq1 bit 0); at each SOF WM0_IMAGE_ADDR
 *       = the other slot + REG_UPDATE (redfin 0923). SECONDS from the first SOF; per-second fps; WM0 off, the
 *       command (default camid mode 8 --hold SECONDS+1) waited for, then the last completed frame -> OUTPREFIX.raw.
 * cam-f1: camrx [--dry-run] [--front] ...: the front IMX355 path for phy/rx/capture/stream/status: CSIPHY2
 *       (0xace4000, csi2phytimer RCG 0xad0604c + branches 0xad06064/0xad06068 instead of CSIPHY0's), RX_CFG0 phy 2,
 *       settle from 360 Mb/s/lane, frame 1640x924 RAW10 (mode 2), default command camid --front ... imx355-mode2.txt.
 *       CSID-lite, RDI0, IFE-lite WM0 and the SMRs (0x8c0 + 0xcc0) are the rear's.
 * Needs (checked, read-only): gcc camera_ahb on, TITAN_TOP GDSC on (camid --keep), PLL2 locked (camclk pll2 on).
 *   camrx smmu [SID...]   read-only: sCR0, valid SMRs/S2CRs, their banks, faults; which SMRs each SID (default: the
 *                         14 DT IFE SIDs) matches
 * Never enables PLLs/GDSCs/regulators, never writes GCC/RPMh/refgen. Writes only the whitelist in allowed();
 * every write is logged with its read-back. No camcc register is read before the GDSC check passes, no CSID
 * register before its two branches run.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../wifi/pmem.h"
#include "camrx_lib.h"

int main(int argc, char **argv)
{
	int a = 1;
	if (a < argc && !strcmp(argv[a], "--dry-run")) {
		dry = 1;
		a++;
	}
	int front = 0;	/* cam-f1 */
	if (a < argc && !strcmp(argv[a], "--front")) {
		front = 1;
		a++;
	}
	if (a >= argc)
		goto usage;
	pm_init();
	cam_select(front);
	if (!strcmp(argv[a], "status"))
		return status();
	if (!strcmp(argv[a], "smmu"))	/* cam13: read-only */
		return smmu_report(argc - a - 1, argv + a + 1);
	if (precheck())
		return 1;
	if (!strcmp(argv[a], "clocks") && a + 2 == argc) {
		if (!strcmp(argv[a + 1], "on"))
			return clocks_on();
		if (!strcmp(argv[a + 1], "off"))
			return clocks_off();
	}
	if (!strcmp(argv[a], "phy") && a + 2 <= argc && a + 3 >= argc) {
		if (!strcmp(argv[a + 1], "on")) {
			int st = a + 2 < argc ? atoi(argv[a + 2]) : -1;
			if (a + 2 < argc && (st < 0 || st > 255))
				goto usage;
			return phy_on(st);
		}
		if (!strcmp(argv[a + 1], "off") && a + 2 == argc)
			return phy_off();
	}
	if (!strcmp(argv[a], "rx") && a + 2 == argc) {
		if (!strcmp(argv[a + 1], "arm"))
			return rx_arm();
		if (!strcmp(argv[a + 1], "report"))
			return rx_report();
		if (!strcmp(argv[a + 1], "stop"))
			return rx_stop();
	}
	if (!strcmp(argv[a], "capture") && a + 2 <= argc) {
		const char *cmd = STREAM_CMD;
		int a32 = 0, trap = 0, byp = 0, sid = -1, xlate = 0;
		for (int i = a + 2; i < argc; i++) {
			if (!strcmp(argv[i], "--alloc32"))
				a32 = 1;
			else if (!strcmp(argv[i], "--trap"))
				trap = 1;
			else if (!strcmp(argv[i], "--bypass"))
				byp = 1;
			else if (!strcmp(argv[i], "--xlate"))	/* cam18: = --bypass --sid 0x8c0 --sid-mask 0x400, translating */
				xlate = byp = 1, sid = 0x8c0, sid_mask = 0x400;
			else if (!strcmp(argv[i], "--sid") && i + 1 < argc)
				sid = (int)(strtoul(argv[++i], NULL, 0) & 0x7fff);
			else if (!strcmp(argv[i], "--addr-offset") && i + 1 < argc) {
				addr_off = (uint32_t)strtoul(argv[++i], NULL, 0);
				if (addr_off & 0x3ff || addr_off >= 0x10000)
					goto usage;
			} else if (!strcmp(argv[i], "--sid2") && i + 1 < argc)
				sid2 = (int)(strtoul(argv[++i], NULL, 0) & 0x7fff);
			else if (!strcmp(argv[i], "--sid-mask") && i + 1 < argc)
				sid_mask = (uint16_t)(strtoul(argv[++i], NULL, 0) & 0x7fff);
			else if (!strcmp(argv[i], "--stream-cmd") && i + 1 < argc)
				cmd = argv[++i];
			else
				goto usage;
		}
		if (trap == byp || (byp && sid < 0) || (trap && sid >= 0))
			goto usage;
		return capture(argv[a + 1], cmd, a32, trap, sid, xlate);
	}
	if (!strcmp(argv[a], "stream") && a + 3 <= argc) {	/* camera e1 */
		int secs = atoi(argv[a + 1]);
		static char dcmd[512];
		const char *cmd = dcmd;
		if (secs < 1 || secs > STREAM_MAX_S)
			goto usage;
		snprintf(dcmd, sizeof dcmd, "%s --hold %d", STREAM_CMD, secs + 1);
		for (int i = a + 3; i < argc; i++)
			if (!strcmp(argv[i], "--stream-cmd") && i + 1 < argc)
				cmd = argv[++i];
			else
				goto usage;
		return stream_run(secs, argv[a + 2], cmd);
	}
	if (!strcmp(argv[a], "tpg") && a + 2 >= argc) {
		int s = a + 1 < argc ? atoi(argv[a + 1]) : 1;
		if (s < 1 || s > 60)
			goto usage;
		return tpg(s);
	}
usage:
	fprintf(stderr, "usage: camrx [--dry-run] [--front] status | clocks on|off | tpg [SECONDS 1-60] | phy on [SETTLE 0-255] | "
			"phy off | rx arm|report|stop | smmu [SID...] | capture OUTFILE --trap|--bypass --sid 0xNNN [--sid2 0xNNN] [--sid-mask 0xM] [--addr-offset 0x400]|--xlate "
			"[--stream-cmd \"CMD\"] [--alloc32] | stream SECONDS OUTPREFIX [--stream-cmd \"CMD\"]\n");
	return 2;
}
