#!/bin/bash
# Renders the on-screen menu on a PC:
#   bash test/menu_preview.sh <figures dir> <icon pack dir> <out.ppm> [title id] [slots|grid|special|swap]
set -e
cd "$(dirname "$0")"
INC=~/ps4/OpenOrbis/PS4Toolchain/include
mkdir -p build
# Only the Orbis *type* headers are needed (pad button names); stub the kernel header.
mkdir -p build/orbis
echo '#pragma once
#include <stdint.h>
int sceKernelOpen(const char *, int, int);
int sceKernelGetdents(int, char *, int);
int sceKernelClose(int);
uint64_t sceKernelGetProcessTime(void);' > build/orbis/libkernel.h
mkdir -p build/orbis/_types
cp "$INC/orbis/_types/pad.h" build/orbis/_types/pad.h
sed -i '/#include/d' build/orbis/_types/pad.h
sed -i '1i #include <stdint.h>' build/orbis/_types/pad.h
cc -O1 -std=gnu11 -Ibuild -DMENU_PREVIEW -DGP_FIGURE_DIR="\"$1\"" -DGP_ICON_DIR="\"$2\"" \
   -DGP_MENU_CFG="\"build/menu.cfg\"" \
   menu_preview.c ../source/menu.c ../source/gfx.c ../source/lang.c -o build/menu_preview
./build/menu_preview "$3" "${4:-CUSA00024}" "${5:-grid}"
