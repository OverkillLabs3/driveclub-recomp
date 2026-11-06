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

"$exe" audio info @audio/fmodstudio/gui.bank | head -1 | grep -q "codec atrac9"
"$exe" audio extract @audio/fmodstudio/gui.bank -o "$tmp/a" UX_Confirm_03_L > /dev/null
"$exe" audio extract @audio/fmodstudio/gui.bank -o "$tmp/b" UX_accolade_ticker_02_L > /dev/null
"$exe" audio replace @audio/fmodstudio/gui.bank UX_Confirm_03_L "$tmp/b/UX_accolade_ticker_02_L.raw" -o "$tmp/gui.bank" > /dev/null
"$exe" audio extract "$tmp/gui.bank" -o "$tmp/c" UX_Confirm_03_L > /dev/null
cmp -n "$(stat -c %s "$tmp/b/UX_accolade_ticker_02_L.raw")" "$tmp/c/UX_Confirm_03_L.raw" "$tmp/b/UX_accolade_ticker_02_L.raw"
echo "ok audio replace"

"$exe" archive verify > /dev/null
echo "all real data checks passed"
