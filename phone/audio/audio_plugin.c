/*
 * SunfishAudio.driver: the output device of iOS on this machine.
 *
 * Sunfish (Pixel 4a) port of ios-redfin's RedwoodAudio.driver, which is HackiOS's
 * userland/src/audio_plugin.c (MIT, Copyright (c) 2026 OOSEX GANG, LICENSE.HackiOS).
 * Changed: names (Redwood -> Sunfish, /dev/sunfish-audio|-haptic|-mic, which do not exist yet:
 * the null sink below counts the frames as refused and I/O runs on), a fresh factory UUID, and
 * say() also goes to os_log and /var/local/camdev.log (mediaserverd's stderr goes nowhere here). Device UIDs unchanged.
 *
 * Audio devices reach the audio server's hardware layer (the "HAL" of CoreAudio, inside
 * mediaserverd) in two ways: kernel drivers through IOAudio2 -- that is how an iPhone's own
 * speaker, codec and microphones arrive, and none of it exists here -- and plug-ins in
 * /System/Library/Audio/Plug-Ins/HAL, which is how Bluetooth, AirPlay, CarPlay and USB audio
 * arrive. This is such a plug-in. Its interface is the one macOS documents for user-space
 * audio drivers (AudioServerPlugIn: a CFPlugIn with one COM-style table of functions; the HAL
 * asks for objects and their properties, then runs the I/O); the iOS SDK has no header for it,
 * so what is needed of it is declared below.
 *
 * Above the HAL sits the virtual audio device (/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin),
 * through which everything in iOS is routed. It knows an iPhone's own devices by their names
 * (their UIDs) and makes a port of each: "Speaker" becomes the speaker port, the route an iPhone
 * with nothing plugged in plays through, with a volume, the volume display and the place in
 * Control Center that go with it. So this presents four devices under those names:
 *
 *   "Speaker"   a stereo output whose samples are written to /dev/sunfish-audio, where the
 *               kernel takes them to whatever can play them (see sys/sunfish_audio.h), and a
 *               stereo input that is always silent (on an iPhone: what the amplifiers measure)
 *   "Hawking"   one silent input and nothing else. The virtual audio device will not build the
 *               speaker route without a second virtual device of its own (its type 'vhaw', a
 *               mandatory companion of the default one in the iPhone X's routing tables) whose
 *               only port is this input; without it everything stays on the null port.
 *   "Actuator"  one output of one channel: the iPhone's Taptic Engine, on which iOS plays its
 *               haptics as waveforms (through the chain 'd22_actuator_general'); its samples go to
 *               the kernel's motor driver, /dev/sunfish-haptic (see actuator_write())
 *   "Codec"     one input of one channel: on an iPhone, the audio codec that the built-in
 *               microphones are on, of which the virtual audio device makes the built-in microphone
 *               port; its samples come from the kernel, /dev/sunfish-mic (the phone's main
 *               microphone, boot-arg mic=4), which listens only while this input runs (see codec_read())
 *
 * CoreAudio's own null device ("Null_Device", described in the framework's Info.plist, playing
 * into nowhere) stays as it is: it is the only device the virtual audio device takes while it
 * starts, and ours are taken a moment later, when it says it allows all devices.
 *
 * What is played through the speaker port passes the iPhone X's chain of effects for its own
 * speakers first ('d22_speaker_general', from /Library/Audio/Tunings/D22: equalisers,
 * compressors, speaker protection), at the volume iOS has set.
 *
 * The device has no clock but the system's: it claims to play exactly SUNFISH_AUDIO_RATE frames
 * a second of mach_absolute_time(), and the HAL's I/O thread paces itself by that.
 *
 * The speaker's output has a volume control, as the iPhone's own speaker has: what is set on it
 * goes to the kernel (ioctl SUNFISH_AUDIO_SET_VOLUME), which sets the amplifiers' level by it -- on
 * the phone, so far under the loudest it allows. Its scale is 48 dB in a straight line of
 * decibels, 3 dB for each of iOS's sixteen steps; 0 is silence. What iOS does with it (the phone,
 * 15 October): the virtual audio device sets it once, to its top (0 dB), when it takes the
 * device, and leaves it there -- its routing tables keep the iPhone X's speaker in software volume
 * mode, so the volume buttons and Control Center scale the samples themselves, and at 0 what is
 * written is silence (on which the kernel keeps the amplifiers off).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <CoreAudioTypes/CoreAudioTypes.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <notify.h>
#include <dispatch/dispatch.h>
#include <sys/ioctl.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

#include "sunfish_audio.h"
#include "../camera/camera_client.h"

/* ---- the AudioServerPlugIn interface (macOS: CoreAudio/AudioServerPlugIn.h, AudioHardwareBase.h) ---- */

typedef UInt32 AudioObjectID;
typedef struct {
	UInt32 mSelector, mScope, mElement;
} AudioObjectPropertyAddress;
typedef struct {
	AudioStreamBasicDescription mFormat;
	AudioValueRange mSampleRateRange;
} AudioStreamRangedDescription;
typedef struct {
	UInt32 mClientID;
	pid_t mProcessID;
	Boolean mIsNativeEndian;
	CFStringRef mBundleID;
} AudioServerPlugInClientInfo;
typedef struct {
	UInt64 mIOCycleCounter;
	UInt32 mNominalIOBufferFrameSize;
	AudioTimeStamp mCurrentTime, mInputTime, mOutputTime;
	Float64 mMasterHostTicksPerFrame, mDeviceHostTicksPerFrame;
} AudioServerPlugInIOCycleInfo;

typedef struct AudioServerPlugInHostInterface AudioServerPlugInHostInterface;
typedef const AudioServerPlugInHostInterface *AudioServerPlugInHostRef;
struct AudioServerPlugInHostInterface {
	OSStatus (*PropertiesChanged)(AudioServerPlugInHostRef, AudioObjectID, UInt32, const AudioObjectPropertyAddress *);
	OSStatus (*CopyFromStorage)(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef *);
	OSStatus (*WriteToStorage)(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef);
	OSStatus (*DeleteFromStorage)(AudioServerPlugInHostRef, CFStringRef);
	OSStatus (*RequestDeviceConfigurationChange)(AudioServerPlugInHostRef, AudioObjectID, UInt64, void *);
};

typedef struct AudioServerPlugInDriverInterface AudioServerPlugInDriverInterface;
typedef AudioServerPlugInDriverInterface **AudioServerPlugInDriverRef;
struct AudioServerPlugInDriverInterface {
	void *_reserved;
	HRESULT (*QueryInterface)(void *, REFIID, LPVOID *);
	ULONG (*AddRef)(void *);
	ULONG (*Release)(void *);
	OSStatus (*Initialize)(AudioServerPlugInDriverRef, AudioServerPlugInHostRef);
	OSStatus (*CreateDevice)(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo *, AudioObjectID *);
	OSStatus (*DestroyDevice)(AudioServerPlugInDriverRef, AudioObjectID);
	OSStatus (*AddDeviceClient)(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
	OSStatus (*RemoveDeviceClient)(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
	OSStatus (*PerformDeviceConfigurationChange)(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
	OSStatus (*AbortDeviceConfigurationChange)(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
	Boolean (*HasProperty)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *);
	OSStatus (*IsPropertySettable)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, Boolean *);
	OSStatus (*GetPropertyDataSize)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32,
	    const void *, UInt32 *);
	OSStatus (*GetPropertyData)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32,
	    const void *, UInt32, UInt32 *, void *);
	OSStatus (*SetPropertyData)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32,
	    const void *, UInt32, const void *);
	OSStatus (*StartIO)(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
	OSStatus (*StopIO)(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
	OSStatus (*GetZeroTimeStamp)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64 *, UInt64 *, UInt64 *);
	OSStatus (*WillDoIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean *, Boolean *);
	OSStatus (*BeginIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32,
	    const AudioServerPlugInIOCycleInfo *);
	OSStatus (*DoIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32,
	    const AudioServerPlugInIOCycleInfo *, void *, void *);
	OSStatus (*EndIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32,
	    const AudioServerPlugInIOCycleInfo *);
};

#define kAudioServerPlugInTypeUUID CFUUIDGetConstantUUIDWithBytes(NULL, 0x44, 0x3A, 0xBA, 0xB8, 0xE7, 0xB3, 0x49, 0x1A, \
	    0xB9, 0x85, 0xBE, 0xB9, 0x18, 0x70, 0x30, 0xDB)
#define kAudioServerPlugInDriverInterfaceUUID CFUUIDGetConstantUUIDWithBytes(NULL, 0xEE, 0xA5, 0x77, 0x3D, 0xCC, 0x43, \
	    0x49, 0xF1, 0x8E, 0x00, 0x8F, 0x96, 0xE7, 0xD2, 0x3B, 0x17)

enum {
	kAudioHardwareUnknownPropertyError = 'who?', kAudioHardwareBadPropertySizeError = '!siz',
	kAudioHardwareIllegalOperationError = 'nope', kAudioHardwareBadObjectError = '!obj',
	kAudioHardwareUnsupportedOperationError = 'unop', kAudioDeviceUnsupportedFormatError = '!dat',

	kAudioObjectPropertyScopeGlobal = 'glob', kAudioObjectPropertyScopeInput = 'inpt', kAudioObjectPropertyScopeOutput = 'outp',

	kAudioObjectClassID = 'aobj', kAudioPlugInClassID = 'aplg', kAudioDeviceClassID = 'adev', kAudioStreamClassID = 'astr',

	kAudioObjectPropertyBaseClass = 'bcls', kAudioObjectPropertyClass = 'clas', kAudioObjectPropertyOwner = 'stdv',
	kAudioObjectPropertyName = 'lnam', kAudioObjectPropertyManufacturer = 'lmak', kAudioObjectPropertyOwnedObjects = 'ownd',

	kAudioPlugInPropertyBoxList = 'box#', kAudioPlugInPropertyTranslateUIDToBox = 'uidb',
	kAudioPlugInPropertyDeviceList = 'dev#', kAudioPlugInPropertyTranslateUIDToDevice = 'uidd',
	kAudioPlugInPropertyResourceBundle = 'rsrc',

	kAudioDevicePropertyDeviceUID = 'uid ', kAudioDevicePropertyModelUID = 'muid', kAudioDevicePropertyTransportType = 'tran',
	kAudioDevicePropertyRelatedDevices = 'akin', kAudioDevicePropertyClockDomain = 'clkd',
	kAudioDevicePropertyJackIsConnected = 'jack',
	kAudioDevicePropertyDeviceIsAlive = 'livn', kAudioDevicePropertyDeviceIsRunning = 'goin',
	kAudioDevicePropertyDeviceCanBeDefaultDevice = 'dflt', kAudioDevicePropertyDeviceCanBeDefaultSystemDevice = 'sflt',
	kAudioDevicePropertyLatency = 'ltnc', kAudioDevicePropertyStreams = 'stm#', kAudioObjectPropertyControlList = 'ctrl',
	kAudioDevicePropertySafetyOffset = 'saft', kAudioDevicePropertyNominalSampleRate = 'nsrt',
	kAudioDevicePropertyAvailableNominalSampleRates = 'nsr#', kAudioDevicePropertyIsHidden = 'hidn',
	kAudioDevicePropertyPreferredChannelsForStereo = 'dch2', kAudioDevicePropertyZeroTimeStampPeriod = 'ring',
	kAudioSelectorControlClassID = 'slct', kAudioDataSourceControlClassID = 'dsrc',
	kAudioSelectorControlPropertyCurrentItem = 'scci', kAudioSelectorControlPropertyAvailableItems = 'scai',
	kAudioSelectorControlPropertyItemName = 'scin', kAudioSelectorControlPropertyItemKind = 'clkk',

	kAudioStreamPropertyIsActive = 'sact', kAudioStreamPropertyDirection = 'sdir', kAudioStreamPropertyTerminalType = 'term',
	kAudioStreamPropertyStartingChannel = 'schn', kAudioStreamPropertyLatency = 'ltnc',
	kAudioStreamPropertyVirtualFormat = 'sfmt', kAudioStreamPropertyAvailableVirtualFormats = 'sfma',
	kAudioStreamPropertyPhysicalFormat = 'pft ', kAudioStreamPropertyAvailablePhysicalFormats = 'pfta',

	kAudioStreamTerminalTypeSpeaker = 'spkr', kAudioStreamTerminalTypeMicrophone = 'micr',

	kAudioServerPlugInIOOperationReadInput = 'read', kAudioServerPlugInIOOperationWriteMix = 'rite',

	kAudioLevelControlClassID = 'levl', kAudioVolumeControlClassID = 'vlme',
	kAudioControlPropertyScope = 'cscp', kAudioControlPropertyElement = 'celm',
	kAudioLevelControlPropertyScalarValue = 'lcsv', kAudioLevelControlPropertyDecibelValue = 'lcdv',
	kAudioLevelControlPropertyDecibelRange = 'lcdr', kAudioLevelControlPropertyConvertScalarToDecibels = 'lcsd',
	kAudioLevelControlPropertyConvertDecibelsToScalar = 'lcds',
};

/* ---- the devices ---------------------------------------------------------------------------------- */

/*
 * Object numbers are ours to choose. A device's streams follow it: its output (if it has one),
 * then its input.
 */
enum {
	OBJECT_PLUGIN = 1,
	OBJECT_SPEAKER, OBJECT_SPEAKER_OUTPUT, OBJECT_SPEAKER_INPUT,
	OBJECT_HAWKING, OBJECT_HAWKING_INPUT,
	OBJECT_ACTUATOR, OBJECT_ACTUATOR_OUTPUT,
	OBJECT_CODEC, OBJECT_CODEC_INPUT,
	OBJECT_SPEAKER_VOLUME,          /* the speaker output's volume control */
	OBJECT_CODEC_SOURCE,            /* the codec input's data source control: its microphones */
	OBJECT_CODEC_OUTPUT,            /* sunfish S4: the codec's output, only with JACK_OUT (last: the others keep their numbers) */
	OBJECT_LAST = OBJECT_CODEC_OUTPUT
};

#define VOLUME_RANGE_DB         48.0    /* from the loudest the kernel allows down to the quietest step above 0 */

struct device {
	AudioObjectID object;
	CFStringRef uid;                /* the name the virtual audio device knows it by */
	AudioObjectID output, input;    /* its streams; 0 = none */
	UInt32 output_channels, input_channels;
	/* its clock, running while anybody does I/O */
	unsigned running;               /* how many clients have started I/O */
	UInt64 anchor;                  /* mach_absolute_time() when the first of them did */
	UInt64 periods;                 /* whole timestamp periods since */
};

static struct device devices[] = {
	{ .object = OBJECT_SPEAKER, .uid = CFSTR("Speaker"), .output = OBJECT_SPEAKER_OUTPUT, .input = OBJECT_SPEAKER_INPUT,
	  .output_channels = SUNFISH_AUDIO_CHANNELS, .input_channels = 2 },
	{ .object = OBJECT_HAWKING, .uid = CFSTR("Hawking"), .input = OBJECT_HAWKING_INPUT, .input_channels = 1 },
	{ .object = OBJECT_ACTUATOR, .uid = CFSTR("Actuator"), .output = OBJECT_ACTUATOR_OUTPUT, .output_channels = 1 },
	{ .object = OBJECT_CODEC, .uid = CFSTR("Codec"), .input = OBJECT_CODEC_INPUT, .input_channels = 1 },
};
#define DEVICES                 (sizeof(devices) / sizeof(devices[0]))

#define TRANSPORT_BUILT_IN      'bltn'
/*
 * The codec's input has data sources, one for each microphone, and the virtual audio device will not
 * take a codec without them (VirtualAudio asks for kAudioDevicePropertyDataSources on its input, and
 * keeps those it knows: 'imic', 'smic', 'fmic', 'lmic', 'tmic', 'umic', 'imi1' to 'imi4'). A plug-in
 * has them as a data source control on the input, which the HAL answers those properties from. There
 * is one microphone here, at the bottom: 'imic', the iPhone's internal (bottom) one.
 */
#define MIC_SOURCE              'imic'
#define MANUFACTURER            CFSTR("sunfish")
/* how often a device says where its clock is, in frames: also the largest I/O buffer the HAL will use */
#define TIMESTAMP_PERIOD        16384

static AudioServerPlugInHostRef host;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int sink = -1;                   /* /dev/sunfish-audio */
static Float64 ticks_per_frame;
static UInt64 written, refused;         /* frames since the speaker's I/O started */
static Boolean inactive[OBJECT_LAST + 1];       /* streams the HAL has switched off */
static Float32 volume = 1.0f;           /* the speaker's volume control, 0 to 1 (iOS sets it once it routes there) */
/*
 * sunfish S4 (headphone jack): only with /var/local/sunfish-jack-enable (read once at Initialize), the
 * device "Codec" has 'jack' in the output scope (headphones in) and the input scope (their microphone),
 * which VirtualAudio's HALJackStateHandler reads to make the "Wired Headphones" port. The state comes from
 * the Darwin notification JACK_NOTIFY (its state: bit 0 plugged, bit 1 microphone; jackset posts it).
 */
#define JACK_ENABLE             "/var/local/sunfish-jack-enable"
#define JACK_NOTIFY             "org.iosonpixel.jack"
static bool jack_enabled;
/*
 * sunfish S4: with /var/local/sunfish-jack-out as well (read once at Initialize), "Codec" has an output
 * stream (the speaker's format, a null sink: what is played on it is dropped), for a Wired Headphones
 * port to be bound to.
 */
#define JACK_OUT                "/var/local/sunfish-jack-out"
static UInt32 jack_state;               /* under lock */
static unsigned jack_asked;             /* unknown_call() lines so far (S4) */

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
	/* sunfish: also to the unified log and /var/local/camdev.log, as the job's stderr is not kept and oslog drops
	   lines (jack8: 1 of 3) */
	camera_client_log(line);
}

/* The volume control's scale: a straight line of decibels over VOLUME_RANGE_DB. */
static Float32
scalar_to_db(Float32 scalar)
{
	scalar = scalar < 0 ? 0 : scalar > 1 ? 1 : scalar;
	return (Float32)(-VOLUME_RANGE_DB * (1.0 - scalar));
}

static Float32
db_to_scalar(Float32 db)
{
	Float32 scalar = (Float32)(1.0 + db / VOLUME_RANGE_DB);

	return scalar < 0 ? 0 : scalar > 1 ? 1 : scalar;
}

/* The volume, to the kernel. With the lock held. */
static void
volume_to_kernel(void)
{
	struct sunfish_audio_volume v = { (uint32_t)lround(-scalar_to_db(volume) * 8), volume <= 0 };

	if (sink < 0 || ioctl(sink, SUNFISH_AUDIO_SET_VOLUME, &v) < 0) {
		say("sunfish: audio: the volume %.3f could not be given to the kernel (%s)\n", volume, strerror(errno));
		return;
	}
	say("sunfish: audio: the volume is %.3f: %s%.1f dB under the loudest\n", volume, v.muted ? "silence, " : "",
	    v.below / 8.0);
}

/* The device with this object number, or the one this stream belongs to; NULL = neither. */
static struct device *
device_of(AudioObjectID object)
{
	for (unsigned i = 0; i < DEVICES; i++) {
		if (object == devices[i].object || (object != 0 && (object == devices[i].output || object == devices[i].input))) {
			return &devices[i];
		}
	}
	return NULL;
}

/* The one format of a stream: 16-bit samples at SUNFISH_AUDIO_RATE, so many channels. */
static AudioStreamBasicDescription
format_of(AudioObjectID stream)
{
	const struct device *d = device_of(stream);
	UInt32 channels = stream == d->output ? d->output_channels : d->input_channels;

	return (AudioStreamBasicDescription) {
		       .mSampleRate = SUNFISH_AUDIO_RATE, .mFormatID = kAudioFormatLinearPCM,
		       .mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
		       .mBytesPerPacket = 2 * channels, .mFramesPerPacket = 1, .mBytesPerFrame = 2 * channels,
		       .mChannelsPerFrame = channels, .mBitsPerChannel = 16,
	};
}

static HRESULT
query_interface(void *driver, REFIID uuid, LPVOID *interface)
{
	CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, uuid);
	Boolean ours = CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID);

	CFRelease(requested);
	if (interface == NULL || !ours) {
		return E_NOINTERFACE;
	}
	*interface = driver;            /* one driver, never freed: no counting */
	return S_OK;
}

static ULONG
add_ref(__unused void *driver)
{
	return 1;
}

static ULONG
release(__unused void *driver)
{
	return 1;
}

/* sunfish S4: a post of JACK_NOTIFY; tell the HAL when the state changed, outside the lock */
static void
jack_changed(int token)
{
	AudioObjectPropertyAddress changed[2] = {
		{ kAudioDevicePropertyJackIsConnected, kAudioObjectPropertyScopeOutput, 0 },
		{ kAudioDevicePropertyJackIsConnected, kAudioObjectPropertyScopeInput, 0 },
	};
	uint64_t state = 0;
	UInt32 was;

	notify_get_state(token, &state);
	pthread_mutex_lock(&lock);
	was = jack_state;
	jack_state = (UInt32)state & 3;
	pthread_mutex_unlock(&lock);
	if ((UInt32)(state & 3) == was) {
		return;
	}
	say("sunfish: audio: jack %s, microphone %s\n", state & 1 ? "plugged" : "empty", state & 2 ? "yes" : "no");
	__atomic_store_n(&jack_asked, 0, __ATOMIC_RELAXED);	/* S4: the queries that follow a change get logged too */
	if (host != NULL) {
		host->PropertiesChanged(host, OBJECT_CODEC, 2, changed);
	}
}

static void
jack_start(void)
{
	struct stat st;
	dispatch_queue_t queue;
	uint64_t state = 0;
	int token;

	if (stat(JACK_ENABLE, &st) != 0) {
		return;
	}
	queue = dispatch_queue_create("org.iosonpixel.jack", DISPATCH_QUEUE_SERIAL);
	if (notify_register_dispatch(JACK_NOTIFY, &token, queue, ^(int t) { jack_changed(t); }) != NOTIFY_STATUS_OK) {
		say("sunfish: audio: jack: %s cannot be listened to\n", JACK_NOTIFY);
		return;
	}
	notify_get_state(token, &state);
	jack_state = (UInt32)state & 3;
	jack_enabled = true;
	say("sunfish: audio: jack on (%s): %s, microphone %s\n", JACK_NOTIFY, state & 1 ? "plugged" : "empty",
	    state & 2 ? "yes" : "no");
}

static OSStatus
initialize(__unused AudioServerPlugInDriverRef driver, AudioServerPlugInHostRef the_host)
{
	mach_timebase_info_data_t timebase;
	struct stat st;

	host = the_host;
	mach_timebase_info(&timebase);
	ticks_per_frame = 1e9 * timebase.denom / timebase.numer / SUNFISH_AUDIO_RATE;
	sink = open(SUNFISH_AUDIO_DEVICE, O_WRONLY);
	say("sunfish: audio: the speaker is with the HAL (%s: %s)\n", SUNFISH_AUDIO_DEVICE, sink < 0 ? strerror(errno) : "open");
	jack_start();
	if (stat(JACK_OUT, &st) == 0) {
		/* sunfish S4: devices[] is ours until the HAL first asks */
		for (unsigned i = 0; i < DEVICES; i++) {
			if (devices[i].object == OBJECT_CODEC) {
				devices[i].output = OBJECT_CODEC_OUTPUT;
				devices[i].output_channels = SUNFISH_AUDIO_CHANNELS;
			}
		}
		say("sunfish: audio: the codec has an output (%s): dropped\n", JACK_OUT);
	}
	return 0;
}

static OSStatus
create_device(__unused AudioServerPlugInDriverRef driver, __unused CFDictionaryRef description,
    __unused const AudioServerPlugInClientInfo *client, __unused AudioObjectID *object)
{
	return kAudioHardwareUnsupportedOperationError;
}

static OSStatus
destroy_device(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object)
{
	return kAudioHardwareUnsupportedOperationError;
}

static OSStatus
device_client(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object,
    __unused const AudioServerPlugInClientInfo *client)
{
	return 0;
}

static OSStatus
configuration_change(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object, __unused UInt64 action,
    __unused void *info)
{
	return 0;
}

/* A property's value: `size` bytes of it, or a list with nothing in it, or (neither) no such property. */
struct value {
	UInt32 size;
	bool empty;
	bool string;                    /* a CFString: whoever is given it gets a reference of their own */
	union {
		UInt32 words[4];
		Float64 number;
		Float32 decimal;
		CFStringRef string;
		AudioValueRange range;
		AudioStreamBasicDescription format;
		AudioStreamRangedDescription ranged;
	} u;
};

static struct value
property(AudioObjectID object, const AudioObjectPropertyAddress *address, UInt32 qualifier_size, const void *qualifier)
{
	UInt32 selector = address->mSelector, scope = address->mScope;
	const struct device *d = device_of(object);
	struct value v;

	memset(&v, 0, sizeof(v));
#define WORD(x)         (v.u.words[0] = (x), v.size = sizeof(UInt32))
#define LIST(x)         (v.u.words[v.size / sizeof(UInt32)] = (x), v.size += sizeof(UInt32))
#define STRING(x)       (v.u.string = (x), v.string = true, v.size = sizeof(CFStringRef))
#define FLOAT(x)        (v.u.decimal = (x), v.size = sizeof(Float32))
	if (object == OBJECT_SPEAKER_VOLUME) {
		switch (selector) {
		case kAudioObjectPropertyBaseClass:
			WORD(kAudioLevelControlClassID);
			break;
		case kAudioObjectPropertyClass:
			WORD(kAudioVolumeControlClassID);
			break;
		case kAudioObjectPropertyOwner:
			WORD(OBJECT_SPEAKER);
			break;
		case kAudioObjectPropertyOwnedObjects:
			v.empty = true;
			break;
		case kAudioControlPropertyScope:
			WORD(kAudioObjectPropertyScopeOutput);
			break;
		case kAudioControlPropertyElement:
			WORD(0);
			break;
		case kAudioLevelControlPropertyScalarValue:
			FLOAT(volume);
			break;
		case kAudioLevelControlPropertyDecibelValue:
			FLOAT(scalar_to_db(volume));
			break;
		case kAudioLevelControlPropertyDecibelRange:
			v.u.range.mMinimum = -VOLUME_RANGE_DB;
			v.u.range.mMaximum = 0;
			v.size = sizeof(AudioValueRange);
			break;
		case kAudioLevelControlPropertyConvertScalarToDecibels:
		case kAudioLevelControlPropertyConvertDecibelsToScalar:
			FLOAT(0);       /* (converts what it is given: get_property_data()) */
			break;
		}
		return v;
	}
	if (object == OBJECT_CODEC_SOURCE) {
		switch (selector) {
		case kAudioObjectPropertyBaseClass:
			WORD(kAudioSelectorControlClassID);
			break;
		case kAudioObjectPropertyClass:
			WORD(kAudioDataSourceControlClassID);
			break;
		case kAudioObjectPropertyOwner:
			WORD(OBJECT_CODEC);
			break;
		case kAudioObjectPropertyOwnedObjects:
			v.empty = true;
			break;
		case kAudioControlPropertyScope:
			WORD(kAudioObjectPropertyScopeInput);
			break;
		case kAudioControlPropertyElement:
			WORD(0);
			break;
		case kAudioSelectorControlPropertyCurrentItem:
		case kAudioSelectorControlPropertyAvailableItems:
			WORD(MIC_SOURCE);
			break;
		case kAudioSelectorControlPropertyItemName:
			if (qualifier_size == sizeof(UInt32) && qualifier != NULL && *(const UInt32 *)qualifier == MIC_SOURCE) {
				STRING(CFSTR("Bottom"));
			} else {
				STRING(CFSTR(""));
			}
			break;
		}
		return v;
	}
	if (object != OBJECT_PLUGIN && d == NULL) {
		return v;
	}
	switch (selector) {
	case kAudioObjectPropertyBaseClass:
		WORD(kAudioObjectClassID);
		return v;
	case kAudioObjectPropertyClass:
		WORD(object == OBJECT_PLUGIN ? kAudioPlugInClassID : object == d->object ? kAudioDeviceClassID : kAudioStreamClassID);
		return v;
	case kAudioObjectPropertyOwner:
		WORD(object == OBJECT_PLUGIN ? 0 : object == d->object ? OBJECT_PLUGIN : d->object);
		return v;
	}
	if (object == OBJECT_PLUGIN) {
		switch (selector) {
		case kAudioObjectPropertyManufacturer:
			STRING(MANUFACTURER);
			break;
		case kAudioObjectPropertyOwnedObjects:
		case kAudioPlugInPropertyDeviceList:
			for (unsigned i = 0; i < DEVICES; i++) {
				LIST(devices[i].object);
			}
			break;
		case kAudioPlugInPropertyBoxList:
			v.empty = true;
			break;
		case kAudioPlugInPropertyTranslateUIDToBox:
			WORD(0);
			break;
		case kAudioPlugInPropertyTranslateUIDToDevice:
			WORD(0);
			for (unsigned i = 0; i < DEVICES; i++) {
				if (qualifier_size == sizeof(CFStringRef) && qualifier != NULL && *(const CFStringRef *)qualifier != NULL &&
				    CFEqual(*(const CFStringRef *)qualifier, devices[i].uid)) {
					WORD(devices[i].object);
				}
			}
			break;
		case kAudioPlugInPropertyResourceBundle:
			STRING(CFSTR(""));
			break;
		}
	} else if (object == d->object) {
		switch (selector) {
		case kAudioDevicePropertyJackIsConnected:
			/* sunfish S4: headphones (output scope) and their microphone (input scope) */
			if (jack_enabled && object == OBJECT_CODEC && scope == kAudioObjectPropertyScopeOutput) {
				WORD(jack_state & 1);
			} else if (jack_enabled && object == OBJECT_CODEC && scope == kAudioObjectPropertyScopeInput) {
				WORD(jack_state >> 1 & 1);
			}
			break;
		case kAudioObjectPropertyManufacturer:
			STRING(MANUFACTURER);
			break;
		case kAudioObjectPropertyName:
		case kAudioDevicePropertyDeviceUID:
		case kAudioDevicePropertyModelUID:
			STRING(d->uid);
			break;
		case kAudioDevicePropertyTransportType:
			WORD(TRANSPORT_BUILT_IN);
			break;
		case kAudioDevicePropertyRelatedDevices:
			WORD(d->object);
			break;
		case kAudioDevicePropertyClockDomain:
		case kAudioDevicePropertyLatency:
		case kAudioDevicePropertySafetyOffset:
		case kAudioDevicePropertyIsHidden:
			WORD(0);
			break;
		case kAudioDevicePropertyDeviceIsAlive:
			WORD(1);
			break;
		case kAudioDevicePropertyDeviceCanBeDefaultDevice:
		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
			WORD(d->output != 0);
			break;
		case kAudioDevicePropertyDeviceIsRunning:
			WORD(d->running != 0);
			break;
		case kAudioObjectPropertyOwnedObjects:
		case kAudioDevicePropertyStreams:
			/* the streams of one direction, or of both (and the speaker's volume control, which it owns) */
			if (scope != kAudioObjectPropertyScopeInput && d->output != 0) {
				LIST(d->output);
			}
			if (scope != kAudioObjectPropertyScopeOutput && d->input != 0) {
				LIST(d->input);
			}
			if (selector == kAudioObjectPropertyOwnedObjects && d->object == OBJECT_SPEAKER) {
				LIST(OBJECT_SPEAKER_VOLUME);
			}
			if (selector == kAudioObjectPropertyOwnedObjects && d->object == OBJECT_CODEC && scope != kAudioObjectPropertyScopeOutput) {
				LIST(OBJECT_CODEC_SOURCE);
			}
			v.empty = v.size == 0;
			break;
		case kAudioObjectPropertyControlList:
			if (d->object == OBJECT_SPEAKER) {
				LIST(OBJECT_SPEAKER_VOLUME);
			}
			if (d->object == OBJECT_CODEC) {
				LIST(OBJECT_CODEC_SOURCE);
			}
			v.empty = v.size == 0;
			break;
		case kAudioDevicePropertyNominalSampleRate:
			v.u.number = SUNFISH_AUDIO_RATE;
			v.size = sizeof(Float64);
			break;
		case kAudioDevicePropertyAvailableNominalSampleRates:
			v.u.range.mMinimum = v.u.range.mMaximum = SUNFISH_AUDIO_RATE;
			v.size = sizeof(AudioValueRange);
			break;
		case kAudioDevicePropertyPreferredChannelsForStereo:
			v.u.words[0] = 1;
			v.u.words[1] = 2;
			v.size = 2 * sizeof(UInt32);
			break;
		case kAudioDevicePropertyZeroTimeStampPeriod:
			WORD(TIMESTAMP_PERIOD);
			break;
		}
	} else {
		switch (selector) {
		case kAudioStreamPropertyIsActive:
			WORD(!inactive[object]);
			break;
		case kAudioStreamPropertyDirection:
			WORD(object == d->input);
			break;
		case kAudioStreamPropertyTerminalType:
			WORD(object == d->input ? kAudioStreamTerminalTypeMicrophone : kAudioStreamTerminalTypeSpeaker);
			break;
		case kAudioStreamPropertyStartingChannel:
			WORD(1);
			break;
		case kAudioStreamPropertyLatency:
			WORD(0);
			break;
		case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat:
			v.u.format = format_of(object);
			v.size = sizeof(AudioStreamBasicDescription);
			break;
		case kAudioStreamPropertyAvailableVirtualFormats:
		case kAudioStreamPropertyAvailablePhysicalFormats:
			v.u.ranged.mFormat = format_of(object);
			v.u.ranged.mSampleRateRange.mMinimum = v.u.ranged.mSampleRateRange.mMaximum = SUNFISH_AUDIO_RATE;
			v.size = sizeof(AudioStreamRangedDescription);
			break;
		}
	}
#undef WORD
#undef LIST
#undef STRING
#undef FLOAT
	return v;
}

/*
 * What the HAL (or the virtual audio device through it) asks of an object that it does not have,
 * the first times for each object, selector and scope: what a device it will not take may lack.
 */
#define UNKNOWN_TOLD            64
static UInt32 unknown_told[UNKNOWN_TOLD][3];
static unsigned unknown_count;

static void
unknown(AudioObjectID object, const AudioObjectPropertyAddress *address)
{
	UInt32 s = address->mSelector, c = address->mScope;

	for (unsigned i = 0; i < unknown_count; i++) {
		if (unknown_told[i][0] == object && unknown_told[i][1] == s && unknown_told[i][2] == c) {
			return;
		}
	}
	if (unknown_count >= UNKNOWN_TOLD) {
		return;
	}
	unknown_told[unknown_count][0] = object;
	unknown_told[unknown_count][1] = s;
	unknown_told[unknown_count][2] = c;
	unknown_count++;
	say("sunfish: audio: asked of object %u (%s) what it does not have: '%c%c%c%c' (%#x), scope '%c%c%c%c', element %u\n",
	    (unsigned)object, device_of(object) ? CFStringGetCStringPtr(device_of(object)->uid, kCFStringEncodingUTF8) : "-",
	    (int)(s >> 24 & 0xff), (int)(s >> 16 & 0xff), (int)(s >> 8 & 0xff), (int)(s & 0xff), (unsigned)s,
	    (int)(c >> 24 & 0xff), (int)(c >> 16 & 0xff), (int)(c >> 8 & 0xff), (int)(c & 0xff), (unsigned)address->mElement);
}

/*
 * sunfish S4: with the jack on, every call that ends in "no such property" (the HAL's 'unkn' after a
 * jack change is one of them), the first JACK_ASKED lines of the process and after each jack change.
 */
#define JACK_ASKED              200

static void
unknown_call(AudioObjectID object, const AudioObjectPropertyAddress *address, const char *call)
{
	UInt32 s, c;
	unsigned n;

	if (!jack_enabled || address == NULL) {
		return;
	}
	n = __atomic_fetch_add(&jack_asked, 1, __ATOMIC_RELAXED);
	if (n >= JACK_ASKED) {
		return;
	}
	s = address->mSelector;
	c = address->mScope;
	say("sunfish: audio: unknown property obj %u '%c%c%c%c' '%c%c%c%c' %u (%s)\n", (unsigned)object,
	    (int)(s >> 24 & 0xff), (int)(s >> 16 & 0xff), (int)(s >> 8 & 0xff), (int)(s & 0xff),
	    (int)(c >> 24 & 0xff), (int)(c >> 16 & 0xff), (int)(c >> 8 & 0xff), (int)(c & 0xff), (unsigned)address->mElement, call);
}

/* Whether the object has the property, without telling (has_property() tells). */
static Boolean
known(AudioObjectID object, const AudioObjectPropertyAddress *address)
{
	struct value v;

	if (address == NULL) {
		return false;
	}
	v = property(object, address, 0, NULL);
	if (v.size == 0 && !v.empty) {
		unknown(object, address);
	}
	return v.size != 0 || v.empty;
}

static Boolean
has_property(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused pid_t client,
    const AudioObjectPropertyAddress *address)
{
	Boolean has = known(object, address);

	if (!has) {
		unknown_call(object, address, "Has");
	}
	return has;
}

static OSStatus
is_property_settable(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused pid_t client,
    const AudioObjectPropertyAddress *address, Boolean *settable)
{
	const struct device *d = device_of(object);

	if (address == NULL || settable == NULL || !known(object, address)) {
		unknown_call(object, address, "Settable");
		return kAudioHardwareUnknownPropertyError;
	}
	/* the rate and the formats can be "set", to the ones there are; the volume can be set */
	if (object == OBJECT_SPEAKER_VOLUME) {
		*settable = address->mSelector == kAudioLevelControlPropertyScalarValue ||
		    address->mSelector == kAudioLevelControlPropertyDecibelValue;
	} else if (object == OBJECT_CODEC_SOURCE) {
		*settable = address->mSelector == kAudioSelectorControlPropertyCurrentItem;
	} else if (object == OBJECT_PLUGIN) {
		*settable = false;
	} else if (object == d->object) {
		*settable = address->mSelector == kAudioDevicePropertyNominalSampleRate;
	} else {
		*settable = address->mSelector == kAudioStreamPropertyIsActive ||
		    address->mSelector == kAudioStreamPropertyVirtualFormat || address->mSelector == kAudioStreamPropertyPhysicalFormat;
	}
	return 0;
}

static OSStatus
get_property_data_size(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused pid_t client,
    const AudioObjectPropertyAddress *address, UInt32 qualifier_size, const void *qualifier, UInt32 *size)
{
	if (address == NULL || size == NULL || !known(object, address)) {
		if (object >= OBJECT_PLUGIN && object <= OBJECT_LAST) {
			unknown_call(object, address, "Size");
		}
		return object < OBJECT_PLUGIN || object > OBJECT_LAST ? kAudioHardwareBadObjectError : kAudioHardwareUnknownPropertyError;
	}
	*size = property(object, address, qualifier_size, qualifier).size;
	return 0;
}

static OSStatus
get_property_data(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused pid_t client,
    const AudioObjectPropertyAddress *address, UInt32 qualifier_size, const void *qualifier, UInt32 room, UInt32 *size,
    void *data)
{
	struct value v;

	if (address == NULL || size == NULL || data == NULL || !known(object, address)) {
		if (object >= OBJECT_PLUGIN && object <= OBJECT_LAST) {
			unknown_call(object, address, "Get");
		}
		return object < OBJECT_PLUGIN || object > OBJECT_LAST ? kAudioHardwareBadObjectError : kAudioHardwareUnknownPropertyError;
	}
	if (object == OBJECT_SPEAKER_VOLUME && (address->mSelector == kAudioLevelControlPropertyConvertScalarToDecibels ||
	    address->mSelector == kAudioLevelControlPropertyConvertDecibelsToScalar)) {
		/* what is in the buffer, converted where it is */
		if (room < sizeof(Float32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		*(Float32 *)data = address->mSelector == kAudioLevelControlPropertyConvertScalarToDecibels ?
		    scalar_to_db(*(Float32 *)data) : db_to_scalar(*(Float32 *)data);
		*size = sizeof(Float32);
		return 0;
	}
	pthread_mutex_lock(&lock);
	v = property(object, address, qualifier_size, qualifier);
	pthread_mutex_unlock(&lock);
	if (v.size > room) {
		/* a list may be asked for in part; anything else must fit */
		if (address->mSelector != kAudioObjectPropertyOwnedObjects && address->mSelector != kAudioDevicePropertyStreams &&
		    address->mSelector != kAudioPlugInPropertyDeviceList) {
			return kAudioHardwareBadPropertySizeError;
		}
		v.size = room - room % sizeof(UInt32);
	}
	memcpy(data, &v.u, v.size);
	if (v.string) {
		CFRetain(v.u.string);
	}
	*size = v.size;
	return 0;
}

static OSStatus
set_property_data(AudioServerPlugInDriverRef driver, AudioObjectID object, pid_t client,
    const AudioObjectPropertyAddress *address, __unused UInt32 qualifier_size, __unused const void *qualifier, UInt32 size,
    const void *data)
{
	Boolean settable = false;

	if (address == NULL || data == NULL || is_property_settable(driver, object, client, address, &settable) != 0) {
		return kAudioHardwareUnknownPropertyError;
	}
	if (!settable) {
		return kAudioHardwareIllegalOperationError;
	}
	if (object == OBJECT_SPEAKER_VOLUME) {
		AudioObjectPropertyAddress changed[2] = {
			{ kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, 0 },
			{ kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, 0 },
		};
		Float32 wanted;

		if (size != sizeof(Float32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		wanted = *(const Float32 *)data;
		wanted = address->mSelector == kAudioLevelControlPropertyDecibelValue ? db_to_scalar(wanted) :
		    wanted < 0 ? 0 : wanted > 1 ? 1 : wanted;
		pthread_mutex_lock(&lock);
		volume = wanted;
		volume_to_kernel();
		pthread_mutex_unlock(&lock);
		if (host != NULL) {
			host->PropertiesChanged(host, OBJECT_SPEAKER_VOLUME, 2, changed);
		}
		return 0;
	}
	if (object == OBJECT_CODEC_SOURCE) {
		/* the one microphone there is */
		return size == sizeof(UInt32) ? (*(const UInt32 *)data == MIC_SOURCE ? 0 : kAudioHardwareIllegalOperationError) :
		       kAudioHardwareBadPropertySizeError;
	}
	if (address->mSelector == kAudioStreamPropertyIsActive) {
		if (size != sizeof(UInt32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		pthread_mutex_lock(&lock);
		inactive[object] = *(const UInt32 *)data == 0;
		pthread_mutex_unlock(&lock);
		return 0;
	}
	if (address->mSelector == kAudioDevicePropertyNominalSampleRate) {
		return size == sizeof(Float64) && *(const Float64 *)data == SUNFISH_AUDIO_RATE ? 0 : kAudioDeviceUnsupportedFormatError;
	}
	/* a format: the one we have, or nothing */
	if (size != sizeof(AudioStreamBasicDescription)) {
		return kAudioHardwareBadPropertySizeError;
	} else {
		const AudioStreamBasicDescription *wanted = data, format = format_of(object);

		return wanted->mSampleRate == format.mSampleRate && wanted->mFormatID == format.mFormatID &&
		       wanted->mChannelsPerFrame == format.mChannelsPerFrame && wanted->mBitsPerChannel == format.mBitsPerChannel &&
		       (wanted->mFormatFlags & kAudioFormatFlagIsSignedInteger) ? 0 : kAudioDeviceUnsupportedFormatError;
	}
}

/*
 * The microphone (the device "Codec"): /dev/sunfish-mic is open while the input runs, and the kernel
 * listens only while it is open. A read gives what has come and never waits; what it does not
 * give is silence. Said when it starts and stops, with how much came.
 */
static int mic = -1;
static UInt64 mic_frames, mic_heard, mic_short;
static int mic_peak;

static void
codec_open(void)
{
	mic = open("/dev/sunfish-mic", O_RDONLY | O_NONBLOCK);
	mic_frames = mic_heard = mic_short = 0;
	mic_peak = 0;
	say("sunfish: audio: the microphone is listened to (/dev/sunfish-mic: %s)\n", mic < 0 ? strerror(errno) : "open");
}

static void
codec_close(void)
{
	say("sunfish: audio: the microphone stops: %llu.%02llu s asked for, %llu.%02llu s of it from the kernel (peak %d of 32768), "
	    "%llu reads short\n", mic_frames / SUNFISH_AUDIO_RATE, mic_frames % SUNFISH_AUDIO_RATE * 100 / SUNFISH_AUDIO_RATE,
	    mic_heard / SUNFISH_AUDIO_RATE, mic_heard % SUNFISH_AUDIO_RATE * 100 / SUNFISH_AUDIO_RATE, mic_peak, mic_short);
	if (mic >= 0) {
		close(mic);
	}
	mic = -1;
}

static void
codec_read(int16_t *samples, UInt32 frames)
{
	ssize_t got = mic >= 0 ? read(mic, samples, (size_t)frames * 2) : -1;
	UInt32 have = got > 0 ? (UInt32)got / 2 : 0;

	if (have > 0 && mic_heard == 0) {
		say("sunfish: audio: the microphone's first samples, %llu.%02llu s after the input started\n",
		    mic_frames / SUNFISH_AUDIO_RATE, mic_frames % SUNFISH_AUDIO_RATE * 100 / SUNFISH_AUDIO_RATE);
	}
	for (UInt32 i = 0; i < have; i++) {
		int v = samples[i] < 0 ? -samples[i] : samples[i];

		mic_peak = v > mic_peak ? v : mic_peak;
	}
	if (have < frames) {
		memset(samples + have, 0, (size_t)(frames - have) * 2);
		mic_short += mic_heard > 0;
	}
	mic_frames += frames;
	mic_heard += have;
}

static OSStatus
start_io(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused UInt32 client)
{
	struct device *d = device_of(object);

	if (d == NULL || object != d->object) {
		return kAudioHardwareBadObjectError;
	}
	pthread_mutex_lock(&lock);
	if (d->running++ == 0) {
		d->anchor = mach_absolute_time();
		d->periods = 0;
		if (d->output != 0 && d->object != OBJECT_CODEC) {
			written = refused = 0;
			say("sunfish: audio: playing\n");
		}
		if (d->object == OBJECT_CODEC) {
			codec_open();
		}
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

static OSStatus
stop_io(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused UInt32 client)
{
	struct device *d = device_of(object);

	if (d == NULL || object != d->object) {
		return kAudioHardwareBadObjectError;
	}
	pthread_mutex_lock(&lock);
	if (d->running != 0 && --d->running == 0) {
		if (d->object == OBJECT_CODEC) {
			codec_close();
		} else if (d->output != 0) {
			say("sunfish: audio: stopped after %llu.%02llu s (%llu frames not taken by %s)\n", written / SUNFISH_AUDIO_RATE,
			    written % SUNFISH_AUDIO_RATE * 100 / SUNFISH_AUDIO_RATE, refused, SUNFISH_AUDIO_DEVICE);
		}
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

/* Where the device's clock was last at a multiple of TIMESTAMP_PERIOD frames, and when. */
static OSStatus
get_zero_time_stamp(__unused AudioServerPlugInDriverRef driver, AudioObjectID object, __unused UInt32 client,
    Float64 *sample_time, UInt64 *host_time, UInt64 *seed)
{
	Float64 ticks_per_period = ticks_per_frame * TIMESTAMP_PERIOD;
	UInt64 now = mach_absolute_time();
	struct device *d = device_of(object);

	if (d == NULL) {
		return kAudioHardwareBadObjectError;
	}
	pthread_mutex_lock(&lock);
	while (now >= d->anchor + (UInt64)((Float64)(d->periods + 1) * ticks_per_period)) {
		d->periods++;
	}
	*sample_time = (Float64)d->periods * TIMESTAMP_PERIOD;
	*host_time = d->anchor + (UInt64)((Float64)d->periods * ticks_per_period);
	*seed = 1;
	pthread_mutex_unlock(&lock);
	return 0;
}

static OSStatus
will_do_io_operation(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object, __unused UInt32 client,
    UInt32 operation, Boolean *will_do, Boolean *in_place)
{
	*will_do = operation == kAudioServerPlugInIOOperationReadInput || operation == kAudioServerPlugInIOOperationWriteMix;
	*in_place = true;
	return 0;
}

static OSStatus
begin_end_io_operation(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object, __unused UInt32 client,
    __unused UInt32 operation, __unused UInt32 frames, __unused const AudioServerPlugInIOCycleInfo *cycle)
{
	return 0;
}

/*
 * What iOS plays on the actuator -- the haptics: the system's feedbacks and the ringer's vibration,
 * CoreHaptics' patterns, UIKit's feedbacks, as waveforms of one channel -- goes to the kernel's motor
 * driver (/dev/sunfish-haptic, ios-redfin: bsd/dev/redwood_haptic.c; no sunfish driver yet), which plays Android's own
 * waveforms on the phone's motor by it. (iOS plays on this device at all only because the device
 * tree says the phone's haptics are "closed-loop": see mkidentity.py.) Said the first times, with
 * how loud.
 */
static int haptic = -1;          /* the motor driver; -2: not there (the kernel runs without haptic=5) */
static unsigned haptic_said;

static void
actuator_write(const int16_t *samples, UInt32 frames)
{
	int peak = 0;

	for (UInt32 i = 0; i < frames; i++) {
		int v = samples[i] < 0 ? -samples[i] : samples[i];

		peak = v > peak ? v : peak;
	}
	if (haptic == -1) {
		haptic = open("/dev/sunfish-haptic", O_WRONLY | O_NONBLOCK);
		haptic = haptic >= 0 ? haptic : -2;
	}
	if (haptic >= 0) {
		(void)write(haptic, samples, (size_t)frames * 2);
	}
	if (peak > 0 && haptic_said < 12) {
		haptic_said++;
		say("sunfish: audio: the actuator plays: %u frames, peak %d of 32768%s\n", (unsigned)frames, peak,
		    haptic >= 0 ? "" : " (no /dev/sunfish-haptic: not felt)");
	}
}

static OSStatus
do_io_operation(__unused AudioServerPlugInDriverRef driver, __unused AudioObjectID object, AudioObjectID stream,
    __unused UInt32 client, UInt32 operation, UInt32 frames, __unused const AudioServerPlugInIOCycleInfo *cycle, void *buffer,
    __unused void *secondary)
{
	const struct device *d = device_of(stream);

	if (d == NULL) {
		return kAudioHardwareBadObjectError;
	}
	if (operation == kAudioServerPlugInIOOperationReadInput && stream == OBJECT_CODEC_INPUT) {
		codec_read(buffer, frames);
	} else if (operation == kAudioServerPlugInIOOperationReadInput) {
		/* the speaker's and Hawking's inputs: silence */
		memset(buffer, 0, (size_t)frames * 2 * d->input_channels);
	} else if (operation == kAudioServerPlugInIOOperationWriteMix && stream == OBJECT_CODEC_OUTPUT) {
		/* sunfish S4: the codec's output (JACK_OUT): dropped */
	} else if (operation == kAudioServerPlugInIOOperationWriteMix && stream == OBJECT_ACTUATOR_OUTPUT) {
		actuator_write(buffer, frames);
	} else if (operation == kAudioServerPlugInIOOperationWriteMix && stream == d->output) {
		/* the kernel takes it all or leaves it: this thread must not wait */
		if (sink < 0 || write(sink, buffer, (size_t)frames * SUNFISH_AUDIO_FRAME) < 0) {
			refused += frames;
		}
		written += frames;
	}
	return 0;
}

static AudioServerPlugInDriverInterface interface = {
	NULL, query_interface, add_ref, release, initialize, create_device, destroy_device, device_client, device_client,
	configuration_change, configuration_change, has_property, is_property_settable, get_property_data_size,
	get_property_data, set_property_data, start_io, stop_io, get_zero_time_stamp, will_do_io_operation,
	begin_end_io_operation, do_io_operation, begin_end_io_operation,
};
static AudioServerPlugInDriverInterface *driver = &interface;

/* The factory named in the bundle's Info.plist. */
void *
SunfishAudio_Create(__unused CFAllocatorRef allocator, CFUUIDRef type)
{
	return CFEqual(type, kAudioServerPlugInTypeUUID) ? &driver : NULL;
}
