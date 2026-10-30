/* from ios-redfin (MIT), phone/audio/camera_device.c */
/*
 * The phone's cameras as iOS's capture stack wants them: a FigCaptureDevice with two FigCaptureStreams,
 * the wide camera as the back camera and the front one as the front camera, made for mediaserverd's
 * BWFigCaptureDeviceVendor in place of the device Apple's H10ISP.mediacapture makes of the iPhone's ISP
 * (part of libredwood-media.dylib; inject_media.c gives the vendor our device's create function when
 * the kernel has a camera: cam=5).
 *
 * Both are CoreMedia "derived objects" (CMDerivedObjectCreate): a table of functions for the base
 * class (properties) and one for the device or stream class, as H10ISP's are laid out in iOS 16.5:
 *   device  RequestStreams, RelinquishStreams (and a processing session and Pearl calibration, not here)
 *   stream  Start, Stop, AddAttachmentsToSampleBuffer
 * A stream's frames go to the capture graph through the blocks it is given in its property
 * "VideoOutputHandlers" ({ output: { PixelBufferHandler: ^(CVPixelBufferRef, CMTime) } }), for the
 * outputs "VideoOutputConfigurations" describes (the graph's own buffer pools) and "VideoOutputsEnabled"
 * switches on. A frame: read from the kernel's /dev/redwood-camera (RAW10), made into a picture by
 * camera_isp.c (exposure, white balance kept to the sensor's reference lights, the colour matrix and
 * lens shading of the camera's own tuning, gamma, zoom), one pixel buffer for each output, with the
 * metadata the graph keeps its frame statistics and its zoom by. The kernel gives one sensor's frames
 * at a time: a stream that starts stops the other's frames and has the kernel change sensors.
 *
 * The formats the streams offer are named as the iPhone X's cameras' are in CMCapture's
 * D22/AVCaptureSession.plist (FigCaptureStreamFormats, by the sensor sizes and frame rates the plist
 * matches them by), so that their AVCaptureDevice formats and session presets resolve: the pictures
 * are scaled from the sensor's frames to whatever size is asked for.
 *
 * The shutter: StillImageCaptureNow asks for one frame of the whole sensor on the StillImage output
 * (the time machine's TimeMachineSuspendNow/ResumeNow are taken and ignored). Focus, where there is
 * a lens motor (the wide camera's): by contrast, continuous, and on the window a tap gives (af_frame()).
 *
 * Everything asked of the device and the streams that is not answered here is said in mediaserverd's
 * log ("redwood: camera: ..."), once per property.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include "camera_isp.h"
#include "camera_client.h"

void redwood_camera_say(const char *format, ...) __attribute__((format(printf, 1, 2)));

void
redwood_camera_say(const char *format, ...)
{
	char line[400];
	va_list ap;
	int n = snprintf(line, sizeof(line), "redwood: camera: ");

	va_start(ap, format);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n, format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2) {
		n = sizeof(line) - 2;
	}
	line[n++] = '\n';
	write(2, line, (size_t)n);
	/* sunfish (f2): also os_log + /var/local/camdev.log (mediaserverd's stderr is not kept) */
	line[n] = 0;
	camera_client_log(line);
}
#define say redwood_camera_say

/* ---- CoreMedia's derived objects, as iOS 16.5 lays them out ---------------------------------------- */

typedef struct OpaqueCMBaseObject *CMBaseObjectRef;
typedef const void *CMBaseClassID;

struct base_class {
	uint64_t        version;
	uint64_t        storage_size;
	Boolean         (*equal)(CMBaseObjectRef, CMBaseObjectRef);
	OSStatus        (*invalidate)(CMBaseObjectRef);
	void            (*finalize)(CMBaseObjectRef);
	CFStringRef     (*copy_debug_description)(CMBaseObjectRef);
	OSStatus        (*copy_property)(CMBaseObjectRef, CFStringRef, CFAllocatorRef, void *);
	OSStatus        (*set_property)(CMBaseObjectRef, CFStringRef, CFTypeRef);
	OSStatus        (*notification_barrier)(CMBaseObjectRef);
	const void *    protocols;
	const void *    reserved;
};
struct device_class {
	uint64_t        version;                /* 3 */
	void *          processing_session_create;
	OSStatus        (*request_streams)(CMBaseObjectRef, CFArrayRef, CFDictionaryRef);
	OSStatus        (*relinquish_streams)(CMBaseObjectRef, CFArrayRef, CFDictionaryRef);
	void *          pearl_calibration;
};
struct stream_class {
	uint64_t        version;                /* 2 */
	OSStatus        (*start)(CMBaseObjectRef);
	OSStatus        (*stop)(CMBaseObjectRef);
	OSStatus        (*add_attachments)(CMBaseObjectRef, CMSampleBufferRef, CFDictionaryRef);
};
struct vtable {
	const void *    reserved;
	const struct base_class *base;
	const void *    derived;
};

#define ERR_PARAM               (-12780)
#define ERR_NOT_AVAILABLE       (-12783)
#define ERR_UNSUPPORTED         (-12782)

static OSStatus (*derived_create)(CFAllocatorRef, const struct vtable *, CMBaseClassID, CMBaseObjectRef *);
static void *(*derived_storage)(CMBaseObjectRef);
static CMBaseClassID (*device_class_id)(void);
static CMBaseClassID (*stream_class_id)(void);
static CFStringRef port_back;           /* kFigCapturePortType_BackFacingCamera */
static CFStringRef port_front;          /* kFigCapturePortType_FrontFacingCamera */
static CFStringRef port_ultra_wide;     /* kFigCapturePortType_BackFacingSuperWideCamera (redfin: the ultra-wide camera, B3) */

/*
 * The frames' metadata: what H10ISP's stream attaches to every frame (a dictionary under
 * kFigCaptureSampleBufferAttachmentKey_MetadataDictionary, through the stream's
 * AddAttachmentsToSampleBuffer when the graph defers it), and what the capture device keeps its frame
 * statistics of. -[BWFrameStatisticsByPortType updateWithFrameMetadata:updateFocusDistance:] counts
 * a frame only for a buffer that has it, and -[BWFigVideoCaptureDevice
 * _stillImageCaptureInProgressSubstateForFrameStatisticsByPortType:...] puts a standard photograph
 * off until more than four frames have been counted (then only while AFStatus is 2, a coarse focus
 * scan): without it the shutter's capture was put off on every frame (the retry in
 * _updateStateUsingVideoSampleBuffer:fromCaptureStream:), for ever, and StillImageCaptureNow never
 * came. The dictionary goes on the pixel buffer too, and mutable, as the ISP's does: FigImageControl_Focus
 * (the device's camera controls, on every frame once the sample buffer has metadata) reads it from
 * the pixel buffer with no check that it is there -- CFDictionaryGetValue(NULL, ...): mediaserverd
 * died of SIGSEGV -- and adds its own keys to it. The keys are CoreMedia's; the values come from the
 * frame's header and our ISP's levels, in Apple's units where they are known (the exposure in seconds,
 * gains with 256 for 1x); lux and SNR are rough.
 */
enum {
	MD_PORT, MD_PTS, MD_EXPOSURE, MD_AGC, MD_ISP_DGAIN, MD_SENSOR_DGAIN, MD_ISO_BASE, MD_FRAME_RATE, MD_LUX, MD_SNR,
	MD_AF_STATUS, MD_FOCUSING, MD_AE_STABLE, MD_AE_LOCKED, MD_AWB_STABLE, MD_AWB_LOCKED, MD_AF_LOCKED, MD_ZOOM,
	/* redfin (B5, B7): the lens, the white balance's gains and colour temperature (names and units to be checked on the phone) */
	MD_FOCUS_POSITION, MD_LENS_POSITION, MD_AWB_R, MD_AWB_G, MD_AWB_B, MD_AWB_CCT, MD_CCT, MD_KEYS
};
static const char *const md_names[MD_KEYS] = {
	"kFigCaptureStreamMetadata_PortType", "kFigCaptureSampleBufferAttachmentKey_OriginalPresentationTimeStamp",
	"kFigCaptureStreamMetadata_ExposureTime", "kFigCaptureStreamMetadata_AGC", "kFigCaptureStreamMetadata_ispDGain",
	"kFigCaptureStreamMetadata_sensorDGain", "kFigCaptureStreamMetadata_ISOBase", "kFigCaptureStreamMetadata_CurrentFrameRate",
	"kFigCaptureStreamMetadata_LuxLevel", "kFigCaptureStreamMetadata_NormalizedSNR", "kFigCaptureStreamMetadata_AFStatus",
	"kFigCaptureStreamMetadata_FocusingMethod", "kFigCaptureStreamMetadata_AEStable", "kFigCaptureStreamMetadata_AELocked",
	"kFigCaptureStreamMetadata_AWBStable", "kFigCaptureStreamMetadata_AWBLocked", "kFigCaptureStreamMetadata_AFLocked",
	"kFigCaptureStreamMetadata_ZoomFactor",
	"kFigCaptureStreamMetadata_FocusPosition", "kFigCaptureStreamMetadata_LensPosition", "kFigCaptureStreamMetadata_AWBRGain",
	"kFigCaptureStreamMetadata_AWBGGain", "kFigCaptureStreamMetadata_AWBBGain", "kFigCaptureStreamMetadata_AWBCCT",
	"kFigCaptureStreamMetadata_ColorTemperature",
};
static CFStringRef md_key[MD_KEYS];
static CFStringRef md_attachment;       /* kFigCaptureSampleBufferAttachmentKey_MetadataDictionary */

static bool
find_functions(void)
{
	CFStringRef *port;

	derived_create = dlsym(RTLD_DEFAULT, "CMDerivedObjectCreate");
	derived_storage = dlsym(RTLD_DEFAULT, "CMBaseObjectGetDerivedStorage");
	device_class_id = dlsym(RTLD_DEFAULT, "FigCaptureDeviceGetClassID");
	stream_class_id = dlsym(RTLD_DEFAULT, "FigCaptureStreamGetClassID");
	port = dlsym(RTLD_DEFAULT, "kFigCapturePortType_BackFacingCamera");
	port_back = port != NULL ? *port : NULL;
	port = dlsym(RTLD_DEFAULT, "kFigCapturePortType_FrontFacingCamera");
	port_front = port != NULL ? *port : NULL;
	port = dlsym(RTLD_DEFAULT, "kFigCapturePortType_BackFacingSuperWideCamera");
	port_ultra_wide = port != NULL ? *port : NULL;
	port = dlsym(RTLD_DEFAULT, "kFigCaptureSampleBufferAttachmentKey_MetadataDictionary");
	md_attachment = port != NULL ? *port : NULL;
	for (int i = 0; i < MD_KEYS; i++) {
		port = dlsym(RTLD_DEFAULT, md_names[i]);
		md_key[i] = port != NULL ? *port : NULL;
		if (md_key[i] == NULL) {
			say("%s is not there: the frames' metadata go without it", md_names[i]);
		}
	}
	if (md_attachment == NULL || md_key[MD_PORT] == NULL) {
		say("kFigCaptureSampleBufferAttachmentKey_MetadataDictionary or the port's key is not there: frames without "
		    "metadata, and the shutter will wait for ever");
		md_attachment = NULL;
	}
	if (derived_create == NULL || derived_storage == NULL || device_class_id == NULL || stream_class_id == NULL || port_back == NULL ||
	    port_front == NULL) {
		say("CoreMedia's or CMCapture's functions are not there: no camera");
		return false;
	}
	return true;
}

/* ---- what is asked and not answered: said once per name ------------------------------------------- */

static CFMutableSetRef said_names;
static pthread_mutex_t said_lock = PTHREAD_MUTEX_INITIALIZER;

static void
say_value(const char *what, CFStringRef key, CFTypeRef value)
{
	char name[160] = "?", text[300] = "";

	CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
	if (value != NULL) {
		CFStringRef d = CFCopyDescription(value);

		CFStringGetCString(d, text, sizeof(text), kCFStringEncodingUTF8);
		CFRelease(d);
		for (char *c = text; *c; c++) {
			*c = *c == '\n' ? ' ' : *c;
		}
	}
	say("%s %s%s%s", what, name, value ? " = " : "", text);
}

static void
say_once(const char *what, CFStringRef key, CFTypeRef value)
{
	CFStringRef tag = CFStringCreateWithFormat(NULL, NULL, CFSTR("%s %@"), what, key);

	pthread_mutex_lock(&said_lock);
	if (said_names == NULL) {
		said_names = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	bool first = !CFSetContainsValue(said_names, tag);

	CFSetAddValue(said_names, tag);
	pthread_mutex_unlock(&said_lock);
	CFRelease(tag);
	if (first) {
		say_value(what, key, value);
	}
}

/* ---- numbers and dictionaries ---------------------------------------------------------------------- */

static CFNumberRef
number_i(int64_t v)
{
	return CFNumberCreate(NULL, kCFNumberSInt64Type, &v);
}

static CFNumberRef
number_f(double v)
{
	return CFNumberCreate(NULL, kCFNumberDoubleType, &v);
}

static void
put(CFMutableDictionaryRef d, const char *key, CFTypeRef value)
{
	CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);

	CFDictionarySetValue(d, k, value);
	CFRelease(k);
	CFRelease(value);
}

static CFDictionaryRef
new_dict(void)
{
	return CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
}

static bool
key_is(CFStringRef key, const char *name)
{
	char buffer[128];

	return CFStringGetCString(key, buffer, sizeof(buffer), kCFStringEncodingUTF8) && strcmp(buffer, name) == 0;
}

/* ---- what the device and the stream say they have ---------------------------------------------------- */

/*
 * The capture stack asks only for the properties an object lists in its "SupportedPropertiesDictionary"
 * ({ name: { PropertyType, ReadWriteStatus } }). Listed here: what is answered, and what is taken
 * (and, for now, mostly not acted on) without failing the graph that sets it.
 */
struct prop {
	const char *name, *type, *access;
};

static const struct prop stream_props[] = {
	{ "PortType", "String", "ReadOnly" }, { "UniqueID", "String", "ReadOnly" }, { "NonLocalizedName", "String", "ReadOnly" },
	{ "SupportedFormatsArray", "Collection", "ReadOnly" }, { "FocalLength", "Number", "ReadOnly" },
	{ "LensFNumber", "Number", "ReadOnly" }, { "PixelSize", "Number", "ReadOnly" },
	{ "MaxZoomFactor", "Number", "ReadOnly" }, { "SupportedColorSpaces", "Collection", "ReadOnly" },
	{ "HasAutoFocusPositionSensor", "Boolean", "ReadOnly" }, { "Clock", "Clock", "ReadOnly" },
	{ "FormatIndex", "Number", "ReadWrite" }, { "MinimumFrameRate", "Number", "ReadWrite" }, { "MaximumFrameRate", "Number", "ReadWrite" },
	{ "MaximumAllowedFrameRate", "Number", "ReadWrite" }, { "VideoOutputConfigurations", "Collection", "ReadWrite" },
	{ "VideoOutputsEnabled", "Collection", "ReadWrite" }, { "VideoOutputHandlers", "Collection", "WriteOnly" },
	{ "DeferAdditionOfAttachments", "Boolean", "ReadWrite" }, { "TorchLevel", "Number", "ReadWrite" },
	{ "HighCurrentTorchEnabled", "Boolean", "ReadWrite" }, { "ColorSpace", "Number", "ReadWrite" }, { "ZoomFactor", "Number", "ReadWrite" },
	{ "ExposureBias", "Number", "ReadWrite" }, { "DeviceOrientation", "Number", "ReadWrite" }, { "FlashMode", "Number", "ReadWrite" },
	{ "StillImageCaptureNow", "Collection", "WriteOnly" }, { "LockAENow", "Boolean", "WriteOnly" }, { "UnlockAENow", "Boolean", "WriteOnly" },
	{ "LockAWBNow", "Boolean", "WriteOnly" }, { "UnlockAWBNow", "Boolean", "WriteOnly" }, { "MaxIntegrationTime", "Number", "ReadWrite" },
	{ "AEMaxGain", "Number", "ReadWrite" }, { "AEMinGain", "Number", "ReadWrite" }, { "FaceDetectionEnabled", "Boolean", "ReadWrite" },
	{ "VideoRecordingInProgress", "Boolean", "ReadWrite" }, { "MemoryPoolID", "Number", "ReadWrite" },
	{ "ChromaNoiseReductionEnabled", "Boolean", "ReadWrite" }, { "TemporalNoiseReductionEnabled", "Boolean", "ReadWrite" },
	{ "MultipleOutputSupportEnabled", "Boolean", "ReadWrite" }, { "VideoOutputsColorInfo", "Collection", "ReadWrite" },
	{ "RegionOfInterestForCameraControls", "Collection", "ReadWrite" }, { "AEMeteringMode", "Number", "ReadWrite" },
	{ "MaxFrameCoalescingDuration", "Number", "ReadWrite" }, { "LimitAERange", "Boolean", "ReadWrite" },
	{ "TimeMachineSuspendNow", "Boolean", "WriteOnly" }, { "TimeMachineResumeNow", "Boolean", "WriteOnly" },
	/* focus (the wide camera's lens motor; see af_frame()) */
	{ "AutoFocusNow", "Collection", "WriteOnly" }, { "ContinuousAutoFocusNow", "Collection", "WriteOnly" },
	{ "FocusNow", "Collection", "WriteOnly" }, { "PeakTrackingFocusNow", "Collection", "WriteOnly" },
	{ "LockAFNow", "Boolean", "WriteOnly" }, { "ManualFocusNow", "Collection", "WriteOnly" },
	{ "AFWindowParams", "Collection", "ReadWrite" }, { "AutoFocusMode", "Number", "ReadWrite" },
	/*
	 * redfin (plan B4/B7): the window a tap meters on, and what a custom exposure or white balance may be set by --
	 * the capture stack sets only what is listed, and these names are guesses (H10ISP's are not known here): what it
	 * really sets is said once ("stream: set (not used)"), for the phone to show
	 */
	{ "AEWindowParams", "Collection", "ReadWrite" }, { "ExposureMode", "Number", "ReadWrite" },
	{ "ManualExposureNow", "Collection", "WriteOnly" }, { "IntegrationTimeAndGain", "Collection", "ReadWrite" },
	{ "ManualExposureParams", "Collection", "ReadWrite" }, { "ExposureDurationAndISO", "Collection", "ReadWrite" },
	{ "ManualAWBGainsNow", "Collection", "WriteOnly" }, { "WhiteBalanceGains", "Collection", "ReadWrite" },
	{ "ManualWhiteBalanceGains", "Collection", "ReadWrite" },
}, device_props[] = {
	{ "StreamArray", "Collection", "ReadOnly" }, { "Clock", "Clock", "ReadOnly" }, { "ClientIsActive", "Boolean", "ReadWrite" },
	{ "SupportedSynchronizedStreamsGroups", "Collection", "ReadOnly" }, { "MemoryPoolID", "Number", "ReadWrite" },
};

static CFDictionaryRef
supported(const struct prop *p, size_t n)
{
	CFMutableDictionaryRef all = (CFMutableDictionaryRef)new_dict();

	for (size_t i = 0; i < n; i++) {
		CFMutableDictionaryRef one = (CFMutableDictionaryRef)new_dict();

		put(one, "PropertyType", CFStringCreateWithCString(NULL, p[i].type, kCFStringEncodingUTF8));
		put(one, "ReadWriteStatus", CFStringCreateWithCString(NULL, p[i].access, kCFStringEncodingUTF8));
		put(all, p[i].name, one);
	}
	return all;
}

/* ---- the formats ----------------------------------------------------------------------------------- */

#define PF_420F         0x34323066      /* '420f' */
#define PF_420V         0x34323076      /* '420v' */
#define PF_BGRA         0x42475241      /* 'BGRA' */

static CFDictionaryRef
output_info(int width, int height)
{
	CFMutableDictionaryRef o = (CFMutableDictionaryRef)new_dict();
	CFNumberRef pf[3] = { number_i(PF_420F), number_i(PF_420V), number_i(PF_BGRA) };
	CFArrayRef pfs = CFArrayCreate(NULL, (const void **)pf, 3, &kCFTypeArrayCallBacks);

	for (int i = 0; i < 3; i++) {
		CFRelease(pf[i]);
	}
	put(o, "SupportedPixelFormats", pfs);
	put(o, "NativeWidth", number_i(width));
	put(o, "NativeHeight", number_i(height));
	put(o, "MaxDownscaleFactor", number_f(32));
	put(o, "MaxScaleFactor", number_f(1));
	put(o, "MinScaleFactor", number_f(1.0 / 32));
	put(o, "OutputWidthAlignmentRequirement", number_i(16));
	return o;
}

static CFDictionaryRef
format(int sensor_width, int sensor_height, int max_fps, bool binned, double fov, double pixel_size)
{
	CFMutableDictionaryRef f = (CFMutableDictionaryRef)new_dict(), outputs = (CFMutableDictionaryRef)new_dict();
	CMVideoFormatDescriptionRef fd = NULL;

	CMVideoFormatDescriptionCreate(NULL, PF_420F, sensor_width, sensor_height, NULL, &fd);
	if (fd != NULL) {
		put(f, "FormatDescription", fd);
	}
	put(f, "VideoMinFrameRate", number_f(2));
	put(f, "VideoMaxFrameRate", number_f(max_fps));
	put(f, "VideoIsBinned", CFRetain(binned ? kCFBooleanTrue : kCFBooleanFalse));
	put(f, "VideoBinningFactorHorizontal", number_i(binned ? 2 : 1));
	put(f, "VideoBinningFactorVertical", number_i(binned ? 2 : 1));
	put(f, "VideoFieldOfView", number_f(fov));
	put(f, "PixelSize", number_f(pixel_size));      /* (redfin: the Pixel 5's sensor's pitch, um) */
	put(f, "VideoMinIntegrationTime", number_i(100));
	put(f, "VideoMaxIntegrationTime", number_i(1000000 / 15));
	put(f, "VideoRawBitDepth", number_i(10));
	put(f, "VideoMaxWidth", number_i(sensor_width));
	put(f, "VideoMaxHeight", number_i(sensor_height));
	put(f, "Experimental", CFRetain(kCFBooleanFalse));
	put(f, "SensorWidth", number_i(sensor_width));
	put(f, "SensorHeight", number_i(sensor_height));
	put(f, "ISOBase", number_f(32));
	put(f, "DefaultAEMaxGain", number_f(64));

	/*
	 * The sensor crop geometry the capture-graph builder reads off this format to resolve non-zero
	 * output dimensions (confirmed by a hook: -[FigCaptureSourceVideoFormat sensorCropDimensions] /
	 * postGDCCropDimensions] return these). Everything spans the full sensor: no GDC crop.
	 */
	put(f, "SensorCropWidth", number_i(sensor_width));
	put(f, "SensorCropHeight", number_i(sensor_height));
	put(f, "PreGDCSensorCropWidth", number_i(sensor_width));
	put(f, "PreGDCSensorCropHeight", number_i(sensor_height));
	put(f, "PostGDCCropWidth", number_i(sensor_width));
	put(f, "PostGDCCropHeight", number_i(sensor_height));
	put(f, "GeometricDistortionCorrectedNativeWidth", number_i(sensor_width));
	put(f, "GeometricDistortionCorrectedNativeHeight", number_i(sensor_height));

	put(outputs, "PrimaryScaler", output_info(sensor_width, sensor_height));
	put(outputs, "SecondaryScaler", output_info(sensor_width, sensor_height));
	put(outputs, "StillImage", output_info(sensor_width, sensor_height));
	put(f, "SupportedOutputs", outputs);
	return f;
}

/* ---- the stream ------------------------------------------------------------------------------------- */

#define HELD    3               /* the moving pictures kept from the pool after they are given (see deliver) */

struct output {
	const char *    name;
	CFDictionaryRef config;                 /* from VideoOutputConfigurations */
	bool            enabled;
	void *          handler;                /* a block: ^(CVPixelBufferRef, CMTime) */
	bool            said_shape;
	uint64_t        delivered, refused;
	CVPixelBufferRef held[HELD];            /* the pictures given last, kept from the pool (see deliver) */
	unsigned        held_next;
	bool            said_pool;
};

static void let_go(struct output *out);

#define OUTPUTS 3
static const char *const output_names[OUTPUTS] = { "PrimaryScaler", "SecondaryScaler", "StillImage" };

#define MAX_ZOOM        16              /* (the most the iPhone X's wide camera zooms digitally) */

/*
 * The device's cameras, a stream each, as the iPhone X's are in CMCapture's D22/AVCaptureSession.plist
 * (its devices' uniqueIDs, and the stream formats its formats and presets are matched to by sensor size
 * and frame rate): the wide camera as the back camera, the front one as the front camera. The kernel
 * gives one sensor's frames at a time; a stream's frames start with its own sensor's (see worker()).
 *
 * redfin (plan B3): the formats keep the iPhone X's sensor sizes and frame rates -- they are the keys the plist's
 * formats and presets are found by, and a size the plist does not have loses the AVCaptureDevice formats -- but
 * what they do is the Pixel 5's: each asks the kernel for a kind of sensor mode (REDWOOD_CAMERA_MODE_*: the photo
 * mode, the 16:9 >= 30 fps video mode, the >= 60 fps one), the pictures are scaled from whatever size the mode has
 * (MODE_CHANGED), and the frame rate is the mode's. The optics are the Pixel 5's (focal length in mm, f-number,
 * pixel pitch in um, horizontal field of view in degrees from the sensor's width and the focal length): IMX363
 * 4.38 mm (27 mm equivalent) f/1.7 1.4 um; IMX355 2.59 mm (24 mm eq.) f/2.0 1.12 um; IMX481 2.13 mm (16 mm eq.) f/2.2
 * 1.0 um. The ultra-wide camera is a third device (sensor 3) only with /private/var/local/camera-ultrawide: the
 * iPhone X has none, its port and formats are not in D22's plist, and how the capture stack takes that is for the
 * phone to show.
 */
static const char *mode_name(uint32_t kind);
static uint64_t frame_time(const struct redwood_camera_frame *f);

struct camera_format {
	int             width, height, fps;     /* the iPhone X's (the plist's keys) */
	uint32_t        kind;                   /* REDWOOD_CAMERA_MODE_*: the mode the kernel is asked for */
};

static const struct camera {
	uint32_t        sensor;         /* the kernel's (the frames' sensor, REDWOOD_CAMERA_SENSOR) */
	const char      *name, *unique_id;
	bool            turned;         /* its sensor the other way up from the iPhone's: the pictures turned half round */
	double          focal_length, f_number, pixel_size, fov;
	struct camera_format formats[2];        /* "Photo", and a video format */
	bool            lens_motor;     /* focus can move (where the kernel says so: the frames' focus) */
} cameras[3] = {
	{ 1, "Back Camera", "com.apple.avfoundation.avcapturedevice.built-in_video:0", false, 4.38, 1.7, 1.4, 65.6,
	/* sunfish (f2): camd has one mode (8: 2016x1512 RAW10 @60, stride 2520), so both formats ask for it (PHOTO, 4:3: no
	 * SENSOR mode change); the sizes stay the plist's keys, the pictures are scaled from 2016x1512 (previews binned,
	 * the still demosaiced from the whole frame) */
	  { { 4032, 3024, 30, REDWOOD_CAMERA_MODE_PHOTO }, { 4224, 2376, 60, REDWOOD_CAMERA_MODE_PHOTO } }, true },  /* "Photo", "Video-60" */
	/* redfin: the front IMX355's pictures came out upside down untouched (r18g, Camera.app's own thumbnail): turned,
	 * as HackiOS's phone's was */
	{ 2, "Front Camera", "com.apple.avfoundation.avcapturedevice.built-in_video:1", true, 2.59, 2.0, 1.12, 70.9,
	  { { 3088, 2320, 30, REDWOOD_CAMERA_MODE_PHOTO }, { 3840, 2160, 30, REDWOOD_CAMERA_MODE_VIDEO } }, false },  /* "Photo", "Video-30" */
	/* (the iPhone 11's uniqueID for its ultra-wide; the back camera's plist keys) */
	{ 3, "Back Ultra Wide Camera", "com.apple.avfoundation.avcapturedevice.built-in_video:5", false, 2.13, 2.2, 1.0, 94.5,
	  { { 4032, 3024, 30, REDWOOD_CAMERA_MODE_PHOTO }, { 4224, 2376, 60, REDWOOD_CAMERA_MODE_VIDEO60 } }, false },
};
#define CAMERAS 3

struct stream {
	CFArrayRef      formats;
	CFDictionaryRef supported;
	CFDictionaryRef colour_info;    /* from VideoOutputsColorInfo */
	CFDictionaryRef metadata;       /* the last frame's (frame_metadata()), for a buffer that has none of its own */
	pthread_mutex_t metadata_lock;  /* (not `lock': AddAttachmentsToSampleBuffer may come while a frame is given out) */
	unsigned        stills_wanted;  /* photographs asked for by StillImageCaptureNow, not yet given */
	int             format_index;
	double          min_fps, max_fps;
	bool            min_fps_set;    /* (redfin) MinimumFrameRate was set by the graph */
	volatile bool   mode_wanted;    /* (redfin) FormatIndex changed while the frames run: the kernel to be asked for its mode */
	float           zoom;           /* ZoomFactor: the picture is the middle 1/zoom of the sensor each way */
	const struct camera *camera;
	CFStringRef     port;
	bool            af_now;         /* a focus asked for (AutoFocusNow and the like): a search at the next frame */
	bool            af_locked;      /* LockAFNow: no search but one asked for */
	float           af_window[4];   /* AFWindowParams: where to focus, as fractions of the frame (x0, y0, x1, y1) */
	struct output   out[OUTPUTS];
	pthread_mutex_t lock;
	volatile bool   running;
	volatile bool   graph_running;  /* the capture graph has started this stream: frames may be given out */
	volatile uint64_t given_back_at; /* sunfish (f3): CLOCK_MONOTONIC ns of the give-back (worker_given_back_long()) */
	volatile bool   given_back;     /* sunfish (f3): relinquished, not asked for again: no more reopening (worker()) */
	/*
	 * redfin (B8): the worker threads. Each has the generation it was started for and goes when that is not the
	 * stream's any more; a stop waits for them a while (workers, worker_gone), not for ever: a worker can be
	 * inside the kernel's read for seconds while the sensor starts or stops.
	 */
	volatile unsigned generation;
	unsigned        workers;
	pthread_mutex_t worker_lock;
	pthread_cond_t  worker_gone;
	int             torch;
	uint64_t        frames, stills_given;
	/*
	 * redfin (plan B4-B7): what the capture stack asks of exposure, white balance, focus and the flash (the
	 * properties below; the worker takes a copy each frame, under ctl_lock).
	 */
	pthread_mutex_t ctl_lock;
	bool            ae_locked, ae_manual, limit_ae_range, awb_locked, awb_manual;
	float           ae_bias, ae_roi[4], ae_max_gain, manual_gain, wb_manual[3];
	uint32_t        ae_max_exposure_us, manual_us;
	int             af_mode;        /* AF_MODE_*: what focus does once it has found the sharpest */
	int32_t         manual_focus;   /* ManualFocusNow: where the lens is to go (0 infinity to 1023 macro), -1: nowhere */
	volatile bool   has_focus;      /* the frames say where the lens is (a lens motor): HasAutoFocusPositionSensor */
	int             flash_mode;     /* FlashMode: 0 off, 1 on, 2 auto */
	int             still_flash;    /* the flash the latest StillImageCaptureNow asked for: 0 off, 1 on, 2 auto, -1 as FlashMode */
};

/* AF_MODE_*: after a search, focus keeps watching (continuous), or stays where it is (auto: one search) */
enum { AF_MODE_AUTO = 1, AF_MODE_CONTINUOUS = 2 };

static struct stream *
stream_of(CMBaseObjectRef o)
{
	return derived_storage(o);
}

static int
output_index(CFStringRef name)
{
	for (int i = 0; i < OUTPUTS; i++) {
		if (key_is(name, output_names[i])) {
			return i;
		}
	}
	return -1;
}

static OSStatus
stream_copy_property(CMBaseObjectRef o, CFStringRef key, CFAllocatorRef allocator, void *out)
{
	struct stream *s = stream_of(o);
	CFTypeRef value = NULL;

	(void)allocator;
	if (key_is(key, "PortType")) {
		value = CFRetain(s->port);
	} else if (key_is(key, "UniqueID") || key_is(key, "ModelID")) {
		value = CFStringCreateWithCString(NULL, s->camera->unique_id, kCFStringEncodingUTF8);
	} else if (key_is(key, "NonLocalizedName") || key_is(key, "LocalizedName")) {
		value = CFStringCreateWithCString(NULL, s->camera->name, kCFStringEncodingUTF8);
	} else if (key_is(key, "Manufacturer")) {
		value = CFRetain(CFSTR("Apple Inc."));
	} else if (key_is(key, "SupportedFormatsArray")) {
		value = CFRetain(s->formats);
	} else if (key_is(key, "FormatIndex")) {
		value = number_i(s->format_index);
	} else if (key_is(key, "MinimumFrameRate")) {
		value = number_f(s->min_fps);
	} else if (key_is(key, "MaximumFrameRate")) {
		value = number_f(s->max_fps);
	} else if (key_is(key, "Clock")) {
		value = CFRetain(CMClockGetHostTimeClock());
	} else if (key_is(key, "TorchLevel")) {
		/* redfin (B6): the flashlight's level as the kernel has it (kern.sweet_torch, thousandths) */
		int level = 0;
		size_t size = sizeof(level);

		value = number_f(sysctlbyname("kern.sweet_torch", &level, &size, NULL, 0) == 0 && level > 0 ? level / 1000.0 : 0);
	} else if (key_is(key, "ExposureBias")) {
		value = number_f(s->ae_bias);
	} else if (key_is(key, "FlashMode")) {
		value = number_i(s->flash_mode);
	} else if (key_is(key, "MaxIntegrationTime")) {
		value = number_i(s->ae_max_exposure_us ? s->ae_max_exposure_us : 1000000 / 15);
	} else if (key_is(key, "AEMaxGain")) {
		value = number_f(s->ae_max_gain > 0 ? s->ae_max_gain : 64);

	} else if (key_is(key, "VideoOutputConfigurations") || key_is(key, "VideoOutputsEnabled")) {
		/*
		 * ReadWrite, but the capture-graph builder READS these back while it builds the graph
		 * (BWMultiStreamCameraSourceNode's _updateOutputsStorage does a getProperty: for
		 * VideoOutputConfigurations, not IfSupported:), and a failed read tears the graph down. Return
		 * what each output holds now.
		 */
		bool configs = key_is(key, "VideoOutputConfigurations");
		CFMutableDictionaryRef d = (CFMutableDictionaryRef)new_dict();

		pthread_mutex_lock(&s->lock);
		for (int i = 0; i < OUTPUTS; i++) {
			CFStringRef name = CFStringCreateWithCString(NULL, output_names[i], kCFStringEncodingUTF8);

			if (configs) {
				if (s->out[i].config != NULL) {
					CFDictionarySetValue(d, name, s->out[i].config);
				}
			} else {
				CFDictionarySetValue(d, name, s->out[i].enabled ? kCFBooleanTrue : kCFBooleanFalse);
			}
			CFRelease(name);
		}
		pthread_mutex_unlock(&s->lock);
		value = d;
	} else if (key_is(key, "VideoOutputsColorInfo")) {
		/*
		 * ReadWrite, and the capture graph READS it before it starts the session:
		 * -[BWMultiStreamCameraSourceNode _verifyColorSpacePropertiesForAllOutputs] does a
		 * getProperty:error: for it, and an error there fails the session's transition to running
		 * and tears the whole graph down again. What it holds matters only for an output whose
		 * colour space is not sRGB (_verifyColorSpacePropertiesForOutputID: returns at once when
		 * the output's colour space is 0), and sRGB is the only one this stream offers, so what
		 * the graph set is given back as it is.
		 */
		pthread_mutex_lock(&s->lock);
		value = s->colour_info != NULL ? CFRetain(s->colour_info) : new_dict();
		pthread_mutex_unlock(&s->lock);
	} else if (key_is(key, "SupportedPropertiesDictionary")) {
		value = CFRetain(s->supported);
	} else if (key_is(key, "FocalLength")) {
		value = number_f(s->camera->focal_length);     /* (the iPhone X's cameras', in millimetres) */
	} else if (key_is(key, "LensFNumber")) {
		value = number_f(s->camera->f_number);
	} else if (key_is(key, "PixelSize")) {
		/*
		 * The capture-graph builder REQUIRES this of the stream: -[BWFigVideoCaptureStream
		 * initWithCaptureStream:...] does a getProperty: (not IfSupported:) for it, and without it the
		 * init fails, so FigVideoCaptureSourcesActivateAndCreateDevices errors and the graph is never
		 * built. The sensor's pixel pitch in micrometres (the iPhone X cameras'); any value in
		 * (0, 100) is accepted.
		 */
		value = number_f(s->camera->pixel_size);
	} else if (key_is(key, "MaxZoomFactor")) {
		value = number_f(MAX_ZOOM);
	} else if (key_is(key, "ZoomFactor")) {
		value = number_f(s->zoom);
	} else if (key_is(key, "SupportedColorSpaces")) {
		CFNumberRef srgb = number_i(0);

		value = CFArrayCreate(NULL, (const void **)&srgb, 1, &kCFTypeArrayCallBacks);
		CFRelease(srgb);
	} else if (key_is(key, "ColorSpace") || key_is(key, "DeviceOrientation")) {
		value = number_i(0);
	} else if (key_is(key, "HasAutoFocusPositionSensor")) {
		/* redfin (B5): where the frames say where the lens is (the kernel's lens motor) */
		value = CFRetain(s->has_focus ? kCFBooleanTrue : kCFBooleanFalse);
	} else if (key_is(key, "HighCurrentTorchEnabled") || key_is(key, "DeferAdditionOfAttachments")) {
		value = CFRetain(kCFBooleanFalse);
	} else {
		say_once("stream: no answer for", key, NULL);
		return ERR_NOT_AVAILABLE;
	}
	*(CFTypeRef *)out = value;
	return 0;
}

static void torch_set(struct stream *s, CFTypeRef value);

static double
dict_number(CFDictionaryRef d, const char *name, double otherwise)
{
	CFStringRef k = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
	CFTypeRef v = CFDictionaryGetValue(d, k);
	double n = otherwise;

	CFRelease(k);
	if (v != NULL && CFGetTypeID(v) == CFNumberGetTypeID()) {
		CFNumberGetValue(v, kCFNumberDoubleType, &n);
	}
	return n;
}

/* AFWindowParams: the first window, as fractions of the frame; none (or not one) is the middle */
static void
af_window(struct stream *s, CFTypeRef value)
{
	CFDictionaryRef w = NULL;
	double x0, y0, width, height;

	if (value != NULL && CFGetTypeID(value) == CFArrayGetTypeID() && CFArrayGetCount(value) > 0) {
		w = CFArrayGetValueAtIndex(value, 0);
	}
	if (w == NULL || CFGetTypeID(w) != CFDictionaryGetTypeID() || (width = dict_number(w, "width", 0)) <= 0 ||
	    (height = dict_number(w, "height", 0)) <= 0) {
		s->af_window[0] = s->af_window[1] = 0.3f;
		s->af_window[2] = s->af_window[3] = 0.7f;
		return;
	}
	x0 = dict_number(w, "hStart", 0) / 1000;
	y0 = dict_number(w, "vStart", 0) / 1000;
	width /= 1000;
	height /= 1000;
	/* (at least a tenth of the frame each way, about the middle of what was asked) */
	if (width < 0.1) {
		x0 -= (0.1 - width) / 2;
		width = 0.1;
	}
	if (height < 0.1) {
		y0 -= (0.1 - height) / 2;
		height = 0.1;
	}
	x0 = x0 < 0 ? 0 : x0 + width > 1 ? 1 - width : x0;
	y0 = y0 < 0 ? 0 : y0 + height > 1 ? 1 - height : y0;
	s->af_window[0] = (float)x0;
	s->af_window[1] = (float)y0;
	s->af_window[2] = (float)(x0 + width);
	s->af_window[3] = (float)(y0 + height);
}

/*
 * The lens's travel focus uses (the kernel's positions: 0 infinity, 1023 macro). redfin (WS-A, kernel 0929): the back
 * camera's actuator is an ON Semi LC898129 (AF, OIS and EEPROM in one chip); the kernel maps 0..1023 onto its codes
 * 400..725 for now (to be widened from the EEPROM's calibration), so the whole of it is safe to use -- HackiOS's phone
 * stopped at 768 (an over-current on its LDO) and had a hard stop at 0. The IMX355 and IMX481 are fixed focus.
 */
#define AF_NEAR         0               /* (infinity) */
#define AF_FAR          REDWOOD_CAMERA_FOCUS_MAX        /* (macro) */
#define AF_STEP         64
#define AF_FINE         16

/* ---- redfin (plan B4-B7): the controls' values ------------------------------------------------------------------- */

/* a number, a boolean as 0 or 1, or a CMTime's dictionary in seconds */
static bool
number_of(CFTypeRef value, double *n)
{
	if (value == NULL) {
		return false;
	}
	if (CFGetTypeID(value) == CFNumberGetTypeID()) {
		return CFNumberGetValue(value, kCFNumberDoubleType, n);
	}
	if (CFGetTypeID(value) == CFBooleanGetTypeID()) {
		*n = CFBooleanGetValue(value) ? 1 : 0;
		return true;
	}
	if (CFGetTypeID(value) == CFDictionaryGetTypeID() && CFDictionaryContainsKey(value, CFSTR("timescale"))) {
		CMTime t = CMTimeMakeFromDictionary(value);

		if (CMTIME_IS_NUMERIC(t)) {
			*n = CMTimeGetSeconds(t);
			return true;
		}
	}
	return false;
}

static bool
name_has(CFStringRef key, const char *part)
{
	char name[128];

	return CFGetTypeID(key) == CFStringGetTypeID() && CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8) &&
	    strcasestr(name, part) != NULL;
}

/* the value under the first key with `part' in its name (a dictionary's), as a number */
static bool
dict_find(CFDictionaryRef d, const char *part, double *n, const char *not_part)
{
	CFIndex count = CFDictionaryGetCount(d);
	const void **keys = calloc((size_t)count + 1, sizeof(void *)), **values = calloc((size_t)count + 1, sizeof(void *));
	bool found = false;

	if (keys == NULL || values == NULL) {
		free(keys);
		free(values);
		return false;
	}
	CFDictionaryGetKeysAndValues(d, keys, values);
	for (CFIndex i = 0; i < count && !found; i++) {
		if (name_has(keys[i], part) && (not_part == NULL || !name_has(keys[i], not_part))) {
			found = number_of(values[i], n);
		}
	}
	free(keys);
	free(values);
	return found;
}

/*
 * A rectangle of the frame as fractions (x0, y0, x1, y1): a dictionary of X/Y/Width/Height (CGRect's) or
 * hStart/vStart/width/height (AFWindowParams'), or an array whose first item is one; thousandths where the numbers
 * are larger than 1. False: none (the whole frame).
 */
static bool
rect_of(CFTypeRef value, float r[4])
{
	double x = 0, y = 0, w = 0, h = 0;

	if (value != NULL && CFGetTypeID(value) == CFArrayGetTypeID()) {
		value = CFArrayGetCount(value) > 0 ? CFArrayGetValueAtIndex(value, 0) : NULL;
	}
	if (value == NULL || CFGetTypeID(value) != CFDictionaryGetTypeID()) {
		return false;
	}
	if (!dict_find(value, "width", &w, NULL) || !dict_find(value, "height", &h, NULL) || w <= 0 || h <= 0) {
		return false;
	}
	if (!dict_find(value, "hStart", &x, NULL)) {
		dict_find(value, "x", &x, "max");
	}
	if (!dict_find(value, "vStart", &y, NULL)) {
		dict_find(value, "y", &y, "max");
	}
	if (w > 1.001 || h > 1.001 || x > 1.001 || y > 1.001) {
		x /= 1000, y /= 1000, w /= 1000, h /= 1000;
	}
	if (w >= 0.98 && h >= 0.98) {
		return false;           /* (the whole frame) */
	}
	x = x < 0 ? 0 : x > 1 ? 1 : x;
	y = y < 0 ? 0 : y > 1 ? 1 : y;
	r[0] = (float)x;
	r[1] = (float)y;
	r[2] = (float)(x + w > 1 ? 1 : x + w);
	r[3] = (float)(y + h > 1 ? 1 : y + h);
	return r[2] > r[0] && r[3] > r[1];
}

/*
 * A custom exposure, however it comes (the names are not known here): a duration (a CMTime, seconds below 10,
 * microseconds above) under a key with "Integration", "Duration" or "Exposure" in its name, a gain under one with
 * "ISO" (ISOBase 32 is 1x), "Gain" or "AGC" (256 is 1x where it is that large). True if a duration was found.
 */
static bool
exposure_of(CFTypeRef value, uint32_t *us, float *gain)
{
	double d = 0, g = 0;

	if (value == NULL || CFGetTypeID(value) != CFDictionaryGetTypeID()) {
		return false;
	}
	if (!dict_find(value, "Integration", &d, NULL) && !dict_find(value, "Duration", &d, NULL) &&
	    !dict_find(value, "Exposure", &d, "Bias")) {
		return false;
	}
	*us = (uint32_t)(d < 10 ? d * 1e6 : d);
	*gain = 1;
	if (dict_find(value, "ISO", &g, NULL)) {
		*gain = (float)(g / 32);
	} else if (dict_find(value, "Gain", &g, NULL) || dict_find(value, "AGC", &g, NULL)) {
		*gain = (float)(g >= 64 ? g / 256 : g);
	}
	*gain = *gain < 1 ? 1 : *gain;
	return *us > 0;
}

/* white balance gains: R, G, B under keys with "Red"/"Green"/"Blue" (or "R"/"G"/"B" alone), or an array of three */
static bool
gains_of(CFTypeRef value, float g[3])
{
	double r = 0, gr = 0, b = 0;

	if (value != NULL && CFGetTypeID(value) == CFArrayGetTypeID() && CFArrayGetCount(value) == 3) {
		bool all = number_of(CFArrayGetValueAtIndex(value, 0), &r) && number_of(CFArrayGetValueAtIndex(value, 1), &gr) &&
		    number_of(CFArrayGetValueAtIndex(value, 2), &b);

		if (!all) {
			return false;
		}
	} else if (value == NULL || CFGetTypeID(value) != CFDictionaryGetTypeID() || !dict_find(value, "Red", &r, NULL) ||
	    !dict_find(value, "Green", &gr, NULL) || !dict_find(value, "Blue", &b, NULL)) {
		if (value == NULL || CFGetTypeID(value) != CFDictionaryGetTypeID() || !dict_find(value, "RGain", &r, NULL) ||
		    !dict_find(value, "GGain", &gr, NULL) || !dict_find(value, "BGain", &b, NULL)) {
			return false;
		}
	}
	if (r <= 0 || gr <= 0 || b <= 0) {
		return false;
	}
	/* (as ratios to green: whatever their unit, it goes) */
	g[0] = (float)(r / gr);
	g[1] = 1;
	g[2] = (float)(b / gr);
	return true;
}

/* StillImageCaptureNow's: how many pictures, and the flash (0 off, 1 on, 2 auto; -1 not said) */
static void
still_request(CFTypeRef value, unsigned *count, int *flash)
{
	double n;

	*count = 1;
	*flash = -1;
	if (value == NULL || CFGetTypeID(value) != CFDictionaryGetTypeID()) {
		return;
	}
	if (dict_find(value, "Flash", &n, "Torch")) {
		*flash = n <= 0 ? 0 : n >= 2 ? 2 : 1;
	}
	if ((dict_find(value, "NumberOf", &n, NULL) || dict_find(value, "Count", &n, NULL)) && n >= 1) {
		*count = n > 8 ? 8 : (unsigned)n;
	}
}

/* ManualFocusNow's lens position: AVFoundation's 0 (nearest) to 1 (furthest), as the kernel's 0 (infinity) to 1023 */
static int32_t
focus_of(CFTypeRef value)
{
	double p = -1;

	if (value != NULL && CFGetTypeID(value) == CFDictionaryGetTypeID()) {
		if (!dict_find(value, "LensPosition", &p, NULL) && !dict_find(value, "Position", &p, NULL)) {
			return -1;
		}
	} else if (!number_of(value, &p)) {
		return -1;
	}
	if (p > 1.001) {
		return (int32_t)(p > 1023 ? 1023 : p);  /* (already a position) */
	}
	p = p < 0 ? 0 : p;
	return (int32_t)((1 - p) * (AF_FAR - AF_NEAR) + AF_NEAR + 0.5);
}

static OSStatus
stream_set_property(CMBaseObjectRef o, CFStringRef key, CFTypeRef value)
{
	struct stream *s = stream_of(o);

	if (key_is(key, "FormatIndex")) {
		int64_t i = 0;

		if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
			CFNumberGetValue(value, kCFNumberSInt64Type, &i);
		}
		if (i < 0 || i >= CFArrayGetCount(s->formats)) {
			return ERR_PARAM;
		}
		/* redfin (B3): the format's kind of sensor mode, asked of the kernel at the next frame (worker()) */
		if (s->format_index != (int)i) {
			s->mode_wanted = s->running;
		}
		s->format_index = (int)i;
		say("stream: format %lld (%s)", (long long)i, mode_name(s->camera->formats[i < 2 ? i : 0].kind));
		return 0;
	}
	if (key_is(key, "ZoomFactor")) {
		/*
		 * The zoom: the camera's pictures -- moving and still alike -- made of the middle of the sensor
		 * (src/camera_isp.c, crop()), as the iPhone's ISP crops for it. Digital: no more detail than
		 * that part of the sensor has.
		 */
		double zoom = 1;

		if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
			CFNumberGetValue(value, kCFNumberDoubleType, &zoom);
		}
		zoom = zoom < 1 ? 1 : zoom > MAX_ZOOM ? MAX_ZOOM : zoom;
		if (fabs(zoom - s->zoom) >= 0.25 || (zoom == 1) != (s->zoom == 1)) {
			say("stream: zoom %.2fx", zoom);
		}
		s->zoom = (float)zoom;
		return 0;
	}
	if (key_is(key, "MinimumFrameRate") || key_is(key, "MaximumFrameRate")) {
		double fps = 0;

		if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
			CFNumberGetValue(value, kCFNumberDoubleType, &fps);
		}
		*(key_is(key, "MinimumFrameRate") ? &s->min_fps : &s->max_fps) = fps;
		s->min_fps_set |= key_is(key, "MinimumFrameRate");
		say_value("stream: set", key, value);  /* (redfin: AE keeps to them, see fps_limits()) */
		return 0;
	}
	if (key_is(key, "VideoOutputConfigurations") || key_is(key, "VideoOutputsEnabled") || key_is(key, "VideoOutputHandlers")) {
		bool configs = key_is(key, "VideoOutputConfigurations"), enabled = key_is(key, "VideoOutputsEnabled");

		say_once("stream: set", key, value);
		if (value == NULL || CFGetTypeID(value) != CFDictionaryGetTypeID()) {
			return 0;
		}
		pthread_mutex_lock(&s->lock);
		CFIndex n = CFDictionaryGetCount(value);
		const void **keys = calloc((size_t)n + 1, sizeof(void *)), **values = calloc((size_t)n + 1, sizeof(void *));

		CFDictionaryGetKeysAndValues(value, keys, values);
		for (CFIndex i = 0; i < n; i++) {
			int k = CFGetTypeID(keys[i]) == CFStringGetTypeID() ? output_index(keys[i]) : -1;
			struct output *out;

			if (k < 0) {
				say_once("stream: an output not here:", keys[i], NULL);
				continue;
			}
			out = &s->out[k];
			if (configs) {
				if (out->config != NULL) {
					CFRelease(out->config);
				}
				let_go(out);
				out->config = CFGetTypeID(values[i]) == CFDictionaryGetTypeID() ? CFRetain(values[i]) : NULL;
			} else if (enabled) {
				int64_t on = 1;

				if (CFGetTypeID(values[i]) == CFBooleanGetTypeID()) {
					on = CFBooleanGetValue(values[i]);
				} else if (CFGetTypeID(values[i]) == CFNumberGetTypeID()) {
					CFNumberGetValue(values[i], kCFNumberSInt64Type, &on);
				}
				out->enabled = on != 0;
			} else {
				CFTypeRef h = CFGetTypeID(values[i]) == CFDictionaryGetTypeID() ?
				    CFDictionaryGetValue(values[i], CFSTR("PixelBufferHandler")) : NULL;

				if (out->handler != NULL) {
					CFRelease(out->handler);
				}
				out->handler = h != NULL ? (void *)CFRetain(h) : NULL;
			}
		}
		free(keys);
		free(values);
		pthread_mutex_unlock(&s->lock);
		return 0;
	}
	if (key_is(key, "StillImageCaptureNow")) {
		/*
		 * The shutter: the capture graph's still image coordinator asks for a photograph by writing
		 * this, with what it wants of it (how many pictures, the flash, the settings' number by which
		 * it will know the frames again). One frame of the sensor goes to the StillImage output for
		 * each ask.
		 */
		unsigned count;
		int flash;

		still_request(value, &count, &flash);
		pthread_mutex_lock(&s->lock);
		s->stills_wanted += count;      /* (redfin, B6: as many as asked for) */
		s->still_flash = flash;
		pthread_mutex_unlock(&s->lock);
		say_value("stream: a photograph asked for", key, value);
		return 0;
	}
	if (key_is(key, "VideoOutputsColorInfo")) {
		pthread_mutex_lock(&s->lock);
		if (s->colour_info != NULL) {
			CFRelease(s->colour_info);
		}
		s->colour_info = value != NULL && CFGetTypeID(value) == CFDictionaryGetTypeID() ? CFRetain(value) : NULL;
		pthread_mutex_unlock(&s->lock);
		return 0;
	}
	if (key_is(key, "TorchLevel")) {
		torch_set(s, value);
		return 0;
	}
	/* ---- redfin (plan B4): exposure ---- */
	if (key_is(key, "LockAENow") || key_is(key, "UnlockAENow")) {
		double on = 1;

		number_of(value, &on);          /* (LockAENow false: unlocked) */
		pthread_mutex_lock(&s->ctl_lock);
		s->ae_locked = key_is(key, "LockAENow") && on != 0;
		if (!s->ae_locked) {
			s->ae_manual = false;
		}
		pthread_mutex_unlock(&s->ctl_lock);
		say("stream: exposure %s", s->ae_locked ? "locked" : "unlocked");
		return 0;
	}
	if (key_is(key, "ExposureBias")) {
		double bias = 0;

		number_of(value, &bias);
		s->ae_bias = (float)(bias < -8 ? -8 : bias > 8 ? 8 : bias);
		say_once("stream: set", key, value);
		return 0;
	}
	if (key_is(key, "RegionOfInterestForCameraControls") || key_is(key, "AEWindowParams")) {
		float r[4] = { 0, 0, 0, 0 };
		bool some = rect_of(value, r);

		pthread_mutex_lock(&s->ctl_lock);
		memcpy(s->ae_roi, r, sizeof(r));
		pthread_mutex_unlock(&s->ctl_lock);
		if (some) {
			say_value("stream: exposure: metered on", key, value);
		}
		return 0;
	}
	if (key_is(key, "MaxIntegrationTime")) {
		double us = 0;

		number_of(value, &us);
		s->ae_max_exposure_us = us <= 0 ? 0 : us < 10 ? (uint32_t)(us * 1e6) : (uint32_t)us;
		say_once("stream: set", key, value);
		return 0;
	}
	if (key_is(key, "AEMaxGain")) {
		double g = 0;

		number_of(value, &g);
		s->ae_max_gain = (float)(g >= 256 ? g / 256 : g);
		say_once("stream: set", key, value);
		return 0;
	}
	if (key_is(key, "LimitAERange")) {
		double on = 0;

		number_of(value, &on);
		s->limit_ae_range = on != 0;
		say_once("stream: set", key, value);
		return 0;
	}
	if (key_is(key, "ExposureMode")) {
		double mode = 2;

		/* (AVCaptureExposureMode: 0 locked, 1 auto, 2 continuous, 3 custom) */
		number_of(value, &mode);
		pthread_mutex_lock(&s->ctl_lock);
		s->ae_locked = mode == 0;
		s->ae_manual = mode == 3 && s->manual_us != 0;
		pthread_mutex_unlock(&s->ctl_lock);
		say_value("stream: exposure:", key, value);
		return 0;
	}
	if (key_is(key, "ManualExposureNow") || key_is(key, "IntegrationTimeAndGain") || key_is(key, "ManualExposureParams") ||
	    key_is(key, "ExposureDurationAndISO")) {
		uint32_t us;
		float gain;

		say_value("stream: exposure:", key, value);
		if (exposure_of(value, &us, &gain)) {
			pthread_mutex_lock(&s->ctl_lock);
			s->ae_manual = true;
			s->manual_us = us;
			s->manual_gain = gain;
			pthread_mutex_unlock(&s->ctl_lock);
			say("stream: a custom exposure: %.2f ms at %.2fx", us / 1000.0, gain);
		}
		return 0;
	}
	/* ---- redfin (plan B7): white balance ---- */
	if (key_is(key, "LockAWBNow") || key_is(key, "UnlockAWBNow")) {
		double on = 1;

		number_of(value, &on);          /* (LockAWBNow false: unlocked) */
		pthread_mutex_lock(&s->ctl_lock);
		s->awb_locked = key_is(key, "LockAWBNow") && on != 0;
		if (!s->awb_locked) {
			s->awb_manual = false;
		}
		pthread_mutex_unlock(&s->ctl_lock);
		say("stream: white balance %s", s->awb_locked ? "locked" : "unlocked");
		return 0;
	}
	if (key_is(key, "ManualAWBGainsNow") || key_is(key, "WhiteBalanceGains") || key_is(key, "ManualWhiteBalanceGains")) {
		float g[3];

		say_value("stream: white balance:", key, value);
		if (gains_of(value, g)) {
			pthread_mutex_lock(&s->ctl_lock);
			memcpy(s->wb_manual, g, sizeof(g));
			s->awb_manual = true;
			pthread_mutex_unlock(&s->ctl_lock);
		}
		return 0;
	}
	/* ---- redfin (plan B6): the flash ---- */
	if (key_is(key, "FlashMode")) {
		double mode = 0;

		number_of(value, &mode);
		s->flash_mode = mode <= 0 ? 0 : mode >= 2 ? 2 : 1;
		say_value("stream: set", key, value);
		return 0;
	}
	if (key_is(key, "AutoFocusNow") || key_is(key, "ContinuousAutoFocusNow") || key_is(key, "FocusNow") ||
	    key_is(key, "PeakTrackingFocusNow") || key_is(key, "LockAFNow") || key_is(key, "ManualFocusNow") ||
	    key_is(key, "AFWindowParams") || key_is(key, "AutoFocusMode")) {
		/*
		 * Camera.app's tap: AutoFocusMode "AutoFocusModeWeightedSum", AFWindowParams an array of
		 * { hStart, vStart, width, height, weight } in thousandths of the frame (the first window is
		 * the one taken here; an empty value is the middle), then AutoFocusNow (true). LockAFNow
		 * keeps the lens where it is until focus is asked for again.
		 */
		if (!key_is(key, "AFWindowParams") || (value != NULL && CFGetTypeID(value) == CFArrayGetTypeID() &&
		    CFArrayGetCount(value) > 0)) {
			say_value("stream: focus:", key, value);        /* (an empty window, the middle: often, unsaid) */
		}
		/*
		 * redfin (B5): ContinuousAutoFocusNow is a mode (focus keeps watching the scene and searches again), AutoFocusNow
		 * (and FocusNow, PeakTrackingFocusNow) one search that then stays (AVCaptureFocusModeAutoFocus); ManualFocusNow
		 * takes the lens where it is asked (AVFoundation's lens position) and keeps it there.
		 */
		pthread_mutex_lock(&s->ctl_lock);
		if (key_is(key, "AFWindowParams")) {
			af_window(s, value);
		} else if (key_is(key, "LockAFNow")) {
			double on = 1;

			number_of(value, &on);
			s->af_locked = on != 0;
		} else if (key_is(key, "ManualFocusNow")) {
			int32_t at = focus_of(value);

			if (at >= 0) {
				s->manual_focus = at;
				s->af_locked = true;
			}
		} else if (key_is(key, "AutoFocusNow") || key_is(key, "ContinuousAutoFocusNow") || key_is(key, "FocusNow") ||
		    key_is(key, "PeakTrackingFocusNow")) {
			s->af_mode = key_is(key, "ContinuousAutoFocusNow") ? AF_MODE_CONTINUOUS : AF_MODE_AUTO;
			s->af_locked = false;
			s->manual_focus = -1;
			s->af_now = true;
		}
		pthread_mutex_unlock(&s->ctl_lock);
		return 0;
	}
	say_once("stream: set (not used)", key, value);
	return 0;
}

/* the flashlight: TorchLevel (0 to 1) as thousandths to the kernel (HackiOS: /dev/redwood-torch; redfin: a sysctl) */
static void
torch_set(struct stream *s, CFTypeRef value)
{
	float level = 0;
	uint32_t thousandths;

	if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID()) {
		CFNumberGetValue(value, kCFNumberFloatType, &level);
	}
	level = level < 0 ? 0 : level > 1 ? 1 : level;
	thousandths = (uint32_t)(level * 1000 + 0.5f);
	/* redfin: the kernel's flashlight is sysctl kern.sweet_torch (xnu 0918, sweettorch=2), thousandths as here */
	int kernel_level = (int)thousandths;
	(void)s;
	if (sysctlbyname("kern.sweet_torch", NULL, NULL, &kernel_level, sizeof(kernel_level)) != 0) {
		say("the flashlight at %u of 1000 NOT TAKEN (kern.sweet_torch: %s)", thousandths, strerror(errno));
	} else {
		say("the flashlight at %u of 1000", thousandths);
	}
}

/* ---- the frames ------------------------------------------------------------------------------------- */

struct render_job {
	const struct isp *isp;
	const struct redwood_camera_frame *f;
	const uint8_t *raw;
	CVPixelBufferRef pb;
	uint32_t format, width, height, pieces;
	bool still;                     /* the photograph: every pixel of the sensor (isp_render_still_nv12) */
};

static void
render_piece(void *context, size_t i)
{
	struct render_job *j = context;
	uint32_t rows = (j->height + j->pieces - 1) / j->pieces, first = (uint32_t)i * rows, end = first + rows;

	first &= ~1u;
	end = i + 1 == j->pieces ? j->height : end & ~1u;
	if (j->format == PF_BGRA) {
		isp_render_bgra(j->isp, j->f, j->raw, CVPixelBufferGetBaseAddress(j->pb), CVPixelBufferGetBytesPerRow(j->pb), j->width,
		    j->height, first, end);
	} else if (j->still) {
		isp_render_still_nv12(j->isp, j->f, j->raw, CVPixelBufferGetBaseAddressOfPlane(j->pb, 0),
		    CVPixelBufferGetBytesPerRowOfPlane(j->pb, 0), CVPixelBufferGetBaseAddressOfPlane(j->pb, 1),
		    CVPixelBufferGetBytesPerRowOfPlane(j->pb, 1), j->width, j->height, first, end, j->format == PF_420V);
	} else {
		isp_render_nv12(j->isp, j->f, j->raw, CVPixelBufferGetBaseAddressOfPlane(j->pb, 0), CVPixelBufferGetBytesPerRowOfPlane(j->pb, 0),
		    CVPixelBufferGetBaseAddressOfPlane(j->pb, 1), CVPixelBufferGetBytesPerRowOfPlane(j->pb, 1), j->width, j->height, first, end,
		    j->format == PF_420V);
	}
}

static CVPixelBufferRef
output_buffer(struct output *out)
{
	CVPixelBufferPoolRef pool = NULL;
	CVPixelBufferRef pb = NULL;
	CFTypeRef value = out->config != NULL ? CFDictionaryGetValue(out->config, CFSTR("BufferPool")) : NULL;

	/*
	 * The graph makes the pool itself and hands it over in the output's configuration
	 * (kFigCaptureStreamVideoOutputConfigurationKey_BufferPool), sized and laid out as the node
	 * behind the output wants it (an IOSurface each, extra rows at the bottom where a scaler wants
	 * them, as few buffers as the node holds at once) -- there is nothing for this side to choose,
	 * and a buffer of our own making is not one the graph would take. The configuration carries no
	 * width or height: the pool's buffers have them.
	 */
	if (value != NULL && CFGetTypeID(value) == CVPixelBufferPoolGetTypeID()) {
		pool = (CVPixelBufferPoolRef)value;
	}
	if (pool == NULL) {
		if (!out->said_shape) {
			out->said_shape = true;
			say("%s: no pool of the graph's in the configuration: no pictures", out->name);
		}
		return NULL;
	}
	for (unsigned i = 0; CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pb) != kCVReturnSuccess && i < HELD; i++) {
		/* a pool that will not grow that far: the oldest picture kept is the one to have */
		unsigned oldest = (out->held_next + i) % HELD;

		if (out->held[oldest] != NULL) {
			CVPixelBufferRelease(out->held[oldest]);
			out->held[oldest] = NULL;
		}
		if (!out->said_pool) {
			out->said_pool = true;
			say("%s: the graph's pool will not grow by %u: fewer pictures kept", out->name, HELD);
		}
	}
	if (pb == NULL) {
		/*
		 * The preview pool holds one buffer (MinimumBufferCount 1): while the graph still has it,
		 * there is none to be had, and this frame is not given out. Said at most every 300th frame.
		 */
		if (out->refused++ % 300 == 0) {
			say("%s: the graph's pool has no buffer to spare (%llu frames so)", out->name,
			    (unsigned long long)out->refused);
		}
		return NULL;
	}
	return pb;
}

typedef void (^pixel_buffer_handler)(CVPixelBufferRef, CMTime);

static void
let_go(struct output *out)
{
	for (unsigned i = 0; i < HELD; i++) {
		if (out->held[i] != NULL) {
			CVPixelBufferRelease(out->held[i]);
			out->held[i] = NULL;
		}
	}
}

static void
md_put(CFMutableDictionaryRef m, int key, CFTypeRef value)
{
	if (md_key[key] != NULL && value != NULL) {
		CFDictionarySetValue(m, md_key[key], value);
	}
	if (value != NULL) {
		CFRelease(value);
	}
}

/* redfin (B4-B7): what the frame's metadata say of the controls */
struct md_extra {
	int             af_status;      /* 2: searching (a photograph waits for it), 0 not */
	bool            af_locked, ae_locked, awb_locked, flash;
	float           cct;
};

/* the frame's metadata, as H10ISP's stream gives them (see md_names) */
static CFDictionaryRef
frame_metadata(const struct isp *isp, const struct redwood_camera_frame *f, const struct isp_stats *st, CFStringRef port,
    const struct md_extra *x)
{
	CFMutableDictionaryRef m;
	double exposure = f->exposure_lines * (double)f->line_ns / 1e9, analog = f->gain_unity ? (double)f->gain / f->gain_unity : 1;
	double total = analog * isp->sensor_digital * isp->digital, fps = f->frame_lines && f->line_ns ? 1e9 / ((double)f->frame_lines * f->line_ns) : 30;
	/* (green's level for a second at 1x, a quarter of it: a room's 70 or so, daylight's tens of thousands) */
	double lux = exposure > 0 && total > 0 ? st->mean[1] / (exposure * total) / 4 : 0;

	if (md_attachment == NULL) {
		return NULL;
	}
	m = (CFMutableDictionaryRef)new_dict();
	md_put(m, MD_PORT, CFRetain(port));
	md_put(m, MD_PTS, CMTimeCopyAsDictionary(CMClockMakeHostTimeFromSystemUnits(frame_time(f)), NULL));
	md_put(m, MD_EXPOSURE, number_f(exposure));
	md_put(m, MD_AGC, number_i((int64_t)(analog * 256 + 0.5)));
	md_put(m, MD_ISP_DGAIN, number_i((int64_t)(isp->digital * 256 + 0.5)));
	md_put(m, MD_SENSOR_DGAIN, number_i((int64_t)(isp->sensor_digital * 256 + 0.5)));
	md_put(m, MD_ISO_BASE, number_f(32));
	md_put(m, MD_FRAME_RATE, number_f(fps));
	md_put(m, MD_LUX, number_i((int64_t)(lux + 0.5)));
	md_put(m, MD_SNR, number_f(40 - 10 * log10(total < 1 ? 1 : total)));
	/* redfin (B4, B5, B7): as they are, not always settled and never locked */
	md_put(m, MD_AF_STATUS, number_i(x->af_status));
	md_put(m, MD_FOCUSING, number_i(0));
	md_put(m, MD_AE_STABLE, CFRetain(isp->ae_stable || x->ae_locked ? kCFBooleanTrue : kCFBooleanFalse));
	md_put(m, MD_AE_LOCKED, CFRetain(x->ae_locked ? kCFBooleanTrue : kCFBooleanFalse));
	md_put(m, MD_AWB_STABLE, CFRetain(isp->awb_stable || x->awb_locked ? kCFBooleanTrue : kCFBooleanFalse));
	md_put(m, MD_AWB_LOCKED, CFRetain(x->awb_locked ? kCFBooleanTrue : kCFBooleanFalse));
	md_put(m, MD_AF_LOCKED, CFRetain(x->af_locked ? kCFBooleanTrue : kCFBooleanFalse));
	if (f->focus != REDWOOD_CAMERA_NO_FOCUS) {
		/* (the kernel's position, 0 infinity to 1023 macro; and AVFoundation's lens position, 0 nearest to 1 furthest) */
		md_put(m, MD_FOCUS_POSITION, number_i(f->focus));
		md_put(m, MD_LENS_POSITION, number_f(f->focus <= AF_NEAR ? 1 : f->focus >= AF_FAR ? 0 :
		    1 - (double)(f->focus - AF_NEAR) / (AF_FAR - AF_NEAR)));
	}
	{
		/* (the gains as AVFoundation has them, the least 1; in 1024ths) */
		float least = isp->wb[0] < isp->wb[2] ? isp->wb[0] : isp->wb[2];

		least = least < isp->wb[1] ? least : isp->wb[1];
		least = least > 0 ? least : 1;
		md_put(m, MD_AWB_R, number_i((int64_t)(isp->wb[0] / least * 1024 + 0.5)));
		md_put(m, MD_AWB_G, number_i((int64_t)(isp->wb[1] / least * 1024 + 0.5)));
		md_put(m, MD_AWB_B, number_i((int64_t)(isp->wb[2] / least * 1024 + 0.5)));
	}
	if (x->cct > 0) {
		md_put(m, MD_AWB_CCT, number_i((int64_t)(x->cct + 0.5)));
		md_put(m, MD_CCT, number_i((int64_t)(x->cct + 0.5)));
	}
	/*
	 * The zoom the picture was made with: the source node takes it for what the ISP applied
	 * (-[BWMultiStreamCameraSourceNode _ispAppliedZoomFactorFromSampleBufferMetadataDictionary:outputIndex:])
	 * and would make up the rest of what was asked for itself; without it, a zoomed preview stood still.
	 */
	md_put(m, MD_ZOOM, number_f(isp->zoom));
	return m;
}

/*
 * A picture of the frame into a buffer of the graph's (the scalers' moving picture, or the photograph: every pixel
 * of the sensor, isp_render_still_nv12), in pieces on every core. redfin (B8): without the stream's lock, so that the
 * capture stack's property calls do not wait for it (a 12 MP photograph took a second or more).
 */
static bool
render_into(struct output *out, CVPixelBufferRef pb, const struct isp *isp, const struct redwood_camera_frame *f, const uint8_t *raw,
    bool still)
{
	uint32_t pf = CVPixelBufferGetPixelFormatType(pb), width = (uint32_t)CVPixelBufferGetWidth(pb),
	    height = (uint32_t)CVPixelBufferGetHeight(pb);

	if (width == 0 || height == 0 || (pf != PF_420F && pf != PF_420V && pf != PF_BGRA)) {
		if (out->delivered == 0 && out->said_shape == false) {
			out->said_shape = true;
			say("%s: %ux%u %c%c%c%c buffers are not for this ISP", out->name, width, height, pf >> 24, pf >> 16 & 255,
			    pf >> 8 & 255, pf & 255);
		}
		return false;
	}
	CVPixelBufferLockBaseAddress(pb, 0);

	struct render_job job = { isp, f, raw, pb, pf, width, height, 8, still && pf != PF_BGRA };
	uint64_t began = mach_absolute_time();

	dispatch_apply_f(job.pieces, dispatch_get_global_queue(still ? QOS_CLASS_USER_INITIATED : QOS_CLASS_USER_INTERACTIVE, 0), &job,
	    render_piece);
	if (job.still) {
		static mach_timebase_info_data_t tb;

		if (tb.denom == 0) {
			mach_timebase_info(&tb);
		}
		say("%s: %ux%u made of every pixel in %.0f ms", out->name, width, height,
		    (double)(mach_absolute_time() - began) * tb.numer / tb.denom / 1e6);
	}
	CVPixelBufferUnlockBaseAddress(pb, 0);
	return true;
}

/* redfin (B8): the picture made once for both scalers where their buffers are alike: copied, not made again */
static bool
copy_picture(CVPixelBufferRef from, CVPixelBufferRef to)
{
	size_t planes = CVPixelBufferGetPlaneCount(from);

	if (CVPixelBufferGetPixelFormatType(from) != CVPixelBufferGetPixelFormatType(to) ||
	    CVPixelBufferGetWidth(from) != CVPixelBufferGetWidth(to) || CVPixelBufferGetHeight(from) != CVPixelBufferGetHeight(to) ||
	    planes != CVPixelBufferGetPlaneCount(to)) {
		return false;
	}
	CVPixelBufferLockBaseAddress(from, kCVPixelBufferLock_ReadOnly);
	CVPixelBufferLockBaseAddress(to, 0);
	for (size_t p = 0; p < (planes ? planes : 1); p++) {
		const uint8_t *a = planes ? CVPixelBufferGetBaseAddressOfPlane(from, p) : CVPixelBufferGetBaseAddress(from);
		uint8_t *b = planes ? CVPixelBufferGetBaseAddressOfPlane(to, p) : CVPixelBufferGetBaseAddress(to);
		size_t sa = planes ? CVPixelBufferGetBytesPerRowOfPlane(from, p) : CVPixelBufferGetBytesPerRow(from);
		size_t sb = planes ? CVPixelBufferGetBytesPerRowOfPlane(to, p) : CVPixelBufferGetBytesPerRow(to);
		size_t rows = planes ? CVPixelBufferGetHeightOfPlane(from, p) : CVPixelBufferGetHeight(from);

		for (size_t y = 0; a != NULL && b != NULL && y < rows; y++) {
			memcpy(b + y * sb, a + y * sa, sa < sb ? sa : sb);
		}
	}
	CVPixelBufferUnlockBaseAddress(to, 0);
	CVPixelBufferUnlockBaseAddress(from, kCVPixelBufferLock_ReadOnly);
	return true;
}

static void
attach(CVPixelBufferRef pb, CFDictionaryRef metadata)
{
	CVBufferSetAttachment(pb, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_601_4, kCVAttachmentMode_ShouldPropagate);
	CVBufferSetAttachment(pb, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
	CVBufferSetAttachment(pb, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2,
	    kCVAttachmentMode_ShouldPropagate);
	if (metadata != NULL) {
		CFMutableDictionaryRef m = CFDictionaryCreateMutableCopy(NULL, 0, metadata);

		CVBufferSetAttachment(pb, md_attachment, m, kCVAttachmentMode_ShouldPropagate);
		CFRelease(m);
	}
}

/* the picture to the graph (under the stream's lock, the graph running, the handler the one the buffer was taken for) */
static void
give(struct output *out, CVPixelBufferRef pb, const struct redwood_camera_frame *f, bool still)
{
	CMTime pts = CMClockMakeHostTimeFromSystemUnits(frame_time(f));

	((pixel_buffer_handler)out->handler)(pb, pts);
	/*
	 * The moving picture's buffers are kept for HELD frames after they are given. The preview's pool
	 * would otherwise hand a buffer straight back -- the graph lets go of it as soon as it has passed
	 * it on -- while it is still on its way to the screen (the preview sink, Camera.app's image queue,
	 * the window server copying it), and the next frame drawn into it showed as bands of two frames,
	 * torn across. Kept, they make the pool take others. (One kept was not enough.)
	 */
	if (!still) {
		if (out->held[out->held_next] != NULL) {
			CVPixelBufferRelease(out->held[out->held_next]);
		}
		out->held[out->held_next] = pb;
		out->held_next = (out->held_next + 1) % HELD;
	} else {
		CVPixelBufferRelease(pb);
	}
	if (out->delivered++ == 0) {
		uint32_t pf = CVPixelBufferGetPixelFormatType(pb);

		say("%s: the first picture given (%zux%zu %c%c%c%c)", out->name, CVPixelBufferGetWidth(pb), CVPixelBufferGetHeight(pb),
		    pf >> 24, pf >> 16 & 255, pf >> 8 & 255, pf & 255);
	}
}

/*
 * redfin (B8): the photograph made on a queue of its own, from a copy of the frame and of the processor's state, while
 * the worker goes on with the moving picture; given to the graph only if the graph still runs and still has the
 * handler the buffer was taken for.
 */
struct still_job {
	CMBaseObjectRef o;
	int             output;
	CVPixelBufferRef pb;
	void            *handler;
	struct isp      isp;
	uint8_t         *frame;         /* the header and the RAW10 */
	CFDictionaryRef metadata;
};

static void
still_made(void *context)
{
	struct still_job *j = context;
	struct stream *s = stream_of(j->o);
	struct output *out = &s->out[j->output];
	const struct redwood_camera_frame *f = (const void *)j->frame;
	bool made = render_into(out, j->pb, &j->isp, f, j->frame + f->header_bytes, true);

	if (made) {
		attach(j->pb, j->metadata);
	}
	pthread_mutex_lock(&s->lock);
	if (made && s->graph_running && out->handler == j->handler) {
		give(out, j->pb, f, true);
		say("the photograph is given (%llu so far)", (unsigned long long)++s->stills_given);
	} else {
		CVPixelBufferRelease(j->pb);
		say("a photograph made and not given (%s)", made ? "the graph stopped or changed" : "its buffer is not for this ISP");
	}
	pthread_mutex_unlock(&s->lock);
	CFRelease(j->handler);
	if (j->metadata != NULL) {
		CFRelease(j->metadata);
	}
	free(j->frame);
	CFRelease(j->o);
	free(j);
}

static dispatch_queue_t
still_queue(void)
{
	static dispatch_once_t once;
	static dispatch_queue_t queue;

	dispatch_once(&once, ^{
		queue = dispatch_queue_create("redwood.camera-still", DISPATCH_QUEUE_SERIAL);
	});
	return queue;
}

/*
 * The scene's light as the cameras' tuning measures it (Qualcomm's lux index, higher is darker): about
 * 46 a doubling of exposure times gain, 197 at a 33 ms exposure and 1x -- the black level's ranges in
 * the tuning of the wide camera pair lux indexes with gains, 195-200 with 1-1.1x and 380-385 with
 * 15.9-16x, at the longest exposure a frame at 30 a second allows.
 */
static float
lux_index(const struct isp *isp, const struct redwood_camera_frame *f)
{
	double exposure = f->exposure_lines * (double)f->line_ns / 1e9 * f->gain / f->gain_unity * isp->digital;

	return (float)(197 + 46 * log2(exposure > 1e-6 ? exposure / 0.0333 : 1e-6 / 0.0333));
}

/*
 * The light's colour temperature: where the white balance is on the line of the tuning's reference
 * lights; without them, guessed from how much more the blue is raised than the red, by what is typical of a phone's sensor --
 * blue's gain over red's about 0.8 in daylight (6500 K), 1.3 under a fluorescent tube (4000 K), 2 under
 * a filament lamp (2850 K) -- between those on the logarithm of the ratio, and no further.
 */
static float
colour_temperature(const struct isp *isp)
{
	static const float ratio[3] = { 0.8f, 1.3f, 2.0f }, kelvin[3] = { 6500, 4000, 2850 };
	float r = isp->wb[0] > 0 ? isp->wb[2] / isp->wb[0] : 1, cct = isp_cct(isp);

	if (cct > 0) {
		return cct;     /* (by the tuning's reference lights) */
	}

	if (r <= ratio[0]) {
		return kelvin[0];
	}
	for (int i = 1; i < 3; i++) {
		if (r <= ratio[i]) {
			float t = logf(r / ratio[i - 1]) / logf(ratio[i] / ratio[i - 1]);

			return kelvin[i - 1] + t * (kelvin[i] - kelvin[i - 1]);
		}
	}
	return kelvin[2];
}

/*
 * Focus, where the camera has a lens motor (the frames' focus is a position, not REDWOOD_CAMERA_NO_FOCUS:
 * the wide camera's): by contrast, as there is nothing else to go by -- the sensor's phase-detection
 * pixels are not read. A search climbs from where the lens is: a step of AF_STEP one way, on that way
 * while the middle of the picture gets sharper, back the other way if the first step was not, until
 * two steps in a row are less sharp than the sharpest (or the travel, AF_NEAR to AF_FAR, ends); then
 * around the sharpest in steps of AF_FINE, and there it stays. Each place is measured two frames after
 * the lens was told to go there (it settles in less than one). The lens is only ever moved a step at a
 * time: thrown across its travel it knocked, audibly, against its stop at 0. In focus, it stays until
 * the middle has been a good deal less sharp or quite differently lit for a second, and then until
 * the picture has kept still for a third of one: no searching while the phone is being moved.
 * A focus asked for (a tap: AutoFocusNow, on the window AFWindowParams gave) sweeps instead: the
 * lens is walked to the nearer end of its travel, then measured towards the other in steps of
 * AF_STEP until it is past the sharpest (two steps in a row well under it), then around the sharpest
 * -- it finds what was tapped however far out of focus it was, where the climb gave up on the flat
 * of the curve.
 */
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
};

/* whether focus is looking for the sharpest, or the lens is on its way somewhere (AFStatus; a photograph waits for it) */
static bool
af_searching(const struct af *af)
{
	return af->state == AF_CLIMB || af->state == AF_SWEEP || af->state == AF_FINE_SEARCH || af->state == AF_SETTLE ||
	    af->state == AF_MANUAL || (af->state == AF_STEADY && af->now) || af->at != af->target;
}

static void
af_go(int device, struct af *af, uint32_t target)
{
	struct redwood_camera_command c = { .command = REDWOOD_CAMERA_FOCUS };

	af->target = target < AF_NEAR ? AF_NEAR : target > AF_FAR ? AF_FAR : target;
	c.a = af->at < af->target ? (af->target - af->at > AF_STEP ? af->at + AF_STEP : af->target) :
	    (af->at - af->target > AF_STEP ? af->at - AF_STEP : af->target);
	af->at = c.a;
	/*
	 * The lens settles in a few milliseconds; what is spoilt is the frame being exposed while it moved
	 * (written at a frame's end, the next one's exposure has begun): one frame let go by before a
	 * measurement, none on the way.
	 */
	af->wait = af->at == af->target ? 1 : 0;
	af->steps++;
	af->sum_sharpness = af->sum_level = 0;
	af->measured = 0;
	(void)camera_client_write(device, &c, sizeof(c));        /* sunfish (f2): to camd */
}

static void
af_frame(int device, struct af *af, const struct redwood_camera_frame *f, const uint8_t *raw, const float window[4], bool locked,
    int mode, int32_t manual)
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
		if (locked || (af->sharpness > 0 && sharpness > af->sharpness * 0.6f && level > af->level * 0.7f &&
		    level < af->level * 1.4f)) {
			af->unlike = 0;
			return;
		}
		if (af->sharpness > 0 && ++af->unlike < 30) {
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
		if (af->sweep) {
			af->sweep = false;
			af->state = AF_SWEEP;
			af->best_sharpness = -1;
			af->worse = 0;
			af->steps = 0;
			af->way = af->at - AF_NEAR < AF_FAR - af->at ? 1 : -1;
			af_go(device, af, af->way > 0 ? AF_NEAR : AF_FAR);     /* (walked there a step at a time) */
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
		af->state = AF_FINE_SEARCH;
		af->fine_to = af->best + 2 * AF_FINE > AF_FAR ? AF_FAR : af->best + 2 * AF_FINE;
		af_go(device, af, af->best > 2 * AF_FINE + AF_NEAR ? af->best - 2 * AF_FINE : AF_NEAR);
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
		if (af->worse < 2 && (int)af->at + af->way * AF_STEP >= AF_NEAR && (int)af->at + af->way * AF_STEP <= AF_FAR) {
			af_go(device, af, (uint32_t)((int)af->at + af->way * AF_STEP));
			return;
		}
		af->state = AF_FINE_SEARCH;
		af->fine_to = af->best + 2 * AF_FINE > AF_FAR ? AF_FAR : af->best + 2 * AF_FINE;
		af_go(device, af, af->best > 2 * AF_FINE + AF_NEAR ? af->best - 2 * AF_FINE : AF_NEAR);
		return;
	case AF_FINE_SEARCH:
		if (sharpness > af->best_sharpness) {
			af->best = af->at;
			af->best_sharpness = sharpness;
		}
		if (af->at + AF_FINE <= af->fine_to) {
			af_go(device, af, af->at + AF_FINE);
			return;
		}
		af->state = AF_SETTLE;
		af_go(device, af, af->best);
		return;
	case AF_SETTLE:
		af->state = AF_WATCH;
		af->sharpness = sharpness;
		af->level = level;
		af->unlike = 0;
		af->hold = mode == AF_MODE_AUTO;        /* (redfin, B5: one search, and the lens stays) */
		say("focus: the lens at %u after %u moves (sharpness %.3f)%s", af->at, af->steps, sharpness, af->hold ? ", held" : "");
		return;
	case AF_MANUAL:
		return;
	}
}

/* the sensor's colour tuning, when the image has one (fw/camera-<sensor>.isp, tools/redfin-camtune.py) */
static void
load_tuning(struct isp *isp, uint32_t sensor)
{
	/* redfin: the kernel's sensors 1 back IMX363, 2 front IMX355, 3 ultra-wide IMX481 (xnu 0923); no macro camera */
	static const char *const names[] = { "none", "imx363", "imx355", "imx481" };
	char path[96];

	if (sensor >= sizeof(names) / sizeof(names[0])) {
		return;
	}
	snprintf(path, sizeof(path), "/private/var/local/fw/camera-%s.isp", names[sensor]);
	if (isp_load_tuning(isp, path)) {
		say("the %s's colour tuning from %s: %u colour matrices' lights, %u lens shadings'", names[sensor], path,
		    isp->tuning->ccm.lights, isp->tuning->lsc.lights);
	} else {
		say("no colour tuning for the %s (%s): no colour matrix, no lens shading", names[sensor], path);
		isp_set_ccm(isp, (const float[9]){ 1, 0, 0, 0, 1, 0, 0, 0, 1 });
		isp->lsc.on = false;
	}
}

static const char *
mode_name(uint32_t kind)
{
	return kind == REDWOOD_CAMERA_MODE_PHOTO ? "photo" : kind == REDWOOD_CAMERA_MODE_VIDEO ? "video" :
	    kind == REDWOOD_CAMERA_MODE_VIDEO60 ? "video60" : "auto";
}

/* the start of the frame's exposure, where the kernel gives it (reserved[1..2]), else when it was done */
static uint64_t
frame_time(const struct redwood_camera_frame *f)
{
	uint64_t sof = (uint64_t)f->reserved[REDWOOD_CAMERA_R_SOF_HI] << 32 | f->reserved[REDWOOD_CAMERA_R_SOF_LO];

	return sof != 0 && sof <= f->time ? sof : f->time;
}

/* whether the frames are of the kind of mode the format wants (by their proportions and rate: the kernel says no more) */
static bool
mode_fits(const struct redwood_camera_frame *f, uint32_t kind)
{
	double aspect = f->height ? (double)f->width / f->height : 0;
	uint32_t fps_milli = f->reserved[REDWOOD_CAMERA_R_FPS_MILLI];

	switch (kind) {
	case REDWOOD_CAMERA_MODE_PHOTO:
		return aspect < 1.5;
	case REDWOOD_CAMERA_MODE_VIDEO:
		return aspect > 1.6;
	case REDWOOD_CAMERA_MODE_VIDEO60:
		return aspect > 1.6 && (fps_milli == 0 || fps_milli >= 59000);
	default:
		return true;
	}
}

/*
 * The frame rate AE keeps to (redfin, B3): no slower than MinimumFrameRate -- but not below 15 a second unless the
 * graph asked for less than the format's 2 on purpose: the formats say 2, and AVFoundation passes that on as the
 * default --, no faster than MaximumFrameRate, as frame lengths in lines.
 */
static void
fps_limits(const struct stream *s, const struct redwood_camera_frame *f, struct isp *isp)
{
	double min_fps = s->min_fps_set && (s->min_fps > 2.5 || s->limit_ae_range) ? s->min_fps : 15, max_fps = s->max_fps;

	isp->max_frame_lines = 0;
	isp->min_frame_lines = 0;
	if (f->line_ns == 0) {
		return;
	}
	if (min_fps > 0) {
		double lines = 1e9 / (min_fps * f->line_ns);

		isp->max_frame_lines = lines < f->frame_lines_min ? f->frame_lines_min : f->frame_lines_max && lines > f->frame_lines_max ?
		    f->frame_lines_max : (uint32_t)lines;
	}
	if (max_fps > 0) {
		double lines = ceil(1e9 / (max_fps * f->line_ns));

		isp->min_frame_lines = lines > f->frame_lines_min ? (uint32_t)lines : 0;
		if (isp->max_frame_lines && isp->min_frame_lines > isp->max_frame_lines) {
			isp->max_frame_lines = isp->min_frame_lines;
		}
	}
}

static bool
worker_wanted(const struct stream *s, unsigned generation)
{
	return s->running && s->generation == generation;
}

static bool worker_unwanted_open(const struct stream *s);

/* sunfish (f3): given back (graph not started, no request since) for 3 s on end: a streaming worker stops too */
static bool
worker_given_back_long(const struct stream *s)
{
	return worker_unwanted_open(s) && clock_gettime_nsec_np(CLOCK_MONOTONIC) - s->given_back_at >= 3000000000ull;
}

/* a pause of `ms', cut short when the worker is not wanted any more */
static void
worker_pause(const struct stream *s, unsigned generation, unsigned ms)
{
	for (unsigned waited = 0; waited < ms && worker_wanted(s, generation) && !worker_unwanted_open(s); waited += 50) {
		usleep(50000);
	}
}

/*
 * sunfish (f3): camd's socket may not be there yet at boot, so the open fails and the worker retries; if the
 * streams were given back meanwhile (and the graph never started), a late open streamed for nobody (cam26: 7034
 * frames). So a given-back stream is not opened again, and an open that lands after the give-back is closed at
 * once. Off: /var/local/camdev.reopen exists (the old way: reopen until stopped).
 */
static bool
worker_unwanted_open(const struct stream *s)
{
	static int off = -1;

	if (off < 0) {
		off = access("/var/local/camdev.reopen", F_OK) == 0;
	}
	return !off && s->given_back && !s->graph_running;
}

/* one worker has the kernel's camera open at a time: the next waits for the last to close it */
static pthread_mutex_t device_owner = PTHREAD_MUTEX_INITIALIZER;

/* redfin (B6): a photograph with the flash, on its way */
enum { SHOT_IDLE, SHOT_EXPOSING, SHOT_FIRING };
struct shot {
	int             state;
	unsigned        frames, waited;
	uint32_t        lines, gain;    /* the exposure asked for it */
};

struct worker_start {
	CMBaseObjectRef o;
	unsigned        generation;
};

/*
 * The frames of one opening of the kernel's camera: until the worker is not wanted, or the kernel's camera fails
 * (then the caller opens it again, a little later). The frame's size is learned from its header: a buffer that
 * is too small is made larger (a mode with more pixels: MODE_CHANGED), one that is larger is used as it is.
 */
static const char *
frames(CMBaseObjectRef o, unsigned generation, int device)
{
	struct stream *s = stream_of(o);
	struct redwood_camera_frame head;
	struct isp isp;
	struct isp_stats st;
	uint8_t *buffer = malloc(sizeof(head));
	size_t bytes = sizeof(head);
	uint32_t tuned_for = ~0u, width = 0, height = 0;
	bool switched = false, mode_asked = false;
	float lux = -1, cct = -1;
	struct af af = { 0 };
	struct shot shot = { 0 };
	unsigned failures = 0;
	const char *why = "stopped";

	isp_init(&isp);
	while (buffer != NULL && worker_wanted(s, generation) && !worker_given_back_long(s)) {
		ssize_t n = camera_client_read(device, buffer, bytes);     /* sunfish (f2): camd's slots */
		const struct redwood_camera_frame *f = (const void *)buffer;
		size_t need;

		if (n < 0 && errno == EIO) {
			why = "the kernel's camera has had a fault";
			break;
		}
		if (n < (ssize_t)offsetof(struct redwood_camera_frame, reserved) || memcmp(f->magic, REDWOOD_CAMERA_MAGIC, 8) != 0 ||
		    f->header_bytes < offsetof(struct redwood_camera_frame, reserved) || f->header_bytes > 4096 ||
		    f->stride < f->width * 10 / 8 || f->height == 0) {
			if (!(n < 0 && (errno == EAGAIN || errno == EINTR)) && ++failures >= 5) {
				why = n < 0 ? strerror(errno) : "frames that are not frames";
				break;
			}
			continue;
		}
		need = f->header_bytes + (size_t)f->stride * f->height;
		if (f->sensor != s->camera->sensor) {
			/* another camera's frames (the kernel's last sensor): changed, and the next ones are this one's */
			if (!switched) {
				struct redwood_camera_command c = { .command = REDWOOD_CAMERA_SENSOR, .a = s->camera->sensor, .b = REDWOOD_CAMERA_MODE_ANY,
					.c = s->camera->formats[s->format_index & 1].kind };

				switched = mode_asked = true;
				s->mode_wanted = false;
				say("the kernel's frames are camera %u's: to %u, the %s (a %s mode)", f->sensor, s->camera->sensor, s->camera->name,
				    mode_name(c.c));
				(void)camera_client_write(device, &c, sizeof(c));        /* sunfish (f2): to camd */
			}
			continue;
		}
		if (need > bytes) {
			uint8_t *larger = realloc(buffer, need);

			if (larger == NULL) {
				why = "no memory for a frame";
				break;
			}
			buffer = larger;
			bytes = need;
			continue;       /* (this frame is lost: its bytes did not fit) */
		}
		if ((size_t)n < need) {
			if (++failures >= 5) {
				why = "frames shorter than their headers say";
				break;
			}
			continue;
		}
		failures = 0;
		if (f->width != width || f->height != height || (f->reserved[REDWOOD_CAMERA_R_FLAGS] & REDWOOD_CAMERA_MODE_CHANGED)) {
			say("frames of %ux%u from the kernel (the %s, %.1f fps mode%s)", f->width, f->height, s->camera->name,
			    f->reserved[REDWOOD_CAMERA_R_FPS_MILLI] / 1000.0, f->reserved[REDWOOD_CAMERA_R_FLAGS] & REDWOOD_CAMERA_MODE_CHANGED ?
			    ", a new mode" : "");
			width = f->width;
			height = f->height;
		}
		/* the format's kind of mode (B3): asked once a format, if the frames are not of it */
		if (s->mode_wanted || (!mode_asked && !mode_fits(f, s->camera->formats[s->format_index & 1].kind))) {
			struct redwood_camera_command c = { .command = REDWOOD_CAMERA_SENSOR, .a = s->camera->sensor, .b = REDWOOD_CAMERA_MODE_ANY,
				.c = s->camera->formats[s->format_index & 1].kind };

			s->mode_wanted = false;
			mode_asked = true;
			say("the format wants a %s mode: asked of the kernel", mode_name(c.c));
			(void)camera_client_write(device, &c, sizeof(c));        /* sunfish (f2): to camd */
		}

		const uint8_t *raw = buffer + f->header_bytes;
		struct redwood_camera_command c;
		float window[4];
		bool af_locked, af_now, give_still = false, has_flash = s->camera->sensor != 2;
		int af_mode, flash = 0;
		int32_t manual_focus;

		/* redfin (B4-B7): the controls as the capture stack has set them, for this frame */
		pthread_mutex_lock(&s->ctl_lock);
		isp.ae_locked = s->ae_locked || shot.state != SHOT_IDLE;
		isp.ae_bias = s->ae_bias;
		memcpy(isp.ae_roi, s->ae_roi, sizeof(isp.ae_roi));
		isp.ae_max_gain = s->ae_max_gain;
		isp.ae_max_exposure_us = s->ae_max_exposure_us;
		isp.manual = s->ae_manual;
		isp.manual_us = s->manual_us;
		isp.manual_gain = s->manual_gain;
		isp.awb_locked = s->awb_locked;
		isp.awb_manual = s->awb_manual;
		memcpy(isp.wb_manual, s->wb_manual, sizeof(isp.wb_manual));
		memcpy(window, s->af_window, sizeof(window));
		af_locked = s->af_locked;
		af_mode = s->af_mode;
		manual_focus = s->manual_focus;
		af_now = s->af_now;
		s->af_now = false;
		pthread_mutex_unlock(&s->ctl_lock);
		if (f->focus != REDWOOD_CAMERA_NO_FOCUS && !s->has_focus) {
			s->has_focus = true;
			say("the %s's lens moves (the kernel says where it is: %u)", s->camera->name, f->focus);
		}

		isp_measure(&isp, f, raw, &st);
		isp_awb(&isp, &st);
		if (f->sensor != tuned_for) {
			tuned_for = f->sensor;
			load_tuning(&isp, f->sensor);
		}
		/*
		 * Slowly, a second or so: the colour temperature is guessed from the white balance, the tuning it
		 * picks changes what the white balance sees, and the colour matrix makes every swing of it plain.
		 */
		lux = lux < 0 ? lux_index(&isp, f) : lux * 0.9f + lux_index(&isp, f) * 0.1f;
		cct = cct < 0 ? colour_temperature(&isp) : cct * 0.97f + colour_temperature(&isp) * 0.03f;
		isp_tune(&isp, lux, cct);
		isp.zoom = s->zoom;
		isp.turned = s->camera->turned;
		fps_limits(s, f, &isp);
		if (isp_ae(&isp, f, &st, &c)) {
			(void)camera_client_write(device, &c, sizeof(c));        /* sunfish (f2): to camd */
		}
		if (af_now) {
			/* (whatever it was doing: at once, from the next frame; a tap sweeps, continuous focus climbs) */
			af.state = AF_STEADY;
			af.now = true;
			af.sweep = af_mode != AF_MODE_CONTINUOUS;
			af.hold = false;
			af.manual_at = -1;
		}
		af_frame(device, &af, f, raw, window, af_locked, af_mode, manual_focus);
		s->frames++;

		/*
		 * redfin (B6): a photograph. Not while focus is still searching (up to 20 frames); with the flash (asked for,
		 * or FlashMode on, or auto in the dark), the exposure is made shorter for it first (the flash's own light about
		 * 2 EV at a photograph's distance: a quarter of the exposure, at most 1/60 s), then the kernel lights the next
		 * full frame (FLASH) and the frame that carries FLASH_LIT is the photograph; a kernel that never says so has
		 * the photograph without flash after 8 frames.
		 */
		pthread_mutex_lock(&s->lock);
		unsigned stills_wanted = s->graph_running ? s->stills_wanted : 0;
		int wish = s->still_flash;

		pthread_mutex_unlock(&s->lock);
		if (stills_wanted == 0) {
			shot.state = SHOT_IDLE;
			shot.waited = 0;
		} else if (shot.state == SHOT_IDLE) {
			flash = wish >= 0 ? wish : s->flash_mode;
			flash = flash == 2 ? lux > 335 : flash;         /* (auto: about 8x at 1/30 s and darker) */
			if (af_searching(&af) && af.state != AF_WATCH && shot.waited++ < 20) {
				/* (focus first) */
			} else if (!flash || !has_flash) {
				give_still = true;
			} else {
				uint32_t sixtieth = f->line_ns ? (uint32_t)(1e9 / 60 / f->line_ns) : f->exposure_lines;
				double unity = f->gain_unity ? f->gain_unity : 256, total = f->exposure_lines * (double)f->gain / unity * isp.digital;
				struct redwood_camera_command e = { .command = REDWOOD_CAMERA_EXPOSURE };

				e.a = f->exposure_lines < sixtieth ? f->exposure_lines : sixtieth;
				e.a = e.a < 4 ? 4 : e.a;
				double g = total / 4 / e.a, most = f->gain_max > f->gain_unity ? (double)f->gain_max / unity : 1;

				g = g < 1 ? 1 : g > most ? most : g;
				e.b = (uint32_t)(g * unity + 0.5);
				shot.lines = e.a;
				shot.gain = e.b;
				shot.state = SHOT_EXPOSING;
				shot.frames = 0;
				isp.asked = false;
				isp.digital = 1;
				(void)camera_client_write(device, &e, sizeof(e));        /* sunfish (f2): to camd */
				say("a photograph with the flash: the exposure %.1f ms at %.2fx for it", e.a * (double)f->line_ns / 1e6, g);
			}
		} else if (shot.state == SHOT_EXPOSING) {
			if ((f->exposure_lines == shot.lines && f->gain <= shot.gain + shot.gain / 50) || ++shot.frames >= 4) {
				struct redwood_camera_command l = { .command = REDWOOD_CAMERA_FLASH, .a = 300, .b = 1, .c = 0 };

				(void)camera_client_write(device, &l, sizeof(l));        /* sunfish (f2): to camd */
				shot.state = SHOT_FIRING;
				shot.frames = 0;
			}
		} else if (shot.state == SHOT_FIRING) {
			if (f->reserved[REDWOOD_CAMERA_R_FLAGS] & REDWOOD_CAMERA_FLASH_LIT) {
				give_still = true;
				say("the flash lit frame %llu: the photograph", (unsigned long long)f->sequence);
			} else if (++shot.frames >= 8) {
				give_still = true;
				say("no frame lit by the flash (the kernel's FLASH): the photograph without it");
			}
		}

		struct md_extra extra = {
			.af_status = af_searching(&af) ? 2 : 0, .af_locked = af_locked || af.hold, .ae_locked = isp.ae_locked && shot.state == SHOT_IDLE,
			.awb_locked = isp.awb_locked || isp.awb_manual, .cct = cct, .flash = give_still && shot.state == SHOT_FIRING,
		};
		CFDictionaryRef metadata = frame_metadata(&isp, f, &st, s->port, &extra);

		/*
		 * Only once the graph has started the stream. Its PixelBufferHandler blocks hand the buffer
		 * to multiStreamCameraSourceNode_sendBufferToBufferServicingThread, which reaches into the
		 * source node's innards: called before the graph is started (or after it has been torn down)
		 * it walks a pointer that is not there yet and mediaserverd dies of SIGSEGV. The frames
		 * before that are read and thrown away, which is also what settles the exposure.
		 */
		if (metadata != NULL) {
			pthread_mutex_lock(&s->metadata_lock);
			if (s->metadata != NULL) {
				CFRelease(s->metadata);
			}
			s->metadata = CFRetain(metadata);
			pthread_mutex_unlock(&s->metadata_lock);
		}
		/*
		 * The scalers' outputs are the moving picture and are given every frame. The still image output is the whole
		 * sensor (4032x3024) and is the photograph: a frame goes to it only when the graph has asked for one, which
		 * is not something to make thirty times a second. redfin (B8): the buffers are taken under the stream's lock,
		 * the pictures made without it (once, where both scalers' buffers are alike), and given under it again --
		 * the photograph on a queue of its own.
		 */
		struct { int output; CVPixelBufferRef pb; void *handler; } moving[OUTPUTS];
		struct still_job *job = NULL;
		int taken = 0;

		pthread_mutex_lock(&s->lock);
		for (int i = 0; s->graph_running && i < OUTPUTS; i++) {
			struct output *out = &s->out[i];
			bool still = strcmp(out->name, "StillImage") == 0;
			CVPixelBufferRef pb;

			if (out->handler == NULL || out->config == NULL || !(out->enabled || i == 0)) {
				continue;
			}
			if (still && (!give_still || s->stills_wanted == 0)) {
				continue;
			}
			if ((pb = output_buffer(out)) == NULL) {
				continue;
			}
			if (!still) {
				moving[taken].output = i;
				moving[taken].pb = pb;
				moving[taken].handler = (void *)CFRetain(out->handler);
				taken++;
				continue;
			}
			s->stills_wanted--;
			give_still = false;
			if ((job = calloc(1, sizeof(*job))) == NULL || (job->frame = malloc(need)) == NULL) {
				free(job);
				job = NULL;
				CVPixelBufferRelease(pb);
				say("no memory for a photograph");
				continue;
			}
			memcpy(job->frame, buffer, need);
			job->o = (CMBaseObjectRef)CFRetain(o);
			job->output = i;
			job->pb = pb;
			job->handler = (void *)CFRetain(out->handler);
			job->isp = isp;
			job->isp.tuning = NULL;         /* (the worker's, which it may change: the picture needs none of it) */
			job->metadata = metadata != NULL ? CFRetain(metadata) : NULL;
		}
		if (give_still && s->stills_wanted > 0) {
			/* (no still image output to give it to: not asked for again and again, the flash with it) */
			s->stills_wanted--;
			say("a photograph asked for, and no still image output to give it to");
		}
		pthread_mutex_unlock(&s->lock);
		if (job != NULL || give_still) {
			shot.state = SHOT_IDLE;
			shot.waited = 0;
			isp.asked = false;      /* (AE goes on from what the frames say) */
		}
		if (job != NULL) {
			dispatch_async_f(still_queue(), job, still_made);
		}
		for (int k = 0; k < taken; k++) {
			struct output *out = &s->out[moving[k].output];
			bool made = k > 0 && moving[0].pb != NULL && copy_picture(moving[0].pb, moving[k].pb);

			if (!made && !render_into(out, moving[k].pb, &isp, f, raw, false)) {
				CVPixelBufferRelease(moving[k].pb);
				moving[k].pb = NULL;
				continue;
			}
			attach(moving[k].pb, metadata);
		}
		pthread_mutex_lock(&s->lock);
		for (int k = 0; k < taken; k++) {
			struct output *out = &s->out[moving[k].output];

			if (moving[k].pb != NULL && s->graph_running && out->handler == moving[k].handler) {
				give(out, moving[k].pb, f, false);
			} else if (moving[k].pb != NULL) {
				CVPixelBufferRelease(moving[k].pb);
			}
		}
		pthread_mutex_unlock(&s->lock);
		for (int k = 0; k < taken; k++) {
			CFRelease(moving[k].handler);
		}
		if (metadata != NULL) {
			CFRelease(metadata);
		}
		if (s->frames == 1 || s->frames % 300 == 0) {
			say("frame %llu: exposure %.1f ms, gain %.1fx, digital %.2fx, green %.0f above black; white balance R %.2f "
			    "B %.2f, lux index %.0f, about %.0f K; %.1f fps%s%s", f->sequence, f->exposure_lines * (double)f->line_ns / 1e6,
			    (double)f->gain / f->gain_unity, isp.digital, st.mean[1], isp.wb[0], isp.wb[2], lux, cct,
			    f->frame_lines && f->line_ns ? 1e9 / ((double)f->frame_lines * f->line_ns) : 0, isp.ae_locked ? ", AE locked" : "",
			    isp.manual ? ", custom exposure" : "");
		}
	}
	free(buffer);
	if (isp.tuning != NULL) {
		free(isp.tuning->file);
		free(isp.tuning);
	}
	return buffer == NULL ? "no memory" : why;
}

/*
 * The worker: the kernel's camera opened, its frames (frames()), and opened again after a fault or a run of
 * failed reads, a little later each time (redfin, B8: it used to give up after five failed reads, for good).
 */
static void *
worker(void *context)
{
	struct worker_start start = *(struct worker_start *)context;
	struct stream *s = stream_of(start.o);
	unsigned again = 0;

	free(context);
	pthread_setname_np("redwood-camera");
	pthread_mutex_lock(&device_owner);
	while (worker_wanted(s, start.generation)) {
		int device;
		const char *why;
		unsigned pause;

		if (again > 0 && worker_unwanted_open(s)) {     /* sunfish (f3) */
			say("reopen skipped: stream given back (%u)", again);
			break;
		}
		device = camera_client_open();          /* sunfish (f2): camd, not /dev/redwood-camera */
		if (device >= 0 && (!worker_wanted(s, start.generation) || worker_unwanted_open(s))) {
			say("opened after stop: closed at once");       /* sunfish (f3) */
			camera_client_close(device);
			break;
		}
		if (device < 0) {
			why = strerror(errno);
		} else {
			why = frames(start.o, start.generation, device);
			camera_client_close(device);
			if (worker_given_back_long(s)) {        /* sunfish (f3): STOP sent by the close */
				say("given back 3 s: stopped");
				break;
			}
		}
		if (!worker_wanted(s, start.generation)) {
			break;
		}
		pause = again < 5 ? 250u << again : 5000;
		if (again++ < 10 || again % 20 == 0) {
			say("the frames stopped (%s): the kernel's camera opened again in %u ms (%u)", why, pause, again);
		}
		worker_pause(s, start.generation, pause);
	}
	pthread_mutex_unlock(&device_owner);
	say("the frames stopped (%llu)", (unsigned long long)s->frames);
	/* sunfish (f3): gone on its own (given back): not running, so the next request starts a worker again */
	pthread_mutex_lock(&s->lock);
	if (s->generation == start.generation) {
		s->running = false;
	}
	pthread_mutex_unlock(&s->lock);
	pthread_mutex_lock(&s->worker_lock);
	s->workers--;
	pthread_cond_broadcast(&s->worker_gone);
	pthread_mutex_unlock(&s->worker_lock);
	CFRelease(start.o);
	return NULL;
}

/*
 * Reading frames from the kernel and giving them to the graph are two things: the first begins when
 * the capture graph reserves the stream (`graph` false, from the device's RequestStreams: the sensor
 * takes a moment to settle and the exposure longer), the second only when the graph starts the
 * stream itself (`graph` true, the FigCaptureStream start method).
 */
static CMBaseObjectRef all_streams[CAMERAS];    /* the device's, back, front (and ultra-wide) */
static OSStatus stream_stop(CMBaseObjectRef o);

/* the stream's workers gone, or `ms' passed: whether they are */
static bool
workers_gone(struct stream *s, unsigned ms)
{
	struct timespec until;
	struct timeval now;
	bool gone;

	gettimeofday(&now, NULL);
	until.tv_sec = now.tv_sec + (time_t)(ms / 1000);
	until.tv_nsec = now.tv_usec * 1000L + (long)(ms % 1000) * 1000000L;
	if (until.tv_nsec >= 1000000000L) {
		until.tv_sec++;
		until.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(&s->worker_lock);
	while (s->workers > 0 && pthread_cond_timedwait(&s->worker_gone, &s->worker_lock, &until) == 0) {
	}
	gone = s->workers == 0;
	pthread_mutex_unlock(&s->worker_lock);
	return gone;
}

static OSStatus
stream_begin(CMBaseObjectRef o, bool graph)
{
	struct stream *s = stream_of(o);
	struct worker_start *start;
	pthread_attr_t attributes;
	pthread_t thread;

	if (graph) {
		pthread_mutex_lock(&s->lock);
		s->graph_running = true;
		pthread_mutex_unlock(&s->lock);
		say("stream: the graph starts the stream (format %d)", s->format_index);
	}
	if (s->running) {
		return 0;
	}
	/* one camera at a time: the kernel gives one sensor's frames (the other stream's worker stopped first) */
	for (int i = 0; i < CAMERAS; i++) {
		if (all_streams[i] != NULL && all_streams[i] != o && stream_of(all_streams[i])->running) {
			say("stream: the %s's frames stop for the %s's", stream_of(all_streams[i])->camera->name, s->camera->name);
			(void)stream_stop(all_streams[i]);
		}
	}
	if ((start = malloc(sizeof(*start))) == NULL) {
		return ERR_UNSUPPORTED;
	}
	s->frames = 0;
	s->mode_wanted = false;
	start->o = o;
	start->generation = ++s->generation;
	s->running = true;
	pthread_mutex_lock(&s->worker_lock);
	s->workers++;
	pthread_mutex_unlock(&s->worker_lock);
	CFRetain(o);                    /* (the worker's, until it goes) */
	say("stream: frames begin (format %d, a %s mode)", s->format_index, mode_name(s->camera->formats[s->format_index & 1].kind));
	pthread_attr_init(&attributes);
	pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&thread, &attributes, worker, start) != 0) {
		pthread_attr_destroy(&attributes);
		s->running = false;
		pthread_mutex_lock(&s->worker_lock);
		s->workers--;
		pthread_mutex_unlock(&s->worker_lock);
		CFRelease(o);
		free(start);
		return ERR_UNSUPPORTED;
	}
	pthread_attr_destroy(&attributes);
	return 0;
}

static OSStatus
stream_start(CMBaseObjectRef o)
{
	return stream_begin(o, true);
}

static OSStatus
stream_stop(CMBaseObjectRef o)
{
	struct stream *s = stream_of(o);

	say("stream: stop (%llu frames, %llu pictures given)", (unsigned long long)s->frames,
	    (unsigned long long)s->out[0].delivered);
	pthread_mutex_lock(&s->lock);
	s->graph_running = false;
	s->stills_wanted = 0;           /* (redfin, B6: a photograph asked for and not given is not given to the next session) */
	for (int i = 0; i < OUTPUTS; i++) {
		let_go(&s->out[i]);
	}
	pthread_mutex_unlock(&s->lock);
	s->running = false;
	s->generation++;
	/* (redfin, B8: not for ever: the worker may be in the kernel's read while the sensor starts or stops; it goes when that returns) */
	if (!workers_gone(s, 1500)) {
		say("stream: its worker is still in the kernel: left to finish on its own");
	}
	return 0;
}

static void
add_absent(const void *key, const void *value, void *context)
{
	if (!CFDictionaryContainsKey(context, key)) {
		CFDictionarySetValue(context, key, value);
	}
}

/*
 * The frame's metadata into the sample buffer the graph made of our pixel buffer: the pixel buffer's
 * own dictionary (deliver()), the same object, else the last frame's; whatever the graph has put in a
 * dictionary of the sample buffer's already stays as it is.
 */
static OSStatus
stream_add_attachments(CMBaseObjectRef o, CMSampleBufferRef sbuf, CFDictionaryRef options)
{
	struct stream *s = stream_of(o);
	static bool said;
	CVImageBufferRef image = sbuf != NULL ? CMSampleBufferGetImageBuffer(sbuf) : NULL;
	CFTypeRef ours = image != NULL && md_attachment != NULL ? CVBufferGetAttachment(image, md_attachment, NULL) : NULL, theirs;
	CFMutableDictionaryRef m;

	if (!said) {
		said = true;
		say("stream: AddAttachmentsToSampleBuffer (options %s): the frames' metadata %s", options ? "given" : "none",
		    md_attachment != NULL ? "attached" : "NOT THERE");
	}
	if (md_attachment == NULL || sbuf == NULL) {
		return 0;
	}
	if (ours != NULL && CFGetTypeID(ours) == CFDictionaryGetTypeID()) {
		CFRetain(ours);
	} else {
		pthread_mutex_lock(&s->metadata_lock);
		ours = s->metadata != NULL ? CFDictionaryCreateMutableCopy(NULL, 0, s->metadata) : NULL;
		pthread_mutex_unlock(&s->metadata_lock);
	}
	if (ours == NULL) {
		return 0;
	}
	theirs = CMGetAttachment(sbuf, md_attachment, NULL);
	if (theirs == ours) {
		CFRelease(ours);
		return 0;
	}
	if (theirs != NULL && CFGetTypeID(theirs) == CFDictionaryGetTypeID()) {
		m = CFDictionaryCreateMutableCopy(NULL, 0, theirs);
		CFDictionaryApplyFunction(ours, add_absent, m);
	} else {
		m = (CFMutableDictionaryRef)CFRetain(ours);
	}
	CMSetAttachment(sbuf, md_attachment, m, kCMAttachmentMode_ShouldPropagate);
	CFRelease(m);
	CFRelease(ours);
	return 0;
}

static OSStatus
stream_invalidate(CMBaseObjectRef o)
{
	(void)stream_stop(o);
	return 0;
}

static void
stream_finalize(CMBaseObjectRef o)
{
	struct stream *s = stream_of(o);

	(void)stream_stop(o);
	for (int i = 0; i < OUTPUTS; i++) {
		if (s->out[i].config) {
			CFRelease(s->out[i].config);
		}
		if (s->out[i].handler) {
			CFRelease(s->out[i].handler);
		}
	}
	if (s->formats) {
		CFRelease(s->formats);
	}
	if (s->supported) {
		CFRelease(s->supported);
	}
	if (s->colour_info) {
		CFRelease(s->colour_info);
	}
	if (s->metadata) {
		CFRelease(s->metadata);
	}
	if (s->torch >= 0) {
		close(s->torch);
	}
}

static CFStringRef
stream_description(CMBaseObjectRef o)
{
	return CFStringCreateWithFormat(NULL, NULL, CFSTR("<RedwoodCaptureStream: the %s>"), stream_of(o)->camera->name);
}

static const struct base_class stream_base = {
	1, sizeof(struct stream), NULL, stream_invalidate, stream_finalize, stream_description, stream_copy_property, stream_set_property,
	NULL, NULL, NULL
};
static const struct stream_class stream_derived = { 2, stream_start, stream_stop, stream_add_attachments };
static const struct vtable stream_vtable = { NULL, &stream_base, &stream_derived };

static CMBaseObjectRef
stream_create(CFAllocatorRef allocator, const struct camera *camera, CFStringRef port)
{
	CMBaseObjectRef o = NULL;
	struct stream *s;

	if (derived_create(allocator, &stream_vtable, stream_class_id(), &o) != 0 || o == NULL) {
		say("the stream cannot be made");
		return NULL;
	}
	s = stream_of(o);
	memset(s, 0, sizeof(*s));
	pthread_mutex_init(&s->lock, NULL);
	pthread_mutex_init(&s->metadata_lock, NULL);
	pthread_mutex_init(&s->worker_lock, NULL);
	pthread_mutex_init(&s->ctl_lock, NULL);
	s->af_mode = AF_MODE_CONTINUOUS;
	s->manual_focus = -1;
	s->still_flash = -1;
	pthread_cond_init(&s->worker_gone, NULL);
	s->torch = -1;
	s->min_fps = 2;
	s->zoom = 1;
	s->af_window[0] = s->af_window[1] = 0.3f;
	s->af_window[2] = s->af_window[3] = 0.7f;
	s->max_fps = 30;
	s->camera = camera;
	s->port = port;
	for (int i = 0; i < OUTPUTS; i++) {
		s->out[i].name = output_names[i];
	}

	CFDictionaryRef f[2] = { format(camera->formats[0].width, camera->formats[0].height, camera->formats[0].fps, false, camera->fov,
		camera->pixel_size), format(camera->formats[1].width, camera->formats[1].height, camera->formats[1].fps, false, camera->fov,
		camera->pixel_size) };

	s->formats = CFArrayCreate(NULL, (const void **)f, 2, &kCFTypeArrayCallBacks);
	s->supported = supported(stream_props, sizeof(stream_props) / sizeof(stream_props[0]));
	CFRelease(f[0]);
	CFRelease(f[1]);
	return o;
}

/* ---- the device ------------------------------------------------------------------------------------- */

struct device {
	CFArrayRef      streams;
	CFDictionaryRef supported;
};

static OSStatus
device_copy_property(CMBaseObjectRef o, CFStringRef key, CFAllocatorRef allocator, void *out)
{
	struct device *d = derived_storage(o);
	CFTypeRef value = NULL;

	(void)allocator;
	if (key_is(key, "StreamArray")) {
		value = CFRetain(d->streams);
	} else if (key_is(key, "Clock")) {
		value = CFRetain(CMClockGetHostTimeClock());
	} else if (key_is(key, "SupportedPropertiesDictionary")) {
		value = CFRetain(d->supported);
	} else if (key_is(key, "SupportedSynchronizedStreamsGroups")) {
		value = CFArrayCreate(NULL, NULL, 0, &kCFTypeArrayCallBacks);
	} else {
		say_once("device: no answer for", key, NULL);
		return ERR_NOT_AVAILABLE;
	}
	*(CFTypeRef *)out = value;
	return 0;
}

static OSStatus
device_set_property(CMBaseObjectRef o, CFStringRef key, CFTypeRef value)
{
	(void)o;
	say_once("device: set (not used)", key, value);
	return 0;
}

static unsigned requests, relinquishes;

static OSStatus
device_request_streams(CMBaseObjectRef o, CFArrayRef streams, CFDictionaryRef options)
{
	(void)o;
	if (requests++ < 3) {
		say("device: streams asked for (%ld)%s", streams ? (long)CFArrayGetCount(streams) : 0L, options ? ", with options" : "");
		if (options != NULL) {
			say_once("device: the request's options", CFSTR("RequestStreams"), options);
		}
	}
	/*
	 * Requesting a stream is this device's cue to begin producing: mediaserverd's capture graph
	 * configures the stream (it installs the PrimaryScaler PixelBufferHandler) and then reserves the
	 * streams through here, but it never calls the FigCaptureStream start method, so start the stream's
	 * frame worker from the reservation. stream_start is idempotent (it no-ops when already running),
	 * and relinquish does not stop the worker, so the reservation thrash does not tear it down.
	 */
	if (streams != NULL) {
		CFIndex n = CFArrayGetCount(streams);

		for (CFIndex i = 0; i < n; i++) {
			CMBaseObjectRef st = (CMBaseObjectRef)CFArrayGetValueAtIndex(streams, i);

			if (st != NULL) {
				stream_of(st)->given_back = false;      /* sunfish (f3) */
				(void)stream_begin(st, false);
			}
		}
	}
	return 0;
}

static OSStatus
device_relinquish_streams(CMBaseObjectRef o, CFArrayRef streams, CFDictionaryRef options)
{
	(void)o;
	(void)options;
	/* sunfish (f3): a worker still trying to open camd gives up (a streaming one goes on: the request thrash) */
	for (CFIndex i = 0; streams != NULL && i < CFArrayGetCount(streams); i++) {
		CMBaseObjectRef st = (CMBaseObjectRef)CFArrayGetValueAtIndex(streams, i);

		if (st != NULL && !stream_of(st)->given_back) {
			stream_of(st)->given_back_at = clock_gettime_nsec_np(CLOCK_MONOTONIC);
			stream_of(st)->given_back = true;
		}
	}
	if (relinquishes++ < 3) {
		say("device: streams given back (%ld)", streams ? (long)CFArrayGetCount(streams) : 0L);
	} else if (relinquishes % 1000 == 0) {
		say("device: streams given back %u times", relinquishes);
	}
	return 0;
}

static OSStatus
device_invalidate(CMBaseObjectRef o)
{
	(void)o;
	return 0;
}

static void
device_finalize(CMBaseObjectRef o)
{
	struct device *d = derived_storage(o);

	if (d->streams) {
		CFRelease(d->streams);
	}
	if (d->supported) {
		CFRelease(d->supported);
	}
}

static CFStringRef
device_description(CMBaseObjectRef o)
{
	(void)o;
	return CFRetain(CFSTR("<RedwoodCaptureDevice: the Pixel 5's cameras>"));
}

static const struct base_class device_base = {
	1, sizeof(struct device), NULL, device_invalidate, device_finalize, device_description, device_copy_property, device_set_property,
	NULL, NULL, NULL
};
static const struct device_class device_derived = { 3, NULL, device_request_streams, device_relinquish_streams, NULL };
static const struct vtable device_vtable = { NULL, &device_base, &device_derived };

/* The vendor's device create function (as H10ISPCaptureDeviceCreate is called). */
OSStatus redwood_camera_device_create(CFAllocatorRef allocator, CFDictionaryRef options, CMBaseObjectRef *out);

OSStatus
redwood_camera_device_create(CFAllocatorRef allocator, CFDictionaryRef options, CMBaseObjectRef *out)
{
	static bool found, usable;
	CMBaseObjectRef device = NULL, stream[CAMERAS];
	CFStringRef ports[CAMERAS];
	struct device *d;
	int n = 0;

	(void)options;
	if (!found) {
		found = true;
		usable = find_functions();
	}
	if (!usable || out == NULL) {
		return ERR_UNSUPPORTED;
	}
	ports[0] = port_back;
	ports[1] = NULL;        /* sunfish (f2): camd has only the IMX363 (sensor 1); no front camera yet */
	/* redfin (B3): the ultra-wide camera, where asked for and CMCapture has its port */
	ports[2] = access("/private/var/local/camera-ultrawide", F_OK) == 0 ? port_ultra_wide : NULL;
	if (access("/private/var/local/camera-ultrawide", F_OK) == 0 && port_ultra_wide == NULL) {
		say("no ultra-wide camera: kFigCapturePortType_BackFacingSuperWideCamera is not there");
	}
	for (int i = 0; i < CAMERAS; i++) {
		if (ports[i] == NULL) {
			continue;
		}
		if ((stream[n] = stream_create(allocator, &cameras[i], ports[i])) == NULL) {
			while (n > 0) {
				CFRelease(stream[--n]);
			}
			return ERR_UNSUPPORTED;
		}
		n++;
	}
	if (derived_create(allocator, &device_vtable, device_class_id(), &device) != 0 || device == NULL) {
		while (n > 0) {
			CFRelease(stream[--n]);
		}
		say("the device cannot be made");
		return ERR_UNSUPPORTED;
	}
	d = derived_storage(device);
	memset(d, 0, sizeof(*d));
	d->streams = CFArrayCreate(NULL, (const void **)stream, n, &kCFTypeArrayCallBacks);
	for (int i = 0; i < CAMERAS; i++) {
		all_streams[i] = i < n ? stream[i] : NULL;      /* (kept by the device's array) */
	}
	d->supported = supported(device_props, sizeof(device_props) / sizeof(device_props[0]));
	for (int i = 0; i < n; i++) {
		CFRelease(stream[i]);
	}
	*out = device;
	say("the device made: %d streams, the back camera, the front one%s", n, n > 2 ? " and the ultra-wide one" : "");
	return 0;
}
