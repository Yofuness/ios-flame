/*
 * Focus, where the camera has a lens motor (the frames' focus is a position, not REDWOOD_CAMERA_NO_FOCUS:
 * the wide camera's): by contrast, as there is nothing else to go by -- the sensor's phase-detection
 * pixels are not read. Included by camera_device.c, and by test_af.c against a simulated lens.
 *
 * af6: a search is a full sweep: every AF_COARSE over the travel, then around the sharpest in steps of AF_FINE, and
 * there the lens stays (no climbing: it gave up on the flat of the curve and left the preview soft). The first search
 * of a session waits for AE (statistics are skipped while it moves, for AF_AE_WAIT frames at most) and a new session
 * or a camera switch always starts with one (worker(): af_reset). In focus, the lens stays until the middle has been
 * under 70 % (or over twice) as sharp as it was for AF_UNLIKE measurements and has then kept still for a third of a
 * second, or until a search is asked for: a tap (AutoFocusNow, on the window AFWindowParams gave) always; continuous
 * focus (ContinuousAutoFocusNow) only coming from a lens held by a tap or by hand, as it is already watching otherwise.
 * Off: /var/local/camdev.af-old has the af5 search (climb, tap sweeps, AF_STEP/AF_FINE_OLD, one frame of settling).
 */
/*
 * The lens's travel focus uses (the kernel's positions: 0 infinity, 1023 macro). redfin (WS-A, kernel 0929): the back
 * camera's actuator is an ON Semi LC898129 (AF, OIS and EEPROM in one chip); the kernel maps 0..1023 onto its codes
 * 400..725 for now (to be widened from the EEPROM's calibration), so the whole of it is safe to use -- HackiOS's phone
 * stopped at 768 (an over-current on its LDO) and had a hard stop at 0. The IMX355 and IMX481 are fixed focus.
 */
#define AF_NEAR         0               /* (infinity) */
#define AF_FAR          REDWOOD_CAMERA_FOCUS_MAX        /* (macro) */
#define AF_STEP         64              /* af5: a step of the climb, and of a walk */
#define AF_FINE_OLD     16              /* af5 */
#define AF_COARSE       128             /* af6: the sweep */
#define AF_FINE         32              /* af6: around the sharpest, out to two of these either way */
#define AF_WALK         256             /* af6: at most this per frame on a longer way (camid walks each in small steps) */
/*
 * af6: frames let go by after the lens's last move, before one is measured. A FOCUS goes through camd's main loop, its
 * h_tick and camid's poll, up to 2 frames later than asked, and the frames' focus says what was asked, not what was
 * reached; then the frame being exposed during the move is spoilt too: 2 + 1. test_af prints what 1 and 2 do.
 */
#define AF_SETTLE_FRAMES 3
#define AF_AE_WAIT      60              /* af6: frames of a search (or of watching) to wait for AE at most: it may never settle */
#define AF_UNLIKE       30              /* measurements (about a second) unlike the focused picture for a change of scene */

/* AF_MODE_*: after a search, focus keeps watching (continuous), or stays where it is (auto: one search) */
enum { AF_MODE_AUTO = 1, AF_MODE_CONTINUOUS = 2 };

enum { AF_WATCH, AF_STEADY, AF_CLIMB, AF_SWEEP, AF_FINE_SEARCH, AF_SETTLE, AF_MANUAL };

struct af {
	int             state;
	uint32_t        at, target;     /* where the lens was told to go; where it is going a step at a time */
	unsigned        wait;           /* frames before the next measurement */
	int             way;            /* +1 nearer, -1 further */
	bool            turned;
	unsigned        worse;          /* steps in a row less sharp than the sharpest */
	uint32_t        start, best, fine_to;
	float           best_sharpness;
	float           sharpness, level;       /* in focus: the middle's, to watch */
	float           last_sharpness, last_level;
	unsigned        unlike, steady, steps;
	bool            now;            /* asked for: no waiting for the picture to keep still */
	bool            sweep;          /* ... and the whole travel measured */
	float           sum_sharpness, sum_level;
	unsigned        measured;
	/* redfin (B5) */
	bool            hold;           /* a one-off search (AF_MODE_AUTO) done: the lens stays until focus is asked for again */
	int32_t         manual_at;      /* the ManualFocusNow position being gone to or held (-1: none) */
	bool            started;        /* at and target are the kernel's */
	/* af6 */
	bool            old;            /* /var/local/camdev.af-old: the af5 search */
	unsigned        settle;         /* AF_SETTLE_FRAMES (1 when old) */
	unsigned        ae_waited;      /* frames skipped for AE, of AF_AE_WAIT */
};

static void
af_init(struct af *af, bool old)
{
	memset(af, 0, sizeof(*af));
	af->old = old;
	af->settle = old ? 1 : AF_SETTLE_FRAMES;
}

/* af6: a new camd session: the lens is elsewhere (camd resets focus_a) and what was found is no more */
static void
af_reset(struct af *af)
{
	if (!af->old) {
		unsigned settle = af->settle;

		af_init(af, false);
		af->settle = settle;
	}
}

/* whether focus is looking for the sharpest, or the lens is on its way somewhere (AFStatus; a photograph waits for it) */
static bool
af_searching(const struct af *af)
{
	return af->state == AF_CLIMB || af->state == AF_SWEEP || af->state == AF_FINE_SEARCH || af->state == AF_SETTLE ||
	    af->state == AF_MANUAL || ((af->state == AF_STEADY || af->state == AF_WATCH) && af->now) || af->at != af->target;
}

/* a focus asked for (AutoFocusNow and the like): from the next frame, whatever it was doing */
static void
af_asked(struct af *af, bool continuous)
{
	if (af->old) {
		af->state = AF_STEADY;
		af->now = true;
		af->sweep = !continuous;        /* (a tap sweeps, continuous focus climbs) */
		af->hold = false;
		af->manual_at = -1;
		return;
	}
	/* af6: continuous focus is what runs anyway: asking again (the app does) neither restarts a search nor hunts */
	if (continuous && (!af->started || af->now || af_searching(af) || (af->state == AF_WATCH && !af->hold)) && af->state != AF_MANUAL) {
		return;
	}
	af->state = AF_STEADY;
	af->now = af->sweep = true;
	af->hold = false;
	af->manual_at = -1;
	af->ae_waited = 0;
}

static void
af_go(int device, struct af *af, uint32_t target)
{
	struct redwood_camera_command c = { .command = REDWOOD_CAMERA_FOCUS };
	uint32_t walk = af->old ? AF_STEP : AF_WALK;

	af->target = target < AF_NEAR ? AF_NEAR : target > AF_FAR ? AF_FAR : target;
	c.a = af->at < af->target ? (af->target - af->at > walk ? af->at + walk : af->target) :
	    (af->at - af->target > walk ? af->at - walk : af->target);
	af->at = c.a;
	/*
	 * The lens settles in a few milliseconds; what is spoilt is the frame being exposed while it moved
	 * (written at a frame's end, the next one's exposure has begun), and it moves late (AF_SETTLE_FRAMES):
	 * those let go by before a measurement, none on the way.
	 */
	af->wait = af->at == af->target ? af->settle : 0;
	af->steps++;
	af->sum_sharpness = af->sum_level = 0;
	af->measured = 0;
	(void)camera_client_write(device, &c, sizeof(c));        /* sunfish (f2): to camd */
}

static void
af_sweep_begin(int device, struct af *af)
{
	af->state = AF_SWEEP;
	af->best_sharpness = -1;
	af->worse = 0;
	af->steps = 0;
	af->ae_waited = 0;
	af->way = af->at - AF_NEAR < AF_FAR - af->at ? 1 : -1;
	af_go(device, af, af->way > 0 ? AF_NEAR : AF_FAR);     /* (walked there a step at a time) */
}

/* around the sharpest; af6: what was measured there already (start) is not measured again */
static void
af_fine_begin(int device, struct af *af)
{
	uint32_t span = 2 * (af->old ? AF_FINE_OLD : AF_FINE), first = af->best > span + AF_NEAR ? af->best - span : AF_NEAR;

	af->state = AF_FINE_SEARCH;
	af->start = af->old ? ~0u : af->best;
	af->fine_to = af->best + span > AF_FAR ? AF_FAR : af->best + span;
	af_go(device, af, first == af->start ? first + AF_FINE : first);
}

static void
af_frame(int device, struct af *af, const struct redwood_camera_frame *f, const uint8_t *raw, const float window[4], bool locked,
    int mode, int32_t manual, bool ae_ok)
{
	float level, sharpness;
	unsigned needed = f->gain >= 16 * f->gain_unity ? 2 : 1;      /* (in the dark, two frames to a measurement) */

	if (f->focus == REDWOOD_CAMERA_NO_FOCUS) {
		return;
	}
	if (!af->started) {
		af->started = true;
		af->at = af->target = f->focus;         /* (where the kernel put it) */
		af->manual_at = -1;
		af->now = !af->old;                     /* af6: the first search of the session, once AE is there */
	}
	/* redfin (B5): ManualFocusNow: the lens there, a step at a time, and held */
	if (manual >= 0 && manual != af->manual_at) {
		af->manual_at = manual;
		af->state = AF_MANUAL;
		af->now = af->sweep = false;
		af_go(device, af, (uint32_t)manual);
		return;
	}
	if (af->wait > 0) {
		af->wait--;
		return;
	}
	if (af->at != af->target) {
		af_go(device, af, af->target);          /* (the next step of a longer way) */
		return;
	}
	if (af->state == AF_MANUAL) {
		af->state = AF_WATCH;
		af->hold = true;
		say("focus: the lens at %u, as asked", af->at);
		return;
	}
	/* redfin (B5): a frame exposed while the lens moved (the kernel's LENS_MOVING) is no measure of where it is */
	if (f->reserved[REDWOOD_CAMERA_R_FLAGS] & REDWOOD_CAMERA_LENS_MOVING) {
		return;
	}
	if (af->state == AF_WATCH && (locked || af->hold)) {
		return;                 /* (nothing to measure for) */
	}
	/* af6: the statistics move with the exposure: none while AE moves (not for ever) */
	if (!af->old && !ae_ok && af->ae_waited < AF_AE_WAIT) {
		af->ae_waited++;
		af->sum_sharpness = af->sum_level = 0;
		af->measured = 0;
		return;
	}
	if (!af->old && af->state == AF_WATCH && af->now) {
		af->now = false;
		af_sweep_begin(device, af);
		return;
	}
	sharpness = isp_sharpness(f, raw, window[0], window[1], window[2], window[3], &level);
	af->sum_sharpness += sharpness;
	af->sum_level += level;
	if (++af->measured < needed) {
		return;
	}
	sharpness = af->sum_sharpness / af->measured;
	level = af->sum_level / af->measured;
	af->sum_sharpness = af->sum_level = 0;
	af->measured = 0;
	switch (af->state) {
	case AF_WATCH:
		if (af->old ? locked || (af->sharpness > 0 && sharpness > af->sharpness * 0.6f && level > af->level * 0.7f &&
		    level < af->level * 1.4f) : sharpness >= af->sharpness * 0.7f && sharpness <= af->sharpness * 2) {
			af->unlike = 0;
			return;
		}
		if (af->sharpness > 0 && ++af->unlike < AF_UNLIKE) {
			return;
		}
		af->state = AF_STEADY;
		af->steady = 0;
		af->last_sharpness = sharpness;
		af->last_level = level;
		return;
	case AF_STEADY:
		if (fabsf(sharpness - af->last_sharpness) < 0.15f * af->last_sharpness && fabsf(level - af->last_level) < 0.1f * af->last_level) {
			af->steady++;
		} else {
			af->steady = 0;
		}
		af->last_sharpness = sharpness;
		af->last_level = level;
		if (af->steady < 10 && !af->now) {
			return;
		}
		af->now = false;
		if (af->sweep || !af->old) {
			af->sweep = false;
			af_sweep_begin(device, af);
			return;
		}
		af->state = AF_CLIMB;
		af->best = af->start = af->at;
		af->best_sharpness = sharpness;
		af->way = af->at + AF_STEP > AF_FAR ? -1 : 1;
		af->turned = false;
		af->worse = 0;
		af->steps = 0;
		af_go(device, af, (uint32_t)((int)af->at + af->way * AF_STEP));
		return;
	case AF_CLIMB: {
		int next = (int)af->at + af->way * AF_STEP;
		bool end = next < AF_NEAR || next > AF_FAR;

		if (sharpness > af->best_sharpness * 1.02f) {
			af->best = af->at;
			af->best_sharpness = sharpness;
			af->worse = 0;
		} else {
			af->worse++;
		}
		if (af->worse < 2 && !end) {
			af_go(device, af, (uint32_t)next);
			return;
		}
		next = (int)af->best - af->way * AF_STEP;
		if (!af->turned && af->best == af->start && next >= AF_NEAR && next <= AF_FAR) {
			/* not sharper that way at all: the other way, from where it started */
			af->turned = true;
			af->way = -af->way;
			af->worse = 0;
			af_go(device, af, (uint32_t)next);
			return;
		}
		af_fine_begin(device, af);
		return;
	}
	case AF_SWEEP:
		say("focus: sweep: %u %.4f", af->at, sharpness);
		if (sharpness > af->best_sharpness) {
			af->best = af->at;
			af->best_sharpness = sharpness;
			af->worse = 0;
		} else if (sharpness < af->best_sharpness * 0.85f) {
			af->worse++;
		}
		if (af->old) {
			if (af->worse < 2 && (int)af->at + af->way * AF_STEP >= AF_NEAR && (int)af->at + af->way * AF_STEP <= AF_FAR) {
				af_go(device, af, (uint32_t)((int)af->at + af->way * AF_STEP));
				return;
			}
		} else if (af->at != (af->way > 0 ? AF_FAR : AF_NEAR)) {
			/* af6: the whole travel, its far end too (af_go clamps): the sharpest may be right there, as at 10 cm */
			af_go(device, af, af->way > 0 ? af->at + AF_COARSE : af->at > AF_COARSE ? af->at - AF_COARSE : AF_NEAR);
			return;
		}
		af_fine_begin(device, af);
		return;
	case AF_FINE_SEARCH: {
		uint32_t next = af->at + (af->old ? AF_FINE_OLD : AF_FINE);

		if (sharpness > af->best_sharpness) {
			af->best = af->at;
			af->best_sharpness = sharpness;
		}
		next = next == af->start ? next + AF_FINE : next;
		if (next <= af->fine_to) {
			af_go(device, af, next);
			return;
		}
		af->state = AF_SETTLE;
		af_go(device, af, af->best);
		return;
	}
	case AF_SETTLE:
		af->state = AF_WATCH;
		af->sharpness = sharpness;
		af->level = level;
		af->unlike = 0;
		af->ae_waited = 0;
		af->hold = mode == AF_MODE_AUTO;        /* (redfin, B5: one search, and the lens stays) */
		say("focus: the lens at %u after %u moves (sharpness %.3f)%s", af->at, af->steps, sharpness, af->hold ? ", held" : "");
		return;
	case AF_MANUAL:
		return;
	}
}
