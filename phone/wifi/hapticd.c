/*
 * hapticd (haptics step 3): iOS's haptics on the DRV2624. SunfishAudio.driver writes what iOS plays on its
 * "Actuator" device (48 kHz mono s16) into the FIFO /var/run/sunfish-haptic (made here, 0666); this turns it into
 * RTP plays, by ios-redfin's rules (hakios-xnu bsd/dev/redwood_haptic.c serve(), MIT):
 *   a burst  = samples above 256 after a pause (30 ms without one ends it)
 *   tap      = 8 ms into a burst (its peak known): one 18 ms RTP pulse, level 0x60 * peak / 32767
 *   vibration = a burst going on past 60 ms: RTP on, its level following the burst's peak every 20 ms
 *              (0x40 * peak / 32767), stopped when the burst ends or after 1000 ms, then 1000 ms rest
 * The chip is out of reset only while playing: on the first event SE9 is set up, gpio11 configured and released,
 * Android's init written (drv2624.c); 2 s after the last play it goes back to reset and gpio11 to its old CFG
 * (also on SIGTERM/SIGINT). A fault (STATUS UVLO/OT/OC) or an I2C error: reset, and the chip is left alone for the
 * rest of the run (the FIFO is still drained). Hard caps: drv2624.c (OD_CLAMP <= 0xA1, LRA bit, no 0xFD-0xFF,
 * RTP <= 0x60). Each event is one line on stdout: "hapticd: tap|vibration|cut ... ms ... level ...".
 * SE9 is shared (cs35l41, camid, i2cprobe, kern.sweet_camprobe): hapticd only touches it while playing, but a
 * play during another SE9 user's transfer can corrupt both; don't run those while haptics are expected.
 * Needs sweetse9=2 and the ADT /product/haptics (sunfish-boot.sh HAPTICS=1). Off: /var/local/haptics.off makes it
 * exit 0 at start (launchd job org.iosonpixel.hapticd then leaves it stopped; tools/sunfish-install-haptics.sh).
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "drv2624.h"
#include "geni_i2c.h"
#include "pmem.h"

#define FIFO		"/var/run/sunfish-haptic"
#define SILENT_BELOW	256
#define ACTIVE_US	30000
#define SETTLE_US	8000
#define TAP_US		60000
#define TAP_PULSE_MS	18
#define FOLLOW_US	20000
#define MAX_MS		1000
#define IDLE_OFF_US	2000000
#define TAP_LEVEL	0x60
#define VIB_LEVEL	0x40

static int up, broken;

static uint64_t now_us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
}

static void on_sig(int s)
{
	drv_reset_low();
	drv_cfg_restore();
	_exit(128 + s);
}

/* redwood_haptic.c's gain, on the RTP scale: as loud as iOS's waveform, cap at full scale, at least 1 */
static uint8_t level_for(uint32_t peak, unsigned cap)
{
	unsigned l = peak >= 32767 ? cap : cap * peak / 32767;
	return (uint8_t)(l ? l : 1);
}

static void chip_off(const char *why)
{
	if (!up)
		return;
	drv_reset_low();
	drv_cfg_restore();
	up = 0;
	printf("hapticd: chip in reset (%s)\n", why);
}

static void give_up(const char *why)
{
	printf("hapticd: %s: THE CHIP IS PUT INTO RESET and left alone for the rest of the run\n", why);
	chip_off(why);
	broken = 1;
}

static int chip_on(void)
{
	if (up)
		return 0;
	if (broken)
		return -1;
	up = 1;
	gi2c_lock();	/* SE9 lock over power-up + init */
	int bad = gi2c_init() || drv_pin_setup() || drv_power_up() || drv_init_chip();
	gi2c_unlock();
	if (bad) {
		give_up("power-up or init failed");
		return -1;
	}
	printf("hapticd: chip up\n");
	return 0;
}

static int go_locked(int on, uint8_t level);

static int go(int on, uint8_t level)	/* SE9 lock over each start/stop sequence */
{
	gi2c_lock();
	int r = go_locked(on, level);
	gi2c_unlock();
	return r;
}

static int go_locked(int on, uint8_t level)
{
	if (on && (drv_wr(DRV_R_RTP, level) || drv_wr(DRV_R_GO, 1)))
		return -1;
	if (!on) {
		uint8_t st;
		if (drv_wr(DRV_R_GO, 0))
			return -1;
		delay_ms(12);	/* leave pseudo-standby with a read (D 7.3.10), as drvbuzz */
		if (drv_rd(DRV_R_STATUS, &st))
			return -1;
		if (st & 7) {
			printf("hapticd: STATUS %#04x: UVLO %u OT %u OC %u\n", st, (st >> 2) & 1, (st >> 1) & 1, st & 1);
			return -1;
		}
	}
	return 0;
}

int main(void)
{
	enum { IDLE, TAP, LOOP, REST } state = IDLE;
	uint64_t heard = 0, quiet = 0, onset = 0, tap_onset = 0, since = 0, followed = 0, rest_until = 0, last_play = 0;
	uint32_t peak = 0;
	uint8_t playing = 0;
	unsigned taps = 0, loops = 0, capped = 0, follows = 0;
	int16_t buf[1024];
	size_t carry = 0;	/* a sample's first byte left over from the last read */

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (access("/var/local/haptics.off", F_OK) == 0) {	/* off switch: leave SE9 to other users */
		printf("hapticd: /var/local/haptics.off, not starting\n");
		return 0;
	}
	drv_tag = "hapticd";
	drv_verbose = 0;
	drv_rtp_max = TAP_LEVEL;
	pm_init();
	if (drv_pick_tile())
		return 1;
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGPIPE, SIG_IGN);
	unlink(FIFO);
	if (mkfifo(FIFO, 0666) || chmod(FIFO, 0666)) {
		printf("hapticd: %s: %s\n", FIFO, strerror(errno));
		return 1;
	}
	int fd = open(FIFO, O_RDONLY | O_NONBLOCK);
	int keep = open(FIFO, O_WRONLY | O_NONBLOCK);	/* a writer of our own: no EOF when the plug-in closes */
	if (fd < 0 || keep < 0) {
		printf("hapticd: open %s: %s\n", FIFO, strerror(errno));
		return 1;
	}
	printf("hapticd: reading %s (48 kHz mono s16); tap <= %u ms, vibration capped %u ms + %u ms rest\n", FIFO,
	       TAP_US / 1000, MAX_MS, MAX_MS);
	int poll4 = getenv("HAPTICD_POLL4") != NULL;	/* O12: 1 = the old fixed 4 ms poll also when idle */
	for (;;) {
		struct pollfd p = { fd, POLLIN, 0 };
		int wait = 4;
		uint64_t t0 = now_us();
		/* O12: idle (no sound tracked, nothing to play) blocks until data or the 2 s chip-off is due */
		if (!poll4 && state == IDLE && (heard == 0 || t0 - heard >= ACTIVE_US)) {
			wait = -1;
			if (up) {
				uint64_t left = t0 - last_play > IDLE_OFF_US ? 0 : IDLE_OFF_US - (t0 - last_play);
				wait = (int)(left / 1000) + 1;
			}
		}
		if (poll(&p, 1, wait) > 0) {
			ssize_t n = read(fd, (char *)buf + carry, sizeof(buf) - carry);
			if (n > 0) {
				size_t bytes = carry + (size_t)n, k = bytes / 2;
				uint32_t pk = 0;
				for (size_t i = 0; i < k; i++) {
					uint32_t v = buf[i] < 0 ? (uint32_t)-(int32_t)buf[i] : (uint32_t)buf[i];
					pk = v > pk ? v : pk;
				}
				carry = bytes & 1;
				if (carry)
					((char *)buf)[0] = ((char *)buf)[bytes - 1];
				uint64_t t = now_us();
				if (pk > SILENT_BELOW) {
					if (heard == 0 || quiet > heard || t - heard >= ACTIVE_US)
						onset = t, peak = 0;
					heard = t;
					peak = pk > peak ? pk : peak;
				} else {
					quiet = t;
				}
			}
		}
		uint64_t now = now_us();
		int active = heard != 0 && now - heard < ACTIVE_US;
		switch (state) {
		case IDLE:
		case TAP:
			if (active && onset != tap_onset && now - onset >= SETTLE_US) {
				uint8_t l = level_for(peak, TAP_LEVEL);
				tap_onset = onset;
				gi2c_lock();	/* one tap = one locked power-up/RTP/stop sequence */
				int bad = chip_on() || go(1, l);
				if (!bad) {
					delay_ms(TAP_PULSE_MS);
					bad = go(0, 0) ? 2 : 0;
				}
				gi2c_unlock();
				if (bad) {
					if (up)
						give_up(bad == 2 ? "tap stop failed or a fault" : "tap start failed");
					break;
				}
				state = TAP;
				since = last_play = now;
				taps++;
				printf("hapticd: tap %u: %u ms level %#04x (peak %u)\n", taps, TAP_PULSE_MS, l, peak);
				break;
			}
			if (state == TAP && active && quiet < tap_onset && now - since > TAP_US) {
				uint8_t l = level_for(peak, VIB_LEVEL);
				if (chip_on())
					break;
				if (go(1, l)) {
					give_up("vibration start failed");
					break;
				}
				state = LOOP;
				since = followed = last_play = now;
				playing = l;
				follows = 0;
				peak = 0;
				loops++;
				printf("hapticd: vibration %u: start level %#04x (cap %u ms)\n", loops, l, MAX_MS);
			} else if (state == TAP && !active && now - since > TAP_US) {
				state = IDLE;
			}
			break;
		case LOOP:
			if (active && now - followed >= FOLLOW_US) {
				uint8_t l = level_for(peak, VIB_LEVEL);
				followed = now;
				peak = 0;
				if (l != playing && (l > playing + 4 || l + 4 < playing)) {
					if (drv_wr(DRV_R_RTP, l)) {
						give_up("level change failed");
						state = IDLE;
						break;
					}
					playing = l;
					follows++;
				}
			}
			if (!active || now - since > MAX_MS * 1000ull) {
				int cut = now - since > MAX_MS * 1000ull;
				if (cut)
					capped++, rest_until = now + MAX_MS * 1000ull;
				if (go(0, 0))
					give_up("vibration stop failed or a fault");
				printf("hapticd: %s %u: %llu ms level %#04x (%u level changes, %u cut so far)\n", cut ? "cut" : "vibration",
				       loops, (unsigned long long)(now - since) / 1000, playing, follows, capped);
				state = cut ? REST : IDLE;
				last_play = now;
			}
			break;
		case REST:
			if (now >= rest_until)
				state = IDLE;
			break;
		}
		if (up && state != LOOP && now - last_play > IDLE_OFF_US)
			chip_off("2 s idle");
	}
	return 0;
}
