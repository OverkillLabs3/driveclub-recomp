#!/usr/bin/env bash
# AMD's signed FSR SDK 2.4 Vulkan DLLs (loader and upscaler: FSR 4.1 on Radeon), checked against pinned hashes.
# They are redistributed under the licence in LICENSE-AMD-FidelityFX-SDK.md (kept beside them in the package).
set -euo pipefail
dest="${1:?usage: fetch_amd_fsr.sh <folder>}"
base=https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/main
mkdir -p "$dest"
declare -A sha=(
  [amd_fidelityfx_loader_vk.dll]=38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9
  [amd_fidelityfx_upscaler_vk.dll]=4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7
)
for f in "${!sha[@]}"; do
  if [[ -f $dest/$f ]] && echo "${sha[$f]}  $dest/$f" | sha256sum -c --quiet - 2>/dev/null; then continue; fi
  curl -fsSL --retry 3 -o "$dest/$f.part" "$base/Kits/FidelityFX/signedbin/$f"
  echo "${sha[$f]}  $dest/$f.part" | sha256sum -c --quiet - || { echo "$f does not match its pinned hash" >&2; rm -f "$dest/$f.part"; exit 1; }
  mv "$dest/$f.part" "$dest/$f"
done
[[ -f $dest/LICENSE-AMD-FidelityFX-SDK.md ]] || curl -fsSL -o "$dest/LICENSE-AMD-FidelityFX-SDK.md" "$base/docs/license.md"
