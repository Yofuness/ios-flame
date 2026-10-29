#!/bin/bash
# Build CocoaTop (github.com/D0m0/CocoaTop, process viewer) for the phone on Linux, without Xcode/theos.
# Out: work/cocoatop/CocoaTop.app (install: tools/sunfish-cocoatop-install.sh).
# No actool/ibtool here: loose PNG icons instead of the asset catalog, UILaunchScreen instead of the storyboard,
# and a small xpc.h stub (the theos iOS 16.5 SDK has no xpc headers).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=$ROOT/work/cocoatop-src
W=$ROOT/work/cocoatop
[ -d "$SRC" ] || git clone -q --depth 1 https://github.com/D0m0/CocoaTop.git "$SRC"
CC=$ROOT/tools/linux-ios-cc
A=$SRC/src/CocoaTop
rm -rf "$W"; mkdir -p "$W/obj" "$W/CocoaTop.app"
cat > "$A/src/xpc/xpc.h" <<'EOF'
/* sunfish build: minimal libxpc declarations (the theos iOS 16.5 SDK has no xpc headers). */
#ifndef __XPC_H__
#define __XPC_H__
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <mach/mach.h>
typedef void *xpc_object_t;
typedef void *xpc_connection_t;
typedef void *xpc_endpoint_t;
typedef void *xpc_type_t;
typedef void *xpc_pipe_t;
typedef void (*xpc_handler_t)(xpc_object_t);
void xpc_release(xpc_object_t object);
xpc_object_t xpc_dictionary_create(const char * const *keys, const xpc_object_t *values, size_t count);
void xpc_dictionary_set_uint64(xpc_object_t xdict, const char *key, uint64_t value);
void xpc_dictionary_set_fd(xpc_object_t xdict, const char *key, int fd);
int64_t xpc_dictionary_get_int64(xpc_object_t xdict, const char *key);
xpc_pipe_t xpc_pipe_create_from_port(mach_port_t port, uint64_t flags);
int xpc_pipe_routine(xpc_pipe_t pipe, xpc_object_t message, xpc_object_t *reply);
#endif
EOF
for f in "$A"/src/*.m; do
	"$CC" -c -fobjc-arc -O2 -include sys/mman.h -iquote "$A/src" -idirafter "$A/src" -F"$A" \
		-Wno-deprecated-declarations -o "$W/obj/$(basename "$f" .m).o" "$f"
done
APP=$W/CocoaTop.app
"$CC" -fobjc-arc -o "$APP/CocoaTop" "$W"/obj/*.o -framework UIKit -framework Foundation \
	-framework CoreGraphics -framework IOKit -framework QuartzCore
"$CC" -O2 -Wall -o "$APP/CocoaTop-helper" "$SRC/src/CocoaTop-helper/RootWrapper.c"
cp "$A"/res/*.html "$A"/res/*.css "$A/res/Assets.xcassets/UIButtonBarHamburger.imageset/UIButtonBarHamburger.png" "$APP/"
ICON="$A/res/Assets.xcassets/AppIcon.appiconset/Icon-any.png"
magick "$ICON" -resize 120x120 "$APP/AppIcon60x60@2x.png"
magick "$ICON" -resize 180x180 "$APP/AppIcon60x60@3x.png"
python3 -I - "$A/Info.plist" "$APP/Info.plist" <<'EOF'
import plistlib, sys
p = plistlib.load(open(sys.argv[1], 'rb'))
p['CFBundleIdentifier'] = 'ru.domo.cocoatop64'
p.pop('UILaunchStoryboardName', None)
p['UILaunchScreen'] = {}
p['CFBundleIcons'] = {'CFBundlePrimaryIcon': {'CFBundleIconFiles': ['AppIcon60x60'], 'CFBundleIconName': 'AppIcon'}}
p['MinimumOSVersion'] = '16.0'
p['CFBundleSupportedPlatforms'] = ['iPhoneOS']
p['UIDeviceFamily'] = [1, 2]
p['UIRequiredDeviceCapabilities'] = ['arm64']
plistlib.dump(p, open(sys.argv[2], 'wb'))
EOF
"$ROOT/tools/linux-ios-sign" "$APP/CocoaTop-helper" "$SRC/src/CocoaTop-helper/Ent.plist"
"$ROOT/tools/linux-ios-sign" "$APP/CocoaTop" "$A/entitlements.xml"
echo "built $APP"
