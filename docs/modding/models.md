# Models and Blender

`dcmod mesh` turns the geometry of a model pack into glTF 2.0 (`.glb`), which Blender, Unreal Engine, Unity and most other tools open, and writes edited positions back into the pack.

```
dcmod mesh info @vehicles/ferrari_458_italia.rpk
dcmod mesh export @vehicles/ferrari_458_italia.rpk -o ferrari.glb [NAME...]
dcmod mesh import @vehicles/ferrari_458_italia.rpk ferrari.glb -o mymod/vehicles/ferrari_458_italia.rpk
```

## How the geometry is stored

A `mesh` resource depends on a position stream (a `vertex_buffer`) and a second vertex stream with the other vertex data (normal, tangent, texture coordinates, colour and other data; 16 to 44 bytes per vertex; the normal, tangent and texture coordinates are decoded, the rest is kept as it is) and an `index_buffer` (16 bit triangle lists). The mesh holds one record per sub mesh (several copies of it, one per use) with the lowest vertex, highest vertex, first index and index count, and a table at its end (a 3, the lowest vertex, 0, the first index, the index count) that the game draws from. Indices are absolute positions in the vertex buffer. A mesh may use several position buffers; the identifiers of the buffers a record uses come just before it. A car has about 500 sub meshes (the body, each LOD level, wheels, brakes, glass, interior).

Position streams come in four layouts, told apart by the stream format resource they depend on: three floats (12 bytes per vertex), three floats and 4 other bytes (16), three half floats and 2 padding bytes (8), and three half floats and 4 other bytes (12). Cars use the first two or the half layouts; tracks mostly the half layouts. Positions are exported as floats and written back in the stream's own layout (halves are rounded, and a vertex that did not change keeps its exact stored bytes).

## What export and import do

- Export writes one glTF object per sub mesh, named `ID_N_NAME` (the mesh resource's identifier, the sub mesh number, the part name), with positions (the game's axes, Y up), normals and texture coordinates. The normal is the field of kind 2 (type 15) in the attribute stream, three signed 10 bit numbers in 4 bytes with x in the lowest bits; the tangent is the field of kind 5, index 6 (same packing). Normals were checked against the normals of the triangles on the Ferrari body (1870 of 1872 vertices sampled agree). The attribute stream's layout comes from its stream format resource: the field of kind 5 and type 8 is the texture coordinate (two floats). Texture coordinates are checked on real data (body UVs lie in 0 to 1).
- Import reads those objects back by the name prefix and writes their positions, normals and texture coordinates into the pack's vertex streams (a normal that is unchanged after rounding keeps its stored bytes), and their triangles into the index buffer. The result is a complete pack in which everything else is unchanged: exporting and importing without edits gives the original file byte for byte.
- An object may have fewer or more vertices and triangles than the game's. Fewer triangles are written in place. More triangles, or more vertices, are written at the end of the index buffer and the vertex buffers (a part that gets more vertices is written whole, so its old vertices stay unused), and the sub mesh's records and draw table are changed to match. A vertex that did not exist gets its normal from the glTF object (or, if the object has none, from its triangles) and its tangent from the texture coordinates; its other data (colour, skin weights) is copied from the old vertex nearest to it. A part that is only reshaped keeps its normals unless the glTF has different ones. Vertex indices are 16 bit, so a pack's vertex buffer can hold 65535 vertices.
- Not yet: new objects (a part with no counterpart in the pack) and skin weights (parts that bend with a skeleton, such as the wipers, keep the weights of the nearest old vertex). The tangent sign and the fields of kind 5 index 1 and 3 (three floats each) are not decoded; new vertices copy them from the nearest old vertex.

Checked in the game (captures at 1920x1080):

- Normals: every normal of the Ferrari body parts was negated in the glTF and imported; the car's shading in a race changes (the roof and bonnet turn bright and reflective). The main body part (9348 vertices, 14152 triangles) was doubled and raised by half a metre with no normals in the glTF, so its new vertices got normals computed from their triangles: the copy floating above the car is lit and reflects like the real body. Captures: 1902x1033 window, HDR off.
- The Ferrari 458 body was exported, half of its main body part's triangles were left out and a small part was doubled and raised by half a metre, and the glTF was imported: the car has a hole in its right rear and the copies float above it (the game's draw calls, logged, had the new index counts).
- The Ferrari 458 pack was exported, every height doubled, imported and used as a loose file: the whole car (body, glass, wheels) comes out twice as tall.
- The Fraser Valley track pack (`worlds/canada_road_circuit_02_n.rpk`, 2641 sub meshes) was exported, every vertex raised by 60 m and imported: the barns and trees of the scenery float in the sky. The road surface stayed where it was, so it is not (only) in this pack.
- Exporting and importing without changes gives the original bytes for the car and the two tracks tried.

World packs are large (150 MB). They are read through the mods folder like any other pack.

## Blender

`tools/blender/driveclub_modkit.py` is a Blender add-on (tested headless with Blender 5.2: import of the Ferrari body, an edit and the export back into a pack) (Edit > Preferences > Add-ons > Install). It adds File > Import > DriveClub pack and File > Export > DriveClub pack, which call `dcmod` (set its path and the game folders in the add-on preferences). Without the add-on, use `dcmod mesh export`, import the `.glb` with Blender's own glTF importer, and export with the glTF exporter (binary, "Custom Properties" off, object names kept).

## Unreal Engine and others

The `.glb` files import into Unreal (glTF importer plugin or Interchange), Unity and any tool that reads glTF. Keep the object names when exporting back.

## A model from another game

A model made elsewhere (or taken from another game with that game's own tools, where its licence allows it) goes onto the car by taking the place of one of the car's parts:

1. Export the part you want to replace, for example the body: `dcmod mesh export built1/vehicles/my_car.rpk -o body.glb "*_BODYShape"`, and open it in Blender next to your model.
2. Bring your model to the same size and place. The game uses metres and Y up: the Ferrari's body spans x from -0.97 to 0.97 (width), y from 0 to 1.08 (height) and z from -2.0 to 1.66 (length) around the car's origin, with the wheels' centres at the height of their radius. Scale and rotate your model to fit, and check the placement against the exported parts.
3. Give your object the name of the part it replaces (`ID_N_NAME`) and delete the old part's object. One object can replace one part; a part with several sub meshes (the body has one for each material) gets one object per sub mesh, so cut your model by material.
4. Make the UV map: textures are the game's own sheets (the part keeps its material), so either paint a new texture for the material with `dcmod tex` or `tex.set`, or lay out the UVs over the existing texture.
5. Export glTF binary with normals on and import it with `dcmod mesh import`.

Only the geometry comes across: the part keeps the material (shader, textures) of the sub mesh it replaces, skin weights are not imported (a car body has none), and the collision and the physics of the car do not change with its looks. Checked in the game: the Ferrari's main body part (9348 vertices, 14152 triangles) was replaced by an ellipsoid made by a script (2145 vertices, 4096 triangles, normals and UVs, none of it from the car); the race shows a smooth red ellipsoid in place of the body, shaded like the rest of the car (1902x1033 capture, HDR off). A model from a 3D program follows the same path.
