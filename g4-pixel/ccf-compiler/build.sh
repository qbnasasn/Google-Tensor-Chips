#!/data/data/com.termux/files/usr/bin/bash
# Build PixelG4A17_Compiler for current Tensor G4 on A17+
# Run from the PixelG4A17_Compiler dir.

set -e
clang -O2 -std=c11 -Wall -Wextra \
  PixelG4A17_Compiler.c -o PixelG4A17_Compiler -ldl

echo "Built PixelG4A17_Compiler"
file PixelG4A17_Compiler
ls -l PixelG4A17_Compiler