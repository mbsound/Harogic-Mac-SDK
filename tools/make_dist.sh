#!/bin/sh
# Assemble a self-contained, relocatable SDK folder:
#
#   dist/htraapi-macos-arm64/
#     lib/libhtraapi.dylib        (+ libusb, GCC libstdc++/libgcc_s beside it)
#     include/*.h                 (vendor headers)
#     htrausb.conf                (USB whitelist; optional override)
#
# All inter-library references are rewritten to @loader_path, so the lib/
# folder can live anywhere (e.g. inside an application bundle).
set -eu

BUILD=${1:?build dir}
VENDOR_INC=${2:?vendor include dir}
VENDOR_CONF=${3:?htrausb.conf path}
OUT=${4:?output dir}
GCCLIB=${GCCLIB:-/opt/homebrew/opt/gcc/lib/gcc/current}
USBLIB=${USBLIB:-/opt/homebrew/opt/libusb/lib/libusb-1.0.0.dylib}

rm -rf "$OUT"
mkdir -p "$OUT/lib" "$OUT/include"

cp "$BUILD/libhtraapi.dylib" "$OUT/lib/"
cp "$GCCLIB/libstdc++.6.dylib" "$GCCLIB/libgcc_s.1.1.dylib" "$OUT/lib/"
cp "$USBLIB" "$OUT/lib/libusb-1.0.0.dylib"
chmod u+w "$OUT"/lib/*.dylib
cp "$VENDOR_INC"/*.h "$OUT/include/"
[ -f "$VENDOR_CONF" ] && cp "$VENDOR_CONF" "$OUT/lib/"

cd "$OUT/lib"
install_name_tool -id @rpath/libusb-1.0.0.dylib libusb-1.0.0.dylib
install_name_tool -id @rpath/libgcc_s.1.1.dylib libgcc_s.1.1.dylib
install_name_tool -id @rpath/libstdc++.6.dylib libstdc++.6.dylib

install_name_tool \
    -change "$USBLIB" @loader_path/libusb-1.0.0.dylib \
    -change "$(otool -L libhtraapi.dylib | awk '/libusb-1.0/ {print $1; exit}')" @loader_path/libusb-1.0.0.dylib \
    -change "$GCCLIB/libstdc++.6.dylib" @loader_path/libstdc++.6.dylib \
    libhtraapi.dylib
# Drop the build-time rpath to Homebrew's GCC so only the bundled copy is used.
install_name_tool -delete_rpath "$GCCLIB" libhtraapi.dylib 2>/dev/null || true

install_name_tool -change "$GCCLIB/libgcc_s.1.1.dylib" @loader_path/libgcc_s.1.1.dylib \
    libstdc++.6.dylib
install_name_tool -change "$GCCLIB/libgcc_s.1.1.dylib" @loader_path/libgcc_s.1.1.dylib \
    libgcc_s.1.1.dylib 2>/dev/null || true

for f in *.dylib; do codesign -f -s - "$f" 2>/dev/null; done

# Fail loudly if anything still points into Homebrew.
if otool -L *.dylib | grep -q /opt/homebrew; then
    echo "error: unresolved Homebrew references:" >&2
    otool -L *.dylib | grep /opt/homebrew >&2
    exit 1
fi
echo "dist: $OUT"
