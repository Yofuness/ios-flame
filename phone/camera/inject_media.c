/* from ios-redfin (MIT), phone/audio/inject_media.c */
/*
 * libredwood-media.dylib: code of ours inside mediaserverd (see inject.c for how it gets there).
 *
 * redfin: this file is HackiOS's (MIT, LICENSE.HackiOS; hackios-xnu-userland a79ff07 userland/src/inject_media.c), built
 * into RedwoodAudio.driver, which mediaserverd loads as a HAL plug-in: no injection needed. Changed for the
 * iPhone X's iOS 16.7.x on the Pixel 5: no iOS 16.5 addresses are trusted (methods replaced by class and selector;
 * the Metal device list and FigFlashlight's callers left alone), the flashlight is sysctl kern.sweet_torch.
 *
 * mediaserverd is the audio server, and also the camera's: the capture stack of CMCapture lives
 * in the same process. Soon after it starts, that stack "prewarms" the Core Image filters of the
 * portrait mode, for which Core Image wants an OpenGL ES context; there is no GPU, the context
 * has no functions behind it, and the daemon jumps to address 0 -- every time launchd restarts
 * it. Nobody is going to take a portrait here: the prewarming is skipped.
 *
 * What the daemon is wanted for meanwhile is neither sound nor the camera: it is where
 * VideoToolbox decodes for everybody else (the service com.apple.coremedia.decompressionsession),
 * and a HEIC picture is HEVC inside. Without the daemon ImageIO has "no way to decode a HEIC",
 * and the stock wallpaper of iOS 16, whose layers are four such pictures, is its gradient alone.
 * There is no hardware decoder here; VideoToolbox takes its software one (VCPHEVC.videocodec).
 *
 * The flashlight (only where the kernel has one: torch=3, /dev/redwood-torch): Control Center's
 * button and AVFlashlight reach FigFlashlight in here, which asks the capture stack's device vendor
 * (+[BWFigCaptureDeviceVendor sharedCaptureDeviceVendor]) for the camera device and its back
 * camera's stream, and sets the stream's "TorchLevel" (0 to 1). Without a camera there is no
 * vendor (it is nil), every message to it does nothing, and FigFlashlight says it has set the level
 * when nothing has been. So where there is no vendor, FigFlashlight -- and only FigFlashlight,
 * by where it calls from -- is given one of ours, with a device and a stream whose torch level goes
 * to the kernel (see flashlight_*() below). Everybody else still gets none, as before.
 *
 * The camera (only where the kernel has one: cam=5, /dev/redwood-camera): the vendor is made for
 * everybody, with the create function of our capture device (camera_device.c) in place of the
 * one H10ISP.mediacapture would give for the iPhone's ISP; the capture graph is built around it and
 * its frames reach Camera.app's preview (see camera_device.c). Its stream takes the torch level too,
 * so FigFlashlight would need nothing of its own then (not tried yet).
 *
 * What the capture stack asks of hardware this phone has none of is refused here: there is no ISP
 * processing session, so there can be no video stabilization (see video_stabilization_method below).
 *
 * CMCapture is not loaded yet when this library is: the methods are replaced when its image
 * arrives.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <dirent.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include "camera_client.h"

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 1) {
		n = sizeof(line) - 1;
	}
	write(2, line, (size_t)n);
	/* sunfish (f2): also os_log + /var/local/camdev.log */
	line[n] = 0;
	camera_client_log(line);
}

/* (SPI of libdyld; looked up at run time, the SDK's stubs need not have it) */
static const void *(*shared_cache_range)(size_t *length);

#define CACHE_BASE 0x180000000UL                /* where the cache was built to be, before the slide */

static uintptr_t slide;

/* +[BWCoreImageFilterRenderer prewarmPortraitFilterVersion:semanticStyleFilters:] */
static void
prewarm_portrait_filter(__unused id self, __unused SEL selector, __unused int version, __unused BOOL styles)
{
}

/* ---- the camera's device vendor ------------------------------------------------------------------ */

/* sunfish (f2): redfin's flashlight vendor (flashlight_code[] is empty on 16.7.x, so inert) is dropped */
static bool camera;                     /* sunfish (f2): the gate, camera_client_enabled() */
extern int redwood_camera_device_create(const void *allocator, const void *options, void **out);
static IMP original_shared_vendor;

/* +[BWFigCaptureDeviceVendor sharedCaptureDeviceVendor] */
static id
shared_vendor(id self, SEL selector)
{
	id vendor = ((id (*)(id, SEL))original_shared_vendor)(self, selector);
	static id camera_vendor;
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

	if (vendor != NULL) {
		return vendor;
	}
	if (camera) {
		pthread_mutex_lock(&lock);
		if (camera_vendor == NULL) {
			id made = ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("BWFigCaptureDeviceVendor"), sel_registerName("alloc"));

			camera_vendor = ((id (*)(id, SEL, void *))objc_msgSend)(made, sel_registerName("initWithDeviceCreateFunction:"),
			    (void *)redwood_camera_device_create);
			say("redwood: mediaserverd: the camera's device vendor %s\n", camera_vendor ? "made (camera_device.c)" :
			    "CANNOT BE MADE");
		}
		pthread_mutex_unlock(&lock);
		return camera_vendor;
	}
	return NULL;
}

/*
 * Video stabilization, of which this hardware has none: the capture graph's VIS node
 * (-[FigCaptureVISPipeline _newVISNodeWithUpstreamOutput:...]) is made with the ISP's processing
 * session, and this device has no such session, so the node comes back nil, the VIS pipeline with
 * it, the movie file sink's tail pipeline with that, and captureSession_buildGraphWithConfiguration
 * tears the whole graph down -- the preview too, for all that the preview has nothing to do with
 * stabilizing a recording. The connection's stabilization method is where the whole stack reads the
 * wish from (+[FigVideoCaptureConnectionConfiguration videoStabilizationMethods:includeIris:] ->
 * cspc_getVideoStabilizationType -> the connections' FigCaptureVideoStabilizationType), and 0 there
 * means none: the graph is then built without a VIS pipeline anywhere
 * (+[FigCaptureVISPipeline visPipelineIsRequiredForFigCaptureVideoStabilizationType:] says no to
 * every type but 2 and 3), and recordings are as steady as the hand that holds the phone.
 */
static int
video_stabilization_method(id self, SEL selector)
{
	(void)self;
	(void)selector;
	return 0;
}

/*
 * The type the stack acts on. The connection configuration arrives from the application's
 * AVFoundation with the type in it, which is read straight out of the object and not computed from
 * the method above (-[FigCaptureSessionParsedConfiguration _computeVideoStabilizationTypes] only
 * fills in what the client left out), so the wish has to be refused here as well.
 */
static int
video_stabilization_type(id self, SEL selector)
{
	(void)self;
	(void)selector;
	return 0;                       /* kFigCaptureVideoStabilizationType_None */
}

/*
 * The microphone, which the capture stack has none of here. A capture session's sound comes through
 * CoreMedia's own capture audio session -- BWAudioSourceNode's start sets its properties and reads
 * its pull format, all of it through a brokered plug-in of the audio HAL -- and that plug-in is not
 * on this phone: the node's start comes back with 'nope', the HAL's word for "no such thing".
 *
 * Camera.app's session has an audio input in it, because it is a session for photographs and for
 * video both. The graph starts its source nodes late (startDeferredSourceNodesIfNeeded) and takes
 * the first error it gets for the whole graph's, so the session reported AVCaptureSessionRuntimeError
 * to the application, which took the session down and built it again, two seconds later, for ever:
 * the camera's preview came in bursts of five frames with the sensor powering down in between.
 *
 * The node is told that it started. It then produces no sound -- which is all a phone with no
 * capture audio has to offer -- and the rest of the graph, the preview and the photograph, runs.
 * Only that one error is swallowed: a microphone that is there and fails for another reason still
 * takes the session down, as it should. (The kernel's own microphone, adsp=1 mic=4, is a different
 * path: AVAudioRecorder and Voice Memos record through it, and nothing here touches it.)
 */
#define ERROR_NO_SUCH_THING     1852797029      /* 'nope' */

/*
 * redfin (plan B1): a source told that it started has to behave like one that did, or nothing behind it can end.
 * Its output never got a sample nor an end: the movie file sink of a recording waits for audio past the stop
 * time (a video never finalized), and the graph's stop waits for every sink to see end-of-data -- with Live Photo
 * on, its audio converter and movie sink never did, -[BWGraph stop:] gave up after 10 s and mediaserverd was
 * killed in it. So, from the faked start, the node's output is given silence: sample buffers of zeros in the
 * output's own format (the one the graph configured; 44.1 kHz mono 16-bit if it cannot be found), timestamped by
 * the host clock, about every 20 ms; and at the node's stop its output is marked as ended (markEndOfLiveOutput),
 * which is what a real audio source does when its audio unit stops. /private/var/local/camera-no-silence keeps the
 * end and drops the silence. (The preferred way, the HAL plug-in's "Codec" input for the capture audio session,
 * needs to know what start: fails on: the error is said in full once.)
 */
#define SILENT_NODES    4
#define SILENT_CHUNK    1024            /* frames a buffer */

static struct silent {
	id                      node;           /* (not retained: a key; cleared at its stop) */
	id                      output;
	dispatch_queue_t        queue;
	dispatch_source_t       timer;
	CMAudioFormatDescriptionRef format;
	AudioStreamBasicDescription asbd;
	uint64_t                start;          /* mach_absolute_time() of the faked start */
	uint64_t                frames;         /* given so far */
	uint64_t                buffers;
	bool                    ours;           /* the format is our guess, not the graph's */
} silent[SILENT_NODES];
static pthread_mutex_t silent_lock = PTHREAD_MUTEX_INITIALIZER;
static mach_timebase_info_data_t silent_timebase;

static id
send_if(id object, const char *selector)
{
	SEL sel = sel_registerName(selector);

	if (object == nil || !((BOOL (*)(id, SEL, SEL))objc_msgSend)(object, sel_registerName("respondsToSelector:"), sel)) {
		return nil;
	}
	return ((id (*)(id, SEL))objc_msgSend)(object, sel);
}

/* a CoreMedia audio format description, if that is what this is (a plain CF object, never an Objective-C one of the graph's) */
static CMAudioFormatDescriptionRef
as_audio_format(id object)
{
	if (object == nil || strcmp(object_getClassName(object), "__NSCFType") != 0 ||
	    CFGetTypeID((CFTypeRef)object) != CMFormatDescriptionGetTypeID() ||
	    CMFormatDescriptionGetMediaType((CMFormatDescriptionRef)object) != kCMMediaType_Audio) {
		return NULL;
	}
	return (CMAudioFormatDescriptionRef)object;
}

/* what an object answers to, once per class (for finding the format's accessors on the phone) */
static void
say_methods(id object)
{
	static Class said[8];
	Class cls = object != nil ? object_getClass(object) : Nil;
	unsigned count = 0, i;
	char line[220];
	size_t n = 0;

	for (i = 0; cls != Nil && i < 8 && said[i] != Nil; i++) {
		if (said[i] == cls) {
			return;
		}
	}
	if (cls == Nil || i == 8) {
		return;
	}
	said[i] = cls;
	Method *methods = class_copyMethodList(cls, &count);

	say("redwood: mediaserverd: a %s answers to %u methods:\n", class_getName(cls), count);
	for (i = 0; i < count; i++) {
		const char *name = sel_getName(method_getName(methods[i]));

		if (n + strlen(name) + 2 >= sizeof(line) - 1) {
			say("redwood: mediaserverd:   %s\n", line);
			n = 0;
		}
		n += (size_t)snprintf(line + n, sizeof(line) - n, "%s ", name);
	}
	if (n > 0) {
		say("redwood: mediaserverd:   %s\n", line);
	}
	free(methods);
}

/* the format the graph configured on a node's output, or 44.1 kHz mono 16-bit */
static CMAudioFormatDescriptionRef
silent_format(id output, bool *ours)
{
	static const char *const outputs[] = { "liveFormat", "format", "configuredFormat" };
	static const char *const formats[] = { "formatDescription", "audioFormatDescription", "CMFormatDescription" };
	CMAudioFormatDescriptionRef fd = NULL;

	*ours = false;
	for (size_t i = 0; i < sizeof(outputs) / sizeof(outputs[0]) && fd == NULL; i++) {
		id f = send_if(output, outputs[i]);

		if (f == nil) {
			continue;
		}
		say_methods(f);
		if ((fd = as_audio_format(f)) != NULL) {
			break;
		}
		for (size_t j = 0; j < sizeof(formats) / sizeof(formats[0]) && fd == NULL; j++) {
			fd = as_audio_format(send_if(f, formats[j]));
		}
	}
	if (fd != NULL) {
		const AudioStreamBasicDescription *a = CMAudioFormatDescriptionGetStreamBasicDescription(fd);

		if (a != NULL && a->mFormatID == kAudioFormatLinearPCM && a->mSampleRate > 0 && a->mBytesPerFrame > 0) {
			CFRetain(fd);
			return fd;
		}
		say("redwood: mediaserverd: the capture audio's format is not linear PCM: the silence is given in our own\n");
		fd = NULL;
	}
	AudioStreamBasicDescription a = {
		.mSampleRate = 44100, .mFormatID = kAudioFormatLinearPCM,
		.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
		.mBytesPerPacket = 2, .mFramesPerPacket = 1, .mBytesPerFrame = 2, .mChannelsPerFrame = 1, .mBitsPerChannel = 16,
	};

	*ours = true;
	CMAudioFormatDescriptionCreate(NULL, &a, 0, NULL, 0, NULL, NULL, &fd);
	return fd;
}

static void
silent_tick(void *context)
{
	struct silent *sl = context;
	uint64_t now = mach_absolute_time(), due;
	const AudioStreamBasicDescription *a = &sl->asbd;
	double seconds = (double)(now - sl->start) * silent_timebase.numer / silent_timebase.denom / 1e9;
	size_t frame_bytes = a->mBytesPerFrame * ((a->mFormatFlags & kAudioFormatFlagIsNonInterleaved) ? a->mChannelsPerFrame : 1);

	due = (uint64_t)(seconds * a->mSampleRate);
	if (due > sl->frames + (uint64_t)a->mSampleRate) {
		sl->frames = due - SILENT_CHUNK;        /* (more than a second behind: the gap is left, not filled) */
	}
	while (sl->frames + SILENT_CHUNK <= due) {
		CMBlockBufferRef block = NULL;
		CMSampleBufferRef buffer = NULL;
		size_t bytes = SILENT_CHUNK * frame_bytes;
		CMTime pts = CMTimeAdd(CMClockMakeHostTimeFromSystemUnits(sl->start), CMTimeMake((int64_t)sl->frames,
		    (int32_t)a->mSampleRate));

		if (CMBlockBufferCreateWithMemoryBlock(NULL, NULL, bytes, NULL, NULL, 0, bytes, kCMBlockBufferAssureMemoryNowFlag,
		    &block) != 0 || block == NULL) {
			return;
		}
		CMBlockBufferFillDataBytes(0, block, 0, bytes);
		if (CMAudioSampleBufferCreateReadyWithPacketDescriptions(NULL, block, sl->format, SILENT_CHUNK, pts, NULL,
		    &buffer) == 0 && buffer != NULL) {
			((void (*)(id, SEL, CMSampleBufferRef))objc_msgSend)(sl->output, sel_registerName("emitSampleBuffer:"), buffer);
			CFRelease(buffer);
			if (sl->buffers++ == 0) {
				say("redwood: mediaserverd: the capture audio's silence: the first buffer given (%.0f Hz, %u channel(s)%s)\n",
				    a->mSampleRate, a->mChannelsPerFrame, sl->ours ? ", our format" : ", the graph's format");
			}
		}
		CFRelease(block);
		sl->frames += SILENT_CHUNK;
	}
}

static void
nothing(void *context)
{
	(void)context;
}

/* the faked start: the output made live (if the start did not get that far) and silence from now on */
static void
silent_begin(id node)
{
	id output = send_if(node, "output");
	struct silent *sl = NULL;
	bool no_silence = access("/private/var/local/camera-no-silence", F_OK) == 0;

	if (output == nil) {
		say("redwood: mediaserverd: the audio source has no output: nothing to give silence to\n");
		return;
	}
	say_methods(output);
	if (send_if(output, "liveFormat") == nil && send_if(output, "format") != nil &&
	    ((BOOL (*)(id, SEL, SEL))objc_msgSend)(output, sel_registerName("respondsToSelector:"), sel_registerName("makeConfiguredFormatLive"))) {
		((void (*)(id, SEL))objc_msgSend)(output, sel_registerName("makeConfiguredFormatLive"));
		say("redwood: mediaserverd: the audio source's output made live (its start did not get that far)\n");
	}
	if (silent_timebase.denom == 0) {
		mach_timebase_info(&silent_timebase);
	}
	pthread_mutex_lock(&silent_lock);
	for (int i = 0; i < SILENT_NODES && sl == NULL; i++) {
		sl = silent[i].node == node ? &silent[i] : NULL;
	}
	for (int i = 0; i < SILENT_NODES && sl == NULL; i++) {
		sl = silent[i].node == nil ? &silent[i] : NULL;
	}
	if (sl == NULL || sl->timer != NULL) {
		pthread_mutex_unlock(&silent_lock);
		say("redwood: mediaserverd: the audio source's silence %s\n", sl == NULL ? "has no room (too many sources)" : "already runs");
		return;
	}
	sl->node = node;
	sl->output = output;
	sl->frames = sl->buffers = 0;
	sl->start = mach_absolute_time();
	if (sl->format != NULL) {
		CFRelease(sl->format);
	}
	sl->format = silent_format(output, &sl->ours);
	if (sl->format != NULL && !no_silence) {
		sl->asbd = *CMAudioFormatDescriptionGetStreamBasicDescription(sl->format);
		if (sl->queue == NULL) {
			sl->queue = dispatch_queue_create("redwood.capture-audio-silence", DISPATCH_QUEUE_SERIAL);
		}
		sl->timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, sl->queue);
		dispatch_set_context(sl->timer, sl);
		dispatch_source_set_event_handler_f(sl->timer, silent_tick);
		dispatch_source_set_timer(sl->timer, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_MSEC), 20 * NSEC_PER_MSEC,
		    5 * NSEC_PER_MSEC);
		dispatch_resume(sl->timer);
	}
	pthread_mutex_unlock(&silent_lock);
	say("redwood: mediaserverd: the audio source gives %s from now on, and an end at its stop\n",
	    no_silence ? "nothing (camera-no-silence)" : sl->format != NULL ? "silence" : "nothing (no format for silence)");
}

/* the node's stop: no more silence, and its output ended */
static void
silent_end(id node)
{
	struct silent *sl = NULL;
	id output;

	pthread_mutex_lock(&silent_lock);
	for (int i = 0; i < SILENT_NODES && sl == NULL; i++) {
		sl = silent[i].node == node ? &silent[i] : NULL;
	}
	if (sl == NULL) {
		pthread_mutex_unlock(&silent_lock);
		return;                 /* (a source that really started: it ends its own output) */
	}
	if (sl->timer != NULL) {
		dispatch_source_cancel(sl->timer);
		dispatch_sync_f(sl->queue, NULL, nothing);      /* (a tick under way is done) */
		dispatch_release(sl->timer);
		sl->timer = NULL;
	}
	output = sl->output;
	sl->node = nil;
	sl->output = nil;
	pthread_mutex_unlock(&silent_lock);
	if (send_if(output, "liveFormat") == nil && ((BOOL (*)(id, SEL, SEL))objc_msgSend)(output, sel_registerName("respondsToSelector:"),
	    sel_registerName("liveFormat"))) {
		say("redwood: mediaserverd: the audio source stops (%llu silent buffers); its output had ended already\n",
		    (unsigned long long)sl->buffers);
		return;
	}
	if (!((BOOL (*)(id, SEL, SEL))objc_msgSend)(output, sel_registerName("respondsToSelector:"), sel_registerName("markEndOfLiveOutput"))) {
		say("redwood: mediaserverd: the audio source stops, and its output CANNOT BE ENDED (no -markEndOfLiveOutput)\n");
		return;
	}
	((void (*)(id, SEL))objc_msgSend)(output, sel_registerName("markEndOfLiveOutput"));
	say("redwood: mediaserverd: the audio source stops (%llu silent buffers): its output ended\n", (unsigned long long)sl->buffers);
}

static IMP original_audio_source_start, original_audio_source_stop;

static BOOL
audio_source_start(id self, SEL selector, id *error)
{
	BOOL started = ((BOOL (*)(id, SEL, id *))original_audio_source_start)(self, selector, error);
	id made = error != NULL ? *error : NULL;
	static bool said;

	if (started || made == NULL || ((long (*)(id, SEL))objc_msgSend)(made, sel_registerName("code")) != ERROR_NO_SUCH_THING) {
		return started;
	}
	if (!said) {
		id text = send_if(made, "description");

		said = true;
		say("redwood: mediaserverd: there is no capture audio on this phone: the session's audio source gives silence, "
		    "and the camera runs without sound\n");
		say("redwood: mediaserverd: (the audio source's start said: %.180s)\n", text != nil ?
		    ((const char *(*)(id, SEL))objc_msgSend)(text, sel_registerName("UTF8String")) : "?");
	}
	*error = NULL;                  /* (autoreleased by the capture stack: not ours to give back) */
	silent_begin(self);
	return true;
}

static BOOL
audio_source_stop(id self, SEL selector, id *error)
{
	BOOL stopped = original_audio_source_stop != NULL ? ((BOOL (*)(id, SEL, id *))original_audio_source_stop)(self, selector, error) : YES;

	silent_end(self);
	return stopped;
}

/*
 * A photograph's quality prioritization, "speed" at the most. Asked for "balanced" (Camera.app's) or
 * "quality", the capture device takes a scene for a low-light one whenever a frame was exposed at
 * least as long as the sensor's still image stabilization threshold
 * (-[BWFigVideoCaptureDevice _isLowLightSceneUsingFrameStatisticsByPortType:]) -- a threshold from
 * the sensor's tuning in CameraSetup.plist, 0 for a sensor it has no tuning for, as ours -- and
 * then asks the stream for a bracket of four frames for SIS fusion (captureType 4, BWSISNode), which
 * this port has nothing to fuse with: the photograph never came. With "speed" it asks for one frame
 * (-_resolveStillImageCaptureTypeFromStatisticsByPortType:..., captureType 1), which the stream
 * gives (camera_device.c, StillImageCaptureNow).
 */
static IMP original_quality_prioritization;

static int
quality_prioritization(id self, SEL selector)
{
	int asked = ((int (*)(id, SEL))original_quality_prioritization)(self, selector);
	static bool said;

	if (asked > 1 && !said) {
		said = true;
		say("redwood: mediaserverd: a photograph's quality prioritization %d taken as 1 (speed): one frame, no SIS bracket\n",
		    asked);
	}
	return asked > 1 ? 1 : asked;
}

/*
 * A photograph's noise reduction (BWGNRNode, between the still image coordinator and the photo
 * encoder) is a FigSampleBufferProcessor of the iPhone's, which is handed the picture and never
 * hands one back here: the photograph stopped there, with no error, and the request with it (the
 * shutter's second press asked for nothing more). The picture goes past it to the node's output as
 * it came, as with no noise reduction to do.
 */
static void
gnr_render(id self, SEL selector, void *sample_buffer, id input)
{
	id output = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("output"));
	static bool said;

	(void)selector, (void)input;
	if (!said) {
		said = true;
		say("redwood: mediaserverd: a photograph goes past the noise reduction, which has nothing to do it with here\n");
	}
	if (output != nil) {
		((void (*)(id, SEL, void *))objc_msgSend)(output, sel_registerName("emitSampleBuffer:"), sample_buffer);
	}
}

static bool
is_node(id node, const char *name)
{
	return node != nil && strcmp(object_getClassName(node), name) == 0;
}

/*
 * How long a photograph's encoding took, from the picture handed to the photo encoder node to the one
 * it hands on (HEVC in software, see register_hevc_encoder below: the slowest step of a photograph).
 */
static void
time_photo_encoder(id output, CMSampleBufferRef sample_buffer)
{
	CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample_buffer);
	static uint64_t began;
	static mach_timebase_info_data_t timebase;
	id connection, input;

	if (image == NULL || CVPixelBufferGetWidth(image) < 2000) {
		return;                 /* not a photograph */
	}
	if (is_node(((id (*)(id, SEL))objc_msgSend)(output, sel_registerName("node")), "BWPhotoEncoderNode")) {
		if (began != 0) {
			if (timebase.denom == 0) {
				mach_timebase_info(&timebase);
			}
			say("redwood: mediaserverd: a photograph of %zux%zu encoded in %.0f ms\n", CVPixelBufferGetWidth(image),
			    CVPixelBufferGetHeight(image), (double)(mach_absolute_time() - began) * timebase.numer / timebase.denom / 1e6);
		}
		began = 0;
		return;
	}
	connection = ((id (*)(id, SEL))objc_msgSend)(output, sel_registerName("connection"));
	input = connection != nil ? ((id (*)(id, SEL))objc_msgSend)(connection, sel_registerName("input")) : nil;
	if (input != nil && is_node(((id (*)(id, SEL))objc_msgSend)(input, sel_registerName("node")), "BWPhotoEncoderNode")) {
		began = mach_absolute_time();
	}
}

/*
 * A node's output given no sample buffer at all, which no node takes: the session's audio source
 * (-[BWAudioSourceNode _generateSamples]), whose audio unit never started here (see audio_source_start
 * above), fills the silence by a timer with buffers it fails to make and emits them all the same --
 * NULL. Live Photo's staging node added that to an array (-[BWIrisStagingNode renderSampleBuffer:forInput:])
 * and mediaserverd died of the NSInvalidArgumentException, every half a minute with Live Photo on, the
 * preview black for a second each time. A NULL is not passed on.
 */
static IMP original_emit_sample_buffer;

/*
 * redfin (plan B8): why the preview's sink gets 420f when its input asked for BGRA (set_format_requirements below).
 * The first picture that reaches the image queue sink is said, with the node that gives it and what that node's own
 * input was asked for: the node that does not pass the BGRA on is the one to look at. (Only the first 3000 buffers
 * are looked at.)
 */
static const char *
format_list(id requirements)
{
	static char text[160];
	id formats = requirements != nil ? send_if(requirements, "supportedPixelFormats") : nil;
	id description = formats != nil ? send_if(formats, "description") : nil;
	const char *t = description != nil ? ((const char *(*)(id, SEL))objc_msgSend)(description, sel_registerName("UTF8String")) : "(none)";

	snprintf(text, sizeof(text), "%s", t != NULL ? t : "(none)");
	for (char *c = text; *c; c++) {
		*c = *c == '\n' ? ' ' : *c;
	}
	return text;
}

static void
preview_format(id output, CMSampleBufferRef sample_buffer)
{
	static unsigned looked;
	static bool said;
	CVImageBufferRef image;
	id connection, input, node, source_input;

	if (said || looked >= 3000) {
		return;
	}
	looked++;
	connection = send_if(output, "connection");
	input = connection != nil ? send_if(connection, "input") : nil;
	if (input == nil || !is_node(send_if(input, "node"), "BWImageQueueSinkNode") ||
	    (image = CMSampleBufferGetImageBuffer(sample_buffer)) == NULL) {
		return;
	}
	said = true;
	OSType pf = CVPixelBufferGetPixelFormatType(image);
	node = send_if(output, "node");
	source_input = node != nil ? send_if(node, "input") : nil;
	say("redwood: mediaserverd: the preview's sink gets %c%c%c%c %zux%zu from a %s (asked: %s)\n", pf >> 24, pf >> 16 & 255,
	    pf >> 8 & 255, pf & 255, CVPixelBufferGetWidth(image), CVPixelBufferGetHeight(image),
	    node != nil ? object_getClassName(node) : "?", format_list(send_if(input, "formatRequirements")));
	if (source_input != nil) {
		say("redwood: mediaserverd:   whose own input was asked for %s\n", format_list(send_if(source_input, "formatRequirements")));
	}
}

static void
emit_sample_buffer(id self, SEL selector, CMSampleBufferRef sample_buffer)
{
	static bool said;

	if (sample_buffer == NULL) {
		if (!said) {
			said = true;
			say("redwood: mediaserverd: a node's output was given no sample buffer (the silent audio source's): not passed on\n");
		}
		return;
	}
	time_photo_encoder(self, sample_buffer);
	preview_format(self, sample_buffer);
	((void (*)(id, SEL, CMSampleBufferRef))original_emit_sample_buffer)(self, selector, sample_buffer);
}

/*
 * The photograph's encoder. Camera.app asks for HEIC -- HEVC inside a HEIF file, which CMPhoto makes
 * through VideoToolbox -- and VideoToolbox here has no HEVC encoder but the iPhone's hardware ones
 * (com.apple.videotoolbox.videoencoder.hevc and .hevc.gva, behind its AVE): the photo encoder node
 * failed with kVTCouldNotFindVideoEncoderErr (-12908). Apple's software HEVC codec is in the system
 * all the same, VCPHEVC.videocodec, whose decoder VideoToolbox takes here for every HEIC picture (see
 * the top), and there is an encoder in it, HEVCVideoEncoder_CreateInstance, that iOS never registers:
 * the codec's VCPHEVCRegisterEncoder is an empty function. It is registered here, as the codec
 * registers its decoder (VTRegisterVideoDecoderWithInfo, not hardware accelerated).
 *
 * redfin (plan B2): a video is H.264 when Camera.app's formats are "Most Compatible" (and for any app that asks for
 * avc1), and the only H.264 encoders VideoToolbox lists on an iPhone are the AVE's. What the phone has is said once
 * (VideoToolbox's encoder list; the encoder factories the video codec images export, found by walking their export
 * tries), and a software H.264 encoder is registered for 'avc1' if one is found among them. If none is, a
 * recording's video settings that ask for avc1 are given hvc1 instead (recording_video_settings below), so that the
 * video is recorded at all. /private/var/local/camera-no-avc: no H.264 encoder registered (HEVC for every video).
 */
#ifndef LC_DYLD_EXPORTS_TRIE
#define LC_DYLD_EXPORTS_TRIE    0x80000033
#endif

static bool avc_encoder;                /* a software encoder for 'avc1' is there (VideoToolbox's or registered here) */

typedef int (*encoder_create)(uint32_t, CFAllocatorRef, void *);

static uint64_t
uleb(const uint8_t **p, const uint8_t *end)
{
	uint64_t v = 0;
	unsigned shift = 0;

	while (*p < end) {
		uint8_t byte = *(*p)++;

		v |= (uint64_t)(byte & 0x7f) << shift;
		shift += 7;
		if (!(byte & 0x80) || shift > 63) {
			break;
		}
	}
	return v;
}

/* every exported name of the image with `part' in it, by its export trie: said, and each given to found() */
static unsigned
walk_trie(const uint8_t *start, const uint8_t *end, const uint8_t *node, char *name, size_t length, const char *const *parts,
    void (*found)(const char *, void *), void *context, unsigned depth)
{
	const uint8_t *p = node;
	uint64_t terminal;
	unsigned count = 0, children;

	if (node < start || node >= end || depth > 64) {
		return 0;
	}
	terminal = uleb(&p, end);
	if (terminal > 0) {
		for (size_t i = 0; parts[i] != NULL; i++) {
			if (strstr(name, parts[i]) != NULL) {
				found(name, context);
				count++;
				break;
			}
		}
		p += terminal;
	}
	if (p >= end) {
		return count;
	}
	children = *p++;
	for (unsigned i = 0; i < children && p < end; i++) {
		size_t edge = strnlen((const char *)p, (size_t)(end - p));
		uint64_t offset;

		if (length + edge + 1 > 512) {
			return count;
		}
		memcpy(name + length, p, edge);
		name[length + edge] = 0;
		p += edge + 1;
		offset = uleb(&p, end);
		count += walk_trie(start, end, start + offset, name, length + edge, parts, found, context, depth + 1);
		name[length] = 0;
	}
	return count;
}

static void
exports_of(const struct mach_header *header, intptr_t image_slide, const char *const *parts, void (*found)(const char *, void *),
    void *context)
{
	const struct mach_header_64 *mh = (const void *)header;
	const struct load_command *lc = (const void *)(mh + 1);
	uint64_t linkedit_vm = 0, linkedit_off = 0, linkedit_size = 0;
	uint32_t trie_off = 0, trie_size = 0;
	char name[520] = "";

	if (mh->magic != MH_MAGIC_64) {
		return;
	}
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SEGMENT_64 && strcmp(((const struct segment_command_64 *)lc)->segname, "__LINKEDIT") == 0) {
			linkedit_vm = ((const struct segment_command_64 *)lc)->vmaddr;
			linkedit_off = ((const struct segment_command_64 *)lc)->fileoff;
			linkedit_size = ((const struct segment_command_64 *)lc)->vmsize;
		} else if (lc->cmd == LC_DYLD_EXPORTS_TRIE) {
			trie_off = ((const struct linkedit_data_command *)lc)->dataoff;
			trie_size = ((const struct linkedit_data_command *)lc)->datasize;
		} else if ((lc->cmd == LC_DYLD_INFO || lc->cmd == LC_DYLD_INFO_ONLY) && trie_size == 0) {
			trie_off = ((const struct dyld_info_command *)lc)->export_off;
			trie_size = ((const struct dyld_info_command *)lc)->export_size;
		}
	}
	/* (only within the image's __LINKEDIT as mapped: a trie that is not there is not read) */
	if (linkedit_vm == 0 || trie_size == 0 || trie_off < linkedit_off || trie_off - linkedit_off + (uint64_t)trie_size > linkedit_size) {
		return;
	}
	const uint8_t *trie = (const uint8_t *)(uintptr_t)(linkedit_vm + (uint64_t)image_slide + (trie_off - linkedit_off));

	walk_trie(trie, trie + trie_size, trie, name, 0, parts, found, context, 0);
}

struct encoder_search {
	const char *image;
	encoder_create avc;
	char avc_name[160];
	unsigned said;
};

static void
encoder_found(const char *symbol, void *context)
{
	struct encoder_search *e = context;
	const char *name = symbol[0] == '_' ? symbol + 1 : symbol;
	bool avc = (strstr(name, "H264") != NULL || strstr(name, "AVC") != NULL || strstr(name, "Avc") != NULL ||
	    strstr(name, "h264") != NULL) && strstr(name, "Encoder") != NULL && strstr(name, "CreateInstance") != NULL &&
	    strstr(name, "Decoder") == NULL;

	if (e->said++ < 40) {
		const char *base = strrchr(e->image, '/');

		say("redwood: mediaserverd:   %s: %s%s\n", base ? base + 1 : e->image, name, avc ? " (an H.264 encoder?)" : "");
	}
	if (avc && e->avc == NULL) {
		void *handle = dlopen(e->image, RTLD_NOW | RTLD_NOLOAD);

		e->avc = handle != NULL ? (encoder_create)dlsym(handle, name) : NULL;
		snprintf(e->avc_name, sizeof(e->avc_name), "%s", name);
	}
}

/* VideoToolbox's encoders, said once; whether one for `codec' is there that is not hardware accelerated */
static bool
software_encoder_listed(void *vt, uint32_t codec, bool say_all)
{
	int (*copy_list)(CFDictionaryRef, CFArrayRef *) = dlsym(vt, "VTCopyVideoEncoderList");
	CFStringRef *k_codec = dlsym(vt, "kVTVideoEncoderList_CodecType"), *k_id = dlsym(vt, "kVTVideoEncoderList_EncoderID");
	CFStringRef *k_hw = dlsym(vt, "kVTVideoEncoderList_IsHardwareAccelerated");
	CFArrayRef list = NULL;
	bool there = false;

	if (copy_list == NULL || k_codec == NULL || k_id == NULL || copy_list(NULL, &list) != 0 || list == NULL) {
		say("redwood: mediaserverd: VideoToolbox's encoder list cannot be had\n");
		return false;
	}
	for (CFIndex i = 0; i < CFArrayGetCount(list); i++) {
		CFDictionaryRef e = CFArrayGetValueAtIndex(list, i);
		CFNumberRef type = CFDictionaryGetValue(e, *k_codec);
		CFStringRef id = CFDictionaryGetValue(e, *k_id);
		CFBooleanRef hw = k_hw != NULL ? CFDictionaryGetValue(e, *k_hw) : NULL;
		uint32_t t = 0;
		char text[120] = "?";

		if (type != NULL) {
			CFNumberGetValue(type, kCFNumberSInt32Type, &t);
		}
		if (id != NULL) {
			CFStringGetCString(id, text, sizeof(text), kCFStringEncodingUTF8);
		}
		if (say_all) {
			say("redwood: mediaserverd: VideoToolbox encoder %c%c%c%c %s%s\n", t >> 24, t >> 16 & 255, t >> 8 & 255, t & 255, text,
			    hw == kCFBooleanTrue ? " (hardware)" : "");
		}
		there |= t == codec && hw != kCFBooleanTrue;
	}
	CFRelease(list);
	return there;
}

static int
register_encoder(void *vt, uint32_t codec, CFStringRef id, CFStringRef name, void *create)
{
	int (*register_with_info)(uint32_t, CFDictionaryRef, void *) = dlsym(vt, "VTRegisterVideoEncoderWithInfo");
	CFStringRef *id_key = dlsym(vt, "kVTVideoEncoderList_EncoderID");
	CFStringRef *name_key = dlsym(vt, "kVTVideoEncoderList_EncoderName");
	CFStringRef *hardware_key = dlsym(vt, "kVTVideoEncoderList_IsHardwareAccelerated");
	int status;

	if (register_with_info == NULL || id_key == NULL || name_key == NULL || hardware_key == NULL) {
		return -1;
	}
	const void *keys[] = { *id_key, *name_key, *hardware_key };
	const void *values[] = { id, name, kCFBooleanFalse };
	CFDictionaryRef info = CFDictionaryCreate(NULL, keys, values, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	status = register_with_info(codec, info, create);
	CFRelease(info);
	return status;
}

static void
register_hevc_encoder(void)
{
	void *codec = dlopen("/System/Library/VideoCodecs/VCPHEVC.videocodec", RTLD_NOW | RTLD_LOCAL);
	void *vt = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_NOW | RTLD_GLOBAL);
	void *create = codec != NULL ? dlsym(codec, "HEVCVideoEncoder_CreateInstance") : NULL;
	int status;

	if (vt == NULL || create == NULL || dlsym(vt, "VTRegisterVideoEncoderWithInfo") == NULL) {
		say("redwood: mediaserverd: no software HEVC encoder to register (%s)\n", codec == NULL ? "no VCPHEVC.videocodec" :
		    create == NULL ? "no HEVCVideoEncoder_CreateInstance" : "VideoToolbox without VTRegisterVideoEncoderWithInfo");
	} else {
		status = register_encoder(vt, 'hvc1', CFSTR("com.apple.videotoolbox.videoencoder.hevc.vcp"), CFSTR("HEVC (VCPHEVC, software)"),
		    create);
		say("redwood: mediaserverd: the software HEVC encoder of VCPHEVC.videocodec registered for 'hvc1' (%d)\n", status);
	}
	if (vt == NULL) {
		return;
	}
	/* redfin (B2): H.264 */
	if (software_encoder_listed(vt, 'avc1', true)) {
		avc_encoder = true;
		say("redwood: mediaserverd: VideoToolbox has a software H.264 encoder of its own\n");
		return;
	}
	if (access("/private/var/local/camera-no-avc", F_OK) == 0) {
		say("redwood: mediaserverd: no H.264 encoder registered (camera-no-avc): videos asking for avc1 are made HEVC\n");
		return;
	}
	/* the video codecs' images loaded (they are VideoToolbox's plug-ins; nothing of theirs runs at loading) */
	DIR *dir = opendir("/System/Library/VideoCodecs");
	struct dirent *entry;

	while (dir != NULL && (entry = readdir(dir)) != NULL) {
		char path[300];

		if (entry->d_name[0] == '.') {
			continue;
		}
		snprintf(path, sizeof(path), "/System/Library/VideoCodecs/%s", entry->d_name);
		say("redwood: mediaserverd: video codec %s%s\n", entry->d_name, dlopen(path, RTLD_NOW | RTLD_LOCAL) != NULL ? "" : " (not loadable)");
	}
	if (dir != NULL) {
		closedir(dir);
	}
	/* their encoder factories, and VideoToolbox's own */
	static const char *const parts[] = { "Encoder_CreateInstance", "EncoderCreateInstance", "RegisterEncoder", "Encoder_Create", NULL };
	struct encoder_search e = { 0 };

	say("redwood: mediaserverd: encoder factories exported by the video codecs and VideoToolbox:\n");
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *image = _dyld_get_image_name(i);

		if (image != NULL && (strstr(image, "/VideoCodecs/") != NULL || strstr(image, "VideoToolbox.framework") != NULL ||
		    strstr(image, "VCP") != NULL)) {
			e.image = image;
			exports_of(_dyld_get_image_header(i), _dyld_get_image_vmaddr_slide(i), parts, encoder_found, &e);
		}
	}
	if (e.avc == NULL) {
		say("redwood: mediaserverd: no software H.264 encoder found: videos asking for avc1 are made HEVC\n");
		return;
	}
	/* TODO(phase 2): the factory's signature is taken to be VideoToolbox's (as HEVCVideoEncoder_CreateInstance's); check a 1080p30 recording */
	status = register_encoder(vt, 'avc1', CFSTR("com.apple.videotoolbox.videoencoder.h264.redwood"), CFSTR("H.264 (software)"), (void *)e.avc);
	avc_encoder = status == 0 && software_encoder_listed(vt, 'avc1', false);
	say("redwood: mediaserverd: %s registered for 'avc1' (%d)%s\n", e.avc_name, status, avc_encoder ? "" :
	    "; NOT LISTED after: videos asking for avc1 are made HEVC");
}

/*
 * redfin (B2): a recording's video settings: avc1 (H.264) made hvc1 (HEVC) when there is no H.264 encoder here, without
 * the H.264-only compression properties (profile level, entropy mode), which the HEVC encoder would refuse.
 */
static IMP original_video_settings;

static id
recording_video_settings(id self, SEL selector)
{
	id settings = ((id (*)(id, SEL))original_video_settings)(self, selector);
	CFTypeRef codec, properties;
	CFMutableDictionaryRef changed;
	static bool said;

	if (avc_encoder || settings == nil || CFGetTypeID((CFTypeRef)settings) != CFDictionaryGetTypeID() ||
	    (codec = CFDictionaryGetValue((CFDictionaryRef)settings, CFSTR("AVVideoCodecKey"))) == NULL ||
	    CFGetTypeID(codec) != CFStringGetTypeID() || !CFEqual(codec, CFSTR("avc1"))) {
		return settings;
	}
	changed = CFDictionaryCreateMutableCopy(NULL, 0, (CFDictionaryRef)settings);
	CFDictionarySetValue(changed, CFSTR("AVVideoCodecKey"), CFSTR("hvc1"));
	properties = CFDictionaryGetValue(changed, CFSTR("AVVideoCompressionPropertiesKey"));
	if (properties != NULL && CFGetTypeID(properties) == CFDictionaryGetTypeID()) {
		CFMutableDictionaryRef p = CFDictionaryCreateMutableCopy(NULL, 0, properties);

		CFDictionaryRemoveValue(p, CFSTR("ProfileLevel"));
		CFDictionaryRemoveValue(p, CFSTR("H264EntropyMode"));
		CFDictionarySetValue(changed, CFSTR("AVVideoCompressionPropertiesKey"), p);
		CFRelease(p);
	}
	if (!said) {
		said = true;
		say("redwood: mediaserverd: a recording asked for H.264, for which there is no encoder here: made HEVC\n");
	}
	return (id)CFAutorelease(changed);
}

/*
 * Camera.app's filter thumbnails (the grid of the photo filters, each filter applied to the live
 * picture). -[BWMultiFilterThumbnailNode prepareForCurrentConfigurationToBecomeLive] asks its filter
 * renderer for -filterDescriptor and writes into what it returns; the renderer is a Metal one and this
 * process has no Metal device (MTL2 lists the processes that do), so it is nil, the descriptor NULL and
 * mediaserverd died (SIGSEGV at 0x8) as soon as the preview's first picture was given (r18e, iOS
 * 16.7.x). The node is made ready as any node is (its superclass's method) and passes nothing on: no
 * thumbnails, the preview and photographs unaffected.
 */
static void
thumbnail_prepare(id self, SEL selector)
{
	Class super_class = class_getSuperclass(objc_getClass("BWMultiFilterThumbnailNode"));
	IMP ready = super_class != Nil ? class_getMethodImplementation(super_class, selector) : NULL;
	static bool said;

	if (!said) {
		said = true;
		say("redwood: mediaserverd: the filter thumbnails are not made (their renderer wants Metal): the node made ready as a %s\n",
		    super_class != Nil ? class_getName(super_class) : "(none)");
	}
	if (ready != NULL) {
		((void (*)(id, SEL))ready)(self, selector);
	}
}

static void
thumbnail_render(id self, SEL selector, void *sample_buffer, id input)
{
	(void)self, (void)selector, (void)sample_buffer, (void)input;
}

/*
 * The preview in BGRA. The preview's sink (BWImageQueueSinkNode, Camera.app's CAImageQueue) takes 420f/420v,
 * which backboardd's Core Animation samples as one MTLPixelFormatYCBCR8_420_2P texture: a format Turnip (and
 * SweetMetal2) does not have, so every preview frame failed ("Failed to allocate MTLTexture from IOSurface") and
 * the preview stayed black. Its input is asked for BGRA instead; VideoToolbox's software transfer (vt_test) makes it
 * where the graph rotates the picture anyway. /private/var/local/camera-preview-yuv (any content) turns this off.
 */
static IMP original_set_format_requirements;

static void
set_format_requirements(id self, SEL selector, id requirements)
{
	static int wanted = -1;
	id node = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("node"));

	if (wanted < 0) {
		wanted = access("/private/var/local/camera-preview-yuv", F_OK) != 0;
	}
	if (wanted && requirements != nil && is_node(node, "BWImageQueueSinkNode") &&
	    strcmp(object_getClassName(requirements), "BWVideoFormatRequirements") == 0) {
		id formats = ((id (*)(id, SEL))objc_msgSend)(requirements, sel_registerName("supportedPixelFormats"));
		id description = formats != nil ? ((id (*)(id, SEL))objc_msgSend)(formats, sel_registerName("description")) : nil;
		const char *text = description != nil ? ((const char *(*)(id, SEL))objc_msgSend)(description, sel_registerName("UTF8String")) : "(none)";
		CFNumberRef bgra = CFNumberCreate(NULL, kCFNumberSInt32Type, &(int32_t){ 0x42475241 /* BGRA */ });
		CFArrayRef only = CFArrayCreate(NULL, (const void **)&bgra, 1, &kCFTypeArrayCallBacks);

		say("redwood: mediaserverd: the preview's sink asked for %s: given BGRA\n", text);
		((void (*)(id, SEL, id))objc_msgSend)(requirements, sel_registerName("setSupportedPixelFormats:"), (id)only);
		((void (*)(id, SEL, id))objc_msgSend)(requirements, sel_registerName("setPreferredPixelFormats:"), (id)only);
		CFRelease(only);
		CFRelease(bgra);
		((void (*)(id, SEL, id))original_set_format_requirements)(self, selector, requirements);
		/* (redfin, B8: what the input holds after it took them) */
		say("redwood: mediaserverd:   the sink's input now asks for %s\n", format_list(send_if(self, "formatRequirements")));
		return;
	}
	((void (*)(id, SEL, id))original_set_format_requirements)(self, selector, requirements);
}

/*
 * The class methods replaced, each as soon as its class exists. `expected` is the unslid address
 * of the code in iOS 16.5 (20F66): a method that is not what it was is left alone. `original`, where
 * there is one, keeps what was there (the vendor's: given as it is wherever there is one, and where
 * there is none, ours only with the kernel's camera or flashlight).
 */
static struct replacement {
	const char *class_name, *selector;
	uintptr_t expected;
	IMP replacement, *original;
	bool instance;                  /* an instance method, not a class method */
	bool done;
	bool inherited_too;             /* redfin: a method the class only inherits gets an override of its own (original: the superclass's) */
} replacements[] = {
	{ "BWCoreImageFilterRenderer", "prewarmPortraitFilterVersion:semanticStyleFilters:", 0 /* 16.5: 0x1a2115c0cUL */,
	  (IMP)prewarm_portrait_filter, NULL, false, false },
	{ "BWFigCaptureDeviceVendor", "sharedCaptureDeviceVendor", 0 /* 16.5: 0x1a1ef0d04UL */,
	  (IMP)shared_vendor, &original_shared_vendor, false, false },
	{ "BWAudioSourceNode", "start:", 0 /* 16.5: 0x1a1eefa18UL */, (IMP)audio_source_start, &original_audio_source_start, true, false },
	{ "FigVideoCaptureConnectionConfiguration", "videoStabilizationMethod", 0 /* 16.5: 0x1a1e99ef8UL */,
	  (IMP)video_stabilization_method, NULL, true, false },
	{ "FigVideoCaptureConnectionConfiguration", "videoStabilizationType", 0 /* 16.5: 0x1a20d8f48UL */,
	  (IMP)video_stabilization_type, NULL, true, false },
	{ "FigCaptureStillImageSettings", "qualityPrioritization", 0 /* 16.5: 0x1a1f533dcUL */,
	  (IMP)quality_prioritization, &original_quality_prioritization, true, false },
	{ "BWGNRNode", "renderSampleBuffer:forInput:", 0 /* 16.5: 0x1a1ec83b8UL */, (IMP)gnr_render, NULL, true, false },
	{ "BWNodeOutput", "emitSampleBuffer:", 0 /* 16.5: 0x1a1ea37e0UL */, (IMP)emit_sample_buffer, &original_emit_sample_buffer, true, false },
	/* (redfin, 16.7.x: not in HackiOS's list) */
	{ "BWMultiFilterThumbnailNode", "prepareForCurrentConfigurationToBecomeLive", 0, (IMP)thumbnail_prepare, NULL, true, false },
	{ "BWMultiFilterThumbnailNode", "renderSampleBuffer:forInput:", 0, (IMP)thumbnail_render, NULL, true, false },
	{ "BWNodeInput", "setFormatRequirements:", 0, (IMP)set_format_requirements, &original_set_format_requirements, true, false },
	/* (redfin, plan B1/B2) */
	{ "BWAudioSourceNode", "stop:", 0, (IMP)audio_source_stop, &original_audio_source_stop, true, false, true },
	{ "FigCaptureMovieFileRecordingSettings", "videoSettings", 0, (IMP)recording_video_settings, &original_video_settings, true, false,
	  true },
};

/* sunfish (f2): redfin's Metal device removal (MTL_DEVICE_ARRAY, always off on 16.7.x) is dropped */

static void
image_added(__unused const struct mach_header *header, __unused intptr_t image_slide)
{
	/* before anything of the image that was just added runs, and this is the only place in this
	 * library that is told about a new image at all */
	for (size_t i = 0; i < sizeof(replacements) / sizeof(replacements[0]); i++) {
		struct replacement *r = &replacements[i];
		Class cls;
		Method method;
		IMP original;

		if (r->done || (cls = objc_getClass(r->class_name)) == NULL) {
			continue;
		}
		r->done = true;
		method = r->instance ? class_getInstanceMethod(cls, sel_registerName(r->selector)) :
		    class_getClassMethod(cls, sel_registerName(r->selector));
		original = method ? method_getImplementation(method) : NULL;
		/* redfin: a method the class only inherits is not replaced (that would replace its superclass's) */
		if (method != NULL && r->instance && class_getSuperclass(cls) != Nil &&
		    class_getInstanceMethod(class_getSuperclass(cls), sel_registerName(r->selector)) == method) {
			if (r->inherited_too && r->original != NULL &&
			    class_addMethod(cls, sel_registerName(r->selector), r->replacement, method_getTypeEncoding(method))) {
				*r->original = original;
				say("redwood: mediaserverd: -[%s %s] is its superclass's (at %#lx unslid): given one of its own\n", r->class_name,
				    r->selector, (uintptr_t)original - slide);
				continue;
			}
			say("redwood: -[%s %s] is its superclass's: left alone\n", r->class_name, r->selector);
			continue;
		}
		/*
		 * redfin: `expected' is iOS 16.5's (HackiOS's phone); on the iPhone X's 16.7.x it is 0 here, unknown: the
		 * method is replaced by its class and selector alone, and where its code lies is said, so that it can be
		 * put here once it is known to be the same method.
		 */
		if (original == NULL || (r->expected != 0 && (uintptr_t)original != r->expected + slide)) {
			say("redwood: %c[%s %s]: found %#lx, expected %#lx; left alone\n", r->instance ? '-' : '+', r->class_name,
			    r->selector,
			    original ? (uintptr_t)original - slide : 0, r->expected);
			continue;
		}
		if (r->original != NULL) {
			*r->original = original;
		}
		method_setImplementation(method, r->replacement);
		say("redwood: mediaserverd: %c[%s %s] (at %#lx unslid) replaced\n", r->instance ? '-' : '+', r->class_name, r->selector,
		    (uintptr_t)original - slide);
	}
}

__attribute__((constructor))
static void
redwood_media_init(void)
{
	const char *name = getprogname();
	size_t length = 0;

	if (name == NULL || strcmp(name, "mediaserverd") != 0) {
		return;
	}
	/*
	 * sunfish (f2): the gate, once at load: /var/local/camdev.on or boot-arg sweetcamdev=1 (camera_client.c). Without
	 * it nothing of the capture stack is touched and the plug-in is audio only, as before.
	 */
	camera = camera_client_enabled();
	if (!camera) {
		return;
	}
	shared_cache_range = (const void *(*)(size_t *))dlsym(RTLD_DEFAULT, "_dyld_get_shared_cache_range");
	slide = shared_cache_range != NULL ? (uintptr_t)shared_cache_range(&length) - CACHE_BASE : 0;
	say("redwood: mediaserverd: the camera is on (camd, %s); slide %#lx\n", CAMERA_CLIENT_GATE, slide);
	register_hevc_encoder();
	_dyld_register_func_for_add_image(image_added);         /* also called for every image already there */
}
