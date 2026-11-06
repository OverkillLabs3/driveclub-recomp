# SPDX-License-Identifier: GPL-2.0-or-later
"""Blender add-on: import DriveClub model packs and write edited geometry back (uses dcmod, see docs/modding/models.md)."""
bl_info = {
    "name": "DriveClub mod kit",
    "author": "DriveClub Recomp",
    "version": (1, 0, 0),
    "blender": (3, 6, 0),
    "location": "File > Import / Export > DriveClub pack",
    "description": "Edit the geometry of DriveClub model packs through dcmod mesh",
    "category": "Import-Export",
}

import os
import subprocess
import tempfile

import bpy
from bpy.props import StringProperty
from bpy.types import AddonPreferences, Operator


class DCModPrefs(AddonPreferences):
    bl_idname = __name__

    dcmod: StringProperty(name="dcmod program", subtype="FILE_PATH", default="dcmod")
    update_dir: StringProperty(name="Game update folder", subtype="DIR_PATH", default="")
    base_dir: StringProperty(name="Game base folder", subtype="DIR_PATH", default="")

    def draw(self, context):
        col = self.layout.column()
        col.prop(self, "dcmod")
        col.prop(self, "update_dir")
        col.prop(self, "base_dir")


def run_dcmod(context, args):
    addon = context.preferences.addons.get(__name__)
    prefs = addon.preferences if addon else None
    cmd = [bpy.path.abspath(prefs.dcmod) if prefs and prefs.dcmod else os.environ.get("DCMOD", "dcmod")] + args
    if prefs and prefs.update_dir:
        cmd += ["--update", bpy.path.abspath(prefs.update_dir)]
    if prefs and prefs.base_dir:
        cmd += ["--base", bpy.path.abspath(prefs.base_dir)]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError((result.stderr or result.stdout).strip() or "dcmod failed")
    return result.stdout


class DC_OT_import(Operator):
    bl_idname = "import_scene.driveclub_pack"
    bl_label = "Import DriveClub pack"
    bl_description = "Pack file, or @path for a file inside the game's archive (for example @vehicles/ferrari_458_italia.rpk)"

    pack: StringProperty(name="Pack", default="@vehicles/ferrari_458_italia.rpk")
    pattern: StringProperty(name="Parts", description="Optional name pattern, such as *BODY*", default="")

    def execute(self, context):
        glb = os.path.join(tempfile.mkdtemp(prefix="dcmod_"), "pack.glb")
        args = ["mesh", "export", self.pack, "-o", glb] + ([self.pattern] if self.pattern else [])
        try:
            run_dcmod(context, args)
        except RuntimeError as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        bpy.ops.import_scene.gltf(filepath=glb)
        context.scene["driveclub_pack"] = self.pack
        return {"FINISHED"}

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)


class DC_OT_export(Operator):
    bl_idname = "export_scene.driveclub_pack"
    bl_label = "Export DriveClub pack"
    bl_description = "Write the selected objects' positions into a copy of the pack. Keep the object names (ID_N_NAME)"

    pack: StringProperty(name="Pack", default="")
    filepath: StringProperty(name="Output pack", subtype="FILE_PATH", default="")

    def execute(self, context):
        pack = self.pack or context.scene.get("driveclub_pack", "")
        if not pack or not self.filepath:
            self.report({"ERROR"}, "set the pack and the output file")
            return {"CANCELLED"}
        glb = os.path.join(tempfile.mkdtemp(prefix="dcmod_"), "edit.glb")
        bpy.ops.export_scene.gltf(
            filepath=glb, export_format="GLB", use_selection=True, export_apply=False,
            export_normals=False, export_texcoords=False, export_materials="NONE",
            export_cameras=False, export_lights=False, export_extras=False,
        )
        try:
            out = run_dcmod(context, ["mesh", "import", pack, glb, "-o", bpy.path.abspath(self.filepath)])
        except RuntimeError as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        self.report({"INFO"}, out.strip())
        return {"FINISHED"}

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)


def menu_import(self, context):
    self.layout.operator(DC_OT_import.bl_idname, text="DriveClub pack (dcmod)")


def menu_export(self, context):
    self.layout.operator(DC_OT_export.bl_idname, text="DriveClub pack (dcmod)")


classes = (DCModPrefs, DC_OT_import, DC_OT_export)


def register():
    for c in classes:
        bpy.utils.register_class(c)
    bpy.types.TOPBAR_MT_file_import.append(menu_import)
    bpy.types.TOPBAR_MT_file_export.append(menu_export)


def unregister():
    bpy.types.TOPBAR_MT_file_import.remove(menu_import)
    bpy.types.TOPBAR_MT_file_export.remove(menu_export)
    for c in reversed(classes):
        bpy.utils.unregister_class(c)


if __name__ == "__main__":
    register()
