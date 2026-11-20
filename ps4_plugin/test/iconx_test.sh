#!/bin/bash
# PC test of the on-console icon extraction:
#   bash test/iconx_test.sh <archives dir> <out dir> <title id> <year code>
set -e
cd "$(dirname "$0")"
mkdir -p build/orbis
echo '#pragma once
#include <stdint.h>
uint64_t sceKernelGetProcessTime(void);' > build/orbis/libkernel.h
cc -O2 -std=gnu11 -Ibuild -Wall iconx_test.c ../source/iconx.c ../source/inflate.c -o build/iconx_test
./build/iconx_test "$1" "$2" "$3" "$4"
