#!/usr/bin/env bash
# Downloads the FSR 4 v07 INT8 model assets (SPIR-V passes, model initializers, pre-pass weights) into the
# folder given as the first argument. They were built by Q2RTX from AMD's MIT-licensed FidelityFX SDK FSR 4
# source (see LICENSE-FSR4-v07.txt in the folder); they are not part of this repository.
# Output 1920x1080 needs the 1080 tier, 1440p and 2160p outputs the 2160 tier.
set -euo pipefail
dest="${1:?usage: fetch_fsr4_assets.sh <folder>}"
commit=ae8d628fae208813172446d1e49ed94150b04658
base="https://raw.githubusercontent.com/FireBurn/Q2RTX/$commit/baseq2/fsr4_shaders"
mkdir -p "$dest"
files=(LICENSE-FSR4-v07.txt rcas.spv spd_auto_exposure.spv)
for model in native quality balanced performance ultraperf; do
    files+=("fsr4_model_v07_i8_${model}_initializers.bin"
            "fsr4_model_v07_i8_${model}_pre_weights.bin"
            "fsr4_model_v07_i8_${model}_shader_manifest.json")
    for tier in 1080 2160; do
        files+=("fsr4_model_v07_i8_${model}_${tier}_pre.spv"
                "fsr4_model_v07_i8_${model}_${tier}_post.spv")
        for pass in $(seq 1 12); do
            files+=("fsr4_model_v07_i8_${model}_${tier}_pass${pass}.spv")
        done
    done
done
for file in "${files[@]}"; do
    if [[ -s $dest/$file ]]; then continue; fi
    curl -fsSL --retry 3 -o "$dest/$file.part" "$base/$file"
    mv "$dest/$file.part" "$dest/$file"
done
echo "FSR 4 assets: ${#files[@]} files in $dest"
