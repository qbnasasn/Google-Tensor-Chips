#!/data/data/com.termux/files/usr/bin/bash
# Run PixelG4A17_Compiler as root on the device for A17+
# Usage: ./run.sh [tflite] [options_proto]

set -e

RISH=${RISH:-/data/data/com.termux/files/usr/bin/rish}
SU="su -c"

TFLITE=${1:-/data/local/tmp/embgemma_seq512.tflite}
OPTIONS=${2:-/data/local/tmp/PixelG4A17_options.bin}
BINARY=./PixelG4A17_Compiler

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

echo "=== Running (patches will likely need A17 update) ==="
$SU "
  setenforce 0 2>/dev/null || true
  /data/local/tmp/PixelG4A17_Compiler /data/local/tmp/embgemma_seq512.tflite /data/local/tmp/PixelG4A17_options.bin
  setenforce 1 2>/dev/null || true
"

echo "=== Check outputs ==="
$SU "
  ls -l /data/local/tmp/PixelG4A17.dgc0 /data/local/tmp/PixelG4A17_marker.txt 2>/dev/null || echo 'no output yet'
  cat /data/local/tmp/PixelG4A17_marker.txt 2>/dev/null || true
"