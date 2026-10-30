/*
 * camera_avtest: the cameras iOS lists for applications, as AVFoundation gives them: the video
 * devices and every format of the default one. List only: no capture session, no preview, no
 * frames (privacy). From ios-redfin phone/audio/camera_avtest.m (Copyright (c) 2026 dipa-studio, MIT);
 * their version starts no session either.
 *
 * usage: camera_avtest
 */
#import <AVFoundation/AVFoundation.h>
#include <stdio.h>

int
main(void)
{
	@autoreleasepool {
		setvbuf(stdout, NULL, _IOLBF, 0);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		NSArray *devices = [AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo];
#pragma clang diagnostic pop
		printf("camera_avtest: %lu video device(s)\n", (unsigned long)devices.count);
		for (AVCaptureDevice *d in devices) {
			printf("camera_avtest:   %s (%s), position %ld, %lu formats\n", d.localizedName.UTF8String, d.uniqueID.UTF8String,
			    (long)d.position, (unsigned long)d.formats.count);
		}
		AVCaptureDevice *device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
		if (device == nil) {
			printf("camera_avtest: no default video device\n");
			return 1;
		}
		for (AVCaptureDeviceFormat *f in device.formats) {
			printf("camera_avtest:   format %s\n", f.description.UTF8String);
		}
		printf("camera_avtest: the default device's active format: %s\n", device.activeFormat.description.UTF8String);
		return 0;
	}
}
