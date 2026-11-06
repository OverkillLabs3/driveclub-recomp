#!/usr/bin/env bash
# Compiles LibAtrac9 (MIT licence, third_party/LibAtrac9) into out/atrac9/*.o; the audio commands of the kit use it to read
# and write the ATRAC9 streams of the sound banks.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
[[ -f msys2-env.sh ]] && source ./msys2-env.sh
cc=${CC:-clang}
command -v "$cc" >/dev/null || cc=gcc
mkdir -p out/atrac9
for f in band_extension bit_allocation bit_reader decinit decoder huffCodes imdct libatrac9 quantization scale_factors tables unpack utility; do
    if [[ ! -f out/atrac9/$f.o || third_party/LibAtrac9/C/src/$f.c -nt out/atrac9/$f.o ]]; then
        "$cc" -O2 -w -c -o "out/atrac9/$f.o" "third_party/LibAtrac9/C/src/$f.c"
    fi
done
