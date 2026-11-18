#!/data/data/com.termux/files/usr/bin/bash
# Run PixelG4A17_Compiler as root on the device for A17+
# Usage: ./run.sh [tflite] [options_proto]

set -e

RISH=${RISH:-/data/data/com.termux/files/usr/bin/rish}
SU="su -c"

TFLITE=${1:-/data/local/tmp/embgemma_seq512.tflite}
OPTIONS=${2:-/data/local/tmp/PixelG4A17_options.bin}
BINARY=./PixelG4A17_Compiler
COMPILER_SO=/vendor/lib64/libedgetpu_tflite_compiler.so

# Optional build check. The drivers patch the vendor compiler in memory at fixed offsets
# that only fit one specific build. Each patch already verifies the original bytes and the
# driver aborts on mismatch; set COMPILER_SHA256 to also refuse any other .so up front.
#   COMPILER_SHA256=<hex> ./run.sh ...
if [ -n "${COMPILER_SHA256:-}" ]; then
  ACTUAL=$($SU "sha256sum $COMPILER_SO" | cut -d' ' -f1)
  if [ "$ACTUAL" != "$COMPILER_SHA256" ]; then
    echo "Refusing to run: $COMPILER_SO sha256 is $ACTUAL, expected $COMPILER_SHA256" >&2
    exit 1
  fi
  echo "Compiler .so sha256 matches."
else
  echo "Note: COMPILER_SHA256 not set; relying on per-patch byte checks only."
  $SU "ls -l $COMPILER_SO; sha256sum $COMPILER_SO" || true
fi

if [ ! -f "$BINARY" ]; then
  echo "Build first: ./build.sh"
  exit 1
fi

echo "=== Staging as root ==="
$SU "
  mkdir -p /data/local/tmp
  cp $PWD/PixelG4A17_Compiler /data/local/tmp/
  chmod 755 /data/local/tmp/PixelG4A17_Compiler
  cp $TFLITE /data/local/tmp/embgemma_seq512.tflite 2>/dev/null || true
  cp $OPTIONS /data/local/tmp/PixelG4A17_options.bin 2>/dev/null || true
  ls -l /data/local/tmp/PixelG4A17_Compiler /data/local/tmp/embgemma_seq512.tflite /data/local/tmp/PixelG4A17_options.bin
"

# SELinux: the compiler may be denied access to /vendor or /data/vendor/edgetpu under the
# default policy when run from a root shell. Dropping to permissive affects the WHOLE device
# for the duration of the run, so it is opt-in only: PERMISSIVE=1 ./run.sh ...
echo "=== Running (patches will likely need A17 update) ==="
if [ "${PERMISSIVE:-0}" = "1" ]; then
  echo "WARNING: PERMISSIVE=1 — setting SELinux to permissive device-wide for this run." >&2
  $SU "
    setenforce 0 2>/dev/null || true
    /data/local/tmp/PixelG4A17_Compiler /data/local/tmp/embgemma_seq512.tflite /data/local/tmp/PixelG4A17_options.bin
    rc=\$?
    setenforce 1 2>/dev/null || true
    exit \$rc
  "
else
  $SU "/data/local/tmp/PixelG4A17_Compiler /data/local/tmp/embgemma_seq512.tflite /data/local/tmp/PixelG4A17_options.bin"
fi

echo "=== Check outputs ==="
$SU "
  ls -l /data/local/tmp/PixelG4A17.dgc0 /data/local/tmp/PixelG4A17_marker.txt 2>/dev/null || echo 'no output yet'
  cat /data/local/tmp/PixelG4A17_marker.txt 2>/dev/null || true
"
