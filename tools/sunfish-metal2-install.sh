#!/bin/sh
# Install SweetMetal2.bundle (from the metal2 CI artifact, unpacked into DIR) on the phone in
# /System/Library/Extensions, list SpringBoard as a Metal client, restart backboardd. Boot with sweetmtl=2.
#   tools/sunfish-metal2-install.sh DIR [--no-restart]
set -e
cd "$(dirname "$0")/.."
B=$1/SweetMetal2.bundle
for f in SweetMetal2 libvulkan_freedreno.dylib Info.plist _CodeSignature/CodeResources; do
	[ -s "$B/$f" ] || { echo "incomplete bundle: no $B/$f" >&2; exit 1; }
done
S=tools/sunfish-ssh
$S 'rm -rf /var/local/tmp/m2 && mkdir -p /var/local/tmp/m2 && : >> /var/local/mtl2.log && chmod 666 /var/local/mtl2.log;
	printf "SpringBoard\ncom.apple.WebKit.GPU\nMobileSafari\nMaps\n" > /var/local/mtl2-clients'
tar --owner=0 --group=0 -czf - -C "$1" SweetMetal2.bundle | $S 'toybox zcat | toybox tar -xpf - -C /var/local/tmp/m2'
$S 'E=/System/Library/Extensions; rm -rf $E/SweetMetal2.bundle.old; [ -d $E/SweetMetal2.bundle ] &&
	mv $E/SweetMetal2.bundle $E/SweetMetal2.bundle.old; mv /var/local/tmp/m2/SweetMetal2.bundle $E/ &&
	rm -rf $E/SweetMetal2.bundle.old; ls -la $E/SweetMetal2.bundle'
[ "$2" = --no-restart ] || $S 'psx -k backboardd' || true
