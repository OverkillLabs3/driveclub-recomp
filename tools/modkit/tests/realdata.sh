#!/usr/bin/env bash
# Checks of dcmod against the real game files (needs --update/--base or DRIVECLUB_UPDATE_DIR and DRIVECLUB_BASE_DIR).
# Round trips must give the same bytes: packs, car and track geometry, sound banks. Run from the repository root
# after tools/modkit/build.sh. Prints "all real data checks passed" or stops at the first failure.
set -euo pipefail
cd -- "$(dirname -- "$0")/../../.."
exe=out/dcmod
[[ -f out/dcmod.exe ]] && exe=out/dcmod.exe
tmp=$(mktemp -d)
trap 'rm -rf "${tmp:?}"' EXIT

for pack in vehicles/ferrari_458_italia.rpk worlds/canada_road_circuit_02_n.rpk; do
    "$exe" archive cat "$pack" > "$tmp/orig.rpk"
    "$exe" rpk verify "@$pack" > /dev/null
    "$exe" mesh export "@$pack" -o "$tmp/m.glb" > /dev/null
    "$exe" mesh import "@$pack" "$tmp/m.glb" -o "$tmp/same.rpk" > /dev/null
    cmp "$tmp/orig.rpk" "$tmp/same.rpk"
    echo "ok mesh round trip: $pack"
done

# fewer triangles in one part, more of both in another: the pack must read back with the new counts
py=python; "$py" -V > /dev/null 2>&1 || py=python3
pack=vehicles/ferrari_458_italia.rpk
"$exe" mesh export "@$pack" -o "$tmp/body.glb" "0008a48ef19c8161_*" > /dev/null
"$py" -I tools/modkit/tests/glbedit.py "$tmp/body.glb" "$tmp/body2.glb" half4 dup5 > /dev/null
"$exe" mesh import "@$pack" "$tmp/body2.glb" -o "$tmp/grown.rpk" > /dev/null
"$exe" rpk verify "$tmp/grown.rpk" > /dev/null
"$exe" mesh info "$tmp/grown.rpk" | grep -q "0008a48ef19c8161_4_BODYShape  *9348 vertices  *7076 triangles"
"$exe" mesh info "$tmp/grown.rpk" | grep -q "0008a48ef19c8161_5_BODYShape  *120 vertices  *136 triangles"
grep -aq NORMAL "$tmp/body.glb"
echo "ok mesh with changed counts"

# the same part with buffers of its own: two new vertex buffers, the mesh names them, and nothing names the payload of another id
"$exe" mesh export "@$pack" -o "$tmp/boot.glb" "0008dd44e0ac42c5_0_*" > /dev/null
"$py" -I tools/modkit/tests/glbedit.py "$tmp/boot.glb" "$tmp/boot2.glb" dup0 > /dev/null
"$exe" mesh import "@$pack" "$tmp/boot2.glb" --own-buffers -o "$tmp/own.rpk" > /dev/null
"$exe" rpk verify "$tmp/own.rpk" | grep -q "payload names another identifier: 0"
"$exe" mesh info "$tmp/own.rpk" | grep -q "0008dd44e0ac42c5_0_BOOTShape  *610 vertices  *1032 triangles"
[[ $("$exe" rpk list "$tmp/own.rpk" -l | grep -c "vertex_buffer") -eq $(( $("$exe" rpk list "@$pack" -l | grep -c "vertex_buffer") + 2 )) ]]
echo "ok part with its own buffers"

# skinned meshes (a driver): bone numbers and weights go out with the glTF and come back, and a part can grow
drv=data/nexus/skel/driver/male/build/male05_body04_src.rpk
"$exe" archive cat "$drv" > "$tmp/drv.rpk"
"$exe" mesh export "@$drv" -o "$tmp/drv.glb" > /dev/null
grep -aq JOINTS_0 "$tmp/drv.glb"
"$exe" mesh import "@$drv" "$tmp/drv.glb" -o "$tmp/drv2.rpk" > /dev/null
cmp "$tmp/drv.rpk" "$tmp/drv2.rpk"
"$exe" mesh export "@$drv" -o "$tmp/d1.glb" "0008f5239ee9c16f_0_*" > /dev/null
"$py" -I tools/modkit/tests/glbedit.py "$tmp/d1.glb" "$tmp/d2.glb" dup0 > /dev/null
"$exe" mesh import "@$drv" "$tmp/d2.glb" --own-buffers -o "$tmp/d3.rpk" > /dev/null
"$exe" mesh info "$tmp/d3.rpk" | grep -q "0008f5239ee9c16f_0_mesh_male05_body04_LOD4  *624 vertices"
echo "ok skinned mesh"

mkdir "$tmp/clone"
"$exe" car clone fe_458 fe_clone -o "$tmp/clone" > /dev/null
"$exe" mod check "$tmp/clone" > /dev/null
# the tutorial's two mods: the clone, and a second one with a reshaped pack as a loose file
"$exe" mod build "$tmp/clone" -o "$tmp/b1" > /dev/null
"$exe" mesh export "$tmp/b1/vehicles/fe_clone.rpk" -o "$tmp/c.glb" "*_BODYShape" > /dev/null
"$py" -I tools/modkit/tests/glbedit.py "$tmp/c.glb" "$tmp/c2.glb" dup5 > /dev/null
mkdir -p "$tmp/shape/files/vehicles"
"$exe" mesh import "$tmp/b1/vehicles/fe_clone.rpk" "$tmp/c2.glb" -o "$tmp/shape/files/vehicles/fe_clone.rpk" > /dev/null
echo '{"id":"shape","name":"shape","version":"1.0","requires":["fe_clone"]}' > "$tmp/shape/mod.json"
"$exe" mod build "$tmp/clone" "$tmp/shape" -o "$tmp/b2" > /dev/null
"$exe" mesh info "$tmp/b2/vehicles/fe_clone.rpk" | grep -q "_5_BODYShape  *120 vertices  *136 triangles"
echo "ok car clone"

# a car texture whose largest levels are in the top mips pack: out and back must give both packs unchanged
"$exe" tex extract @vehicles/ferrari_458_italia.rpk -o "$tmp/tx" "ferrari_458_sticker*" --top @vehicles/ferrari_458_italia_top_mips.rpk > /dev/null
"$exe" tex import @vehicles/ferrari_458_italia.rpk ferrari_458_sticker.tif "$tmp/tx/ferrari_458_sticker.tif.dds" -o "$tmp/tmain.rpk" --top @vehicles/ferrari_458_italia_top_mips.rpk --top-out "$tmp/ttop.rpk" > /dev/null
"$exe" archive cat vehicles/ferrari_458_italia.rpk | cmp - "$tmp/tmain.rpk"
"$exe" archive cat vehicles/ferrari_458_italia_top_mips.rpk | cmp - "$tmp/ttop.rpk"
echo "ok texture with top mips"

"$exe" audio info @audio/fmodstudio/gui.bank | head -1 | grep -q "codec atrac9"
"$exe" audio extract @audio/fmodstudio/gui.bank -o "$tmp/a" UX_Confirm_03_L > /dev/null
"$exe" audio extract @audio/fmodstudio/gui.bank -o "$tmp/b" UX_accolade_ticker_02_L > /dev/null
"$exe" audio replace @audio/fmodstudio/gui.bank UX_Confirm_03_L "$tmp/b/UX_accolade_ticker_02_L.raw" -o "$tmp/gui.bank" > /dev/null
"$exe" audio extract "$tmp/gui.bank" -o "$tmp/c" UX_Confirm_03_L > /dev/null
cmp -n "$(stat -c %s "$tmp/b/UX_accolade_ticker_02_L.raw")" "$tmp/c/UX_Confirm_03_L.raw" "$tmp/b/UX_accolade_ticker_02_L.raw"
# a sound out of the bank, encoded again into another stream (longer than it was): the bank is rebuilt, other streams stay
"$exe" audio wav @audio/fmodstudio/gui.bank UX_Confirm_03_L -o "$tmp/s.wav" > /dev/null
"$exe" audio import @audio/fmodstudio/gui.bank UX_accolade_ticker_02_L "$tmp/s.wav" -o "$tmp/gui2.bank" > /dev/null
"$exe" audio info "$tmp/gui2.bank" | grep -q "UX_accolade_ticker_02_L  *48000 Hz  1 ch  *94208 samples  *20832 bytes"
"$exe" audio extract @audio/fmodstudio/gui.bank -o "$tmp/o1" UX_Move_02_L > /dev/null
"$exe" audio extract "$tmp/gui2.bank" -o "$tmp/o2" UX_Move_02_L > /dev/null
cmp "$tmp/o1/UX_Move_02_L.raw" "$tmp/o2/UX_Move_02_L.raw"
"$exe" audio wav "$tmp/gui2.bank" UX_accolade_ticker_02_L -o "$tmp/s2.wav" > /dev/null
cmp -n 44 "$tmp/s.wav" "$tmp/s2.wav"
echo "ok audio import"
echo "ok audio replace"

"$exe" archive verify > /dev/null
echo "all real data checks passed"
