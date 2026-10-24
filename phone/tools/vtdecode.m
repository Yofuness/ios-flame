/*
 * vtdecode: can this phone decode video, how fast, and in hardware or software?
 *   vtdecode FILE [SECONDS]
 * 1. AVAssetReader decodes the first video track to BGRA (the whole AVFoundation/VideoToolbox path) and
 *    prints frames, wall time and fps.
 * 2. VTIsHardwareDecodeSupported for the track's codec, and a VTDecompressionSession created with
 *    RequireHardwareAcceleratedVideoDecoder: prints its OSStatus (0 = hardware decoder exists).
 */
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <VideoToolbox/VideoToolbox.h>

int main(int argc, char **argv)
{
	@autoreleasepool {
		if (argc < 2) {
			fprintf(stderr, "usage: vtdecode FILE [SECONDS]\n");
			return 2;
		}
		double limit = argc > 2 ? atof(argv[2]) : 15;
		NSURL *u = [NSURL fileURLWithPath:@(argv[1])];
		AVURLAsset *asset = [AVURLAsset URLAssetWithURL:u options:nil];
		NSArray *tracks = [asset tracksWithMediaType:AVMediaTypeVideo];
		printf("tracks: %lu\n", (unsigned long)tracks.count);
		if (!tracks.count)
			return 1;
		AVAssetTrack *t = tracks[0];
		CMFormatDescriptionRef fd = (__bridge CMFormatDescriptionRef)t.formatDescriptions.firstObject;
		FourCharCode codec = CMFormatDescriptionGetMediaSubType(fd);
		CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(fd);
		printf("codec %c%c%c%c %dx%d nominal %.1f fps\n", codec >> 24, codec >> 16, codec >> 8, codec,
		    d.width, d.height, t.nominalFrameRate);
		printf("VTIsHardwareDecodeSupported: %d\n", VTIsHardwareDecodeSupported(codec));
		VTDecompressionSessionRef s = NULL;
		NSDictionary *spec = @{ (__bridge id)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: @YES };
		OSStatus st = VTDecompressionSessionCreate(NULL, fd, (__bridge CFDictionaryRef)spec, NULL, NULL, &s);
		printf("VTDecompressionSession (require hardware): OSStatus %d\n", (int)st);
		if (s)
			VTDecompressionSessionInvalidate(s);

		NSError *err = nil;
		AVAssetReader *r = [AVAssetReader assetReaderWithAsset:asset error:&err];
		if (!r) {
			printf("reader: %s\n", err.description.UTF8String);
			return 1;
		}
		AVAssetReaderTrackOutput *o = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:t
		    outputSettings:@{ (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA) }];
		[r addOutput:o];
		if (![r startReading]) {
			printf("startReading failed: %s\n", r.error.description.UTF8String);
			return 1;
		}
		CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
		unsigned frames = 0;
		for (;;) {
			CMSampleBufferRef sb = [o copyNextSampleBuffer];
			if (!sb)
				break;
			frames++;
			CFRelease(sb);
			if (CFAbsoluteTimeGetCurrent() - t0 > limit)
				break;
		}
		double dt = CFAbsoluteTimeGetCurrent() - t0;
		printf("decoded %u frames in %.2f s = %.1f fps (status %ld %s)\n", frames, dt, frames / dt,
		    (long)r.status, r.error ? r.error.description.UTF8String : "");
	}
	return 0;
}
