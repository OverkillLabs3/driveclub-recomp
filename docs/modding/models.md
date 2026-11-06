# Models and Blender

`dcmod mesh` turns the geometry of a model pack into glTF 2.0 (`.glb`), which Blender, Unreal Engine, Unity and most other tools open, and writes edited positions back into the pack.

```
dcmod mesh info @vehicles/ferrari_458_italia.rpk
dcmod mesh export @vehicles/ferrari_458_italia.rpk -o ferrari.glb [NAME...]
dcmod mesh import @vehicles/ferrari_458_italia.rpk ferrari.glb -o mymod/vehicles/ferrari_458_italia.rpk
```

## How the geometry is stored

A `mesh` resource depends on a position stream (a `vertex_buffer` with 12 byte XYZ floats), a second vertex stream with the other vertex data (normals, texture coordinates, skin weights; 16 to 44 bytes per vertex, not decoded yet) and an `index_buffer` (16 bit triangle lists). The mesh holds one record per sub mesh: lowest vertex, highest vertex, first index, index count. Indices are absolute positions in the vertex buffer. A mesh may use several position buffers; the identifiers of the buffers a record uses come just before it. A car has about 500 sub meshes (the body, each LOD level, wheels, brakes, glass, interior).

## What export and import do

- Export writes one glTF object per sub mesh, named `ID_N_NAME` (the mesh resource's identifier, the sub mesh number, the part name), with positions (the game's axes, Y up) and texture coordinates. The attribute stream's layout comes from its stream format resource: the field of kind 5 and type 8 is the texture coordinate (two floats). Texture coordinates are checked on real data (body UVs lie in 0 to 1) and are exported only; import does not change them.
- Import reads those objects back by the name prefix and writes their positions into the pack's position stream, and their triangles into the index buffer. The result is a complete pack in which everything else is unchanged: exporting and importing without edits gives the original file byte for byte.
- An object has to keep its number of vertices and triangles. Positions can move and triangles can be reconnected. Normals, texture coordinates and skin weights stay as the game has them, so a heavy reshape can show wrong shading until the second stream is understood.
- Not yet: new objects, a different number of vertices, imported models from other games. These need the second vertex stream, which is not decoded.

Checked in the game: the Ferrari 458 pack was exported, every position stretched upward, imported and used as a loose file; the race starts and the car shows the changed shape.

## Blender

`tools/blender/driveclub_modkit.py` is a Blender add-on (tested headless with Blender 5.2: import of the Ferrari body, an edit and the export back into a pack) (Edit > Preferences > Add-ons > Install). It adds File > Import > DriveClub pack and File > Export > DriveClub pack, which call `dcmod` (set its path and the game folders in the add-on preferences). Without the add-on, use `dcmod mesh export`, import the `.glb` with Blender's own glTF importer, and export with the glTF exporter (binary, "Custom Properties" off, object names kept).

## Unreal Engine and others

The `.glb` files import into Unreal (glTF importer plugin or Interchange), Unity and any tool that reads glTF. Keep the object names when exporting back.
