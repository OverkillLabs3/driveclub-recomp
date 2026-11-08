import importlib.util
from pathlib import Path
import struct
import unittest

spec = importlib.util.spec_from_file_location("registry", Path(__file__).with_name("generate_world_repeat_registry.py"))
registry = importlib.util.module_from_spec(spec)
spec.loader.exec_module(registry)


def shader(uid, program):
    blob = b"OrbShdr" + bytes(9) + struct.pack("<Q", program)
    return dict(uid=(29 << 48) | uid, package=blob, offset=0, size=len(blob), names=["PixelShader"])


def material(uid, dependencies, *sources):
    return dict(uid=(6 << 48) | uid, dependencies=dependencies,
                sources=list(sources), names=["arbitrary-renamed-instance"])


def world(family):
    return registry.WORLD_MATERIAL_ROOT + family + ".material"


class OwnershipTests(unittest.TestCase):
    def test_positive_surface_categories_survive_instance_renaming(self):
        for family in ("road", "road2_norway", "groundblend", "ground_salt_plain",
                       "concrete", "concretetiles", "brick_pom", "pavement2", "pavment_dlc",
                       "tiles", "tarmac", "stonewall", "rockwall"):
            with self.subTest(family=family):
                pixel = shader(1, 42)
                mat = material(1, [pixel["uid"], pixel["uid"]], world(family))
                self.assertEqual(registry.classify([mat, pixel]), [42])
                mat["names"] = ["grass-looking-renamed-instance"]
                self.assertEqual(registry.classify([mat, pixel]), [42])

    def test_vegetation_complex_and_unknown_families_are_excluded(self):
        for family in ("grass", "grass_vertexgradient", "foliagegroundblend", "leaves_procedural",
                       "needles", "bark", "barkeucalyptus", "branch", "trunk_snow", "treewall",
                       "trees_distant_y", "roof_vegetation", "rock_vegitation", "stone_vegitation",
                       "tea_plantation", "clothsimflag", "flag", "metal_deform", "rotatingmeshaxis",
                       "rockingboat", "circlingbirds", "basic", "terrain", "rock", "cliff",
                       "road_decal_alpha", "glass", "waterlake", "debuglightingtests"):
            with self.subTest(family=family):
                pixel = shader(1, 42)
                mat = material(1, [pixel["uid"]], world(family))
                mat["names"] = ["road-and-concrete-looking-instance"]
                self.assertEqual(registry.classify([mat, pixel]), [])

    def test_authored_markings_and_decals_are_not_physical_surface_grain(self):
        for family in ("roadline", "roadlinedotted", "roadlinerotate", "road_markings", "roadmark",
                       "road_decal", "road_decal_baked", "road_patch_decal", "ground_decal",
                       "paintmark", "tarmac_markings_cracks_puddles_oilstains", "road_sign",
                       "concrete_simple_painted_design", "brick_mural", "tiles_logo"):
            with self.subTest(family=family):
                pixel = shader(1, 42)
                mat = material(1, [pixel["uid"]], world(family))
                self.assertEqual(registry.classify([mat, pixel]), [])

    def test_shared_program_is_excluded_even_across_different_resource_ids(self):
        for source in (world("bark"), world("terrain"), world("roadline"), "",
                       "assets/noodle/materials/vehicle/paint_gloss_interior.material"):
            with self.subTest(source=source):
                a, b = shader(1, 42), shader(2, 42)
                good = material(1, [a["uid"]], world("road"))
                bad = material(2, [b["uid"]], source)
                self.assertEqual(registry.classify([good, bad, a, b]), [])

    def test_every_source_of_each_material_must_qualify(self):
        pixel = shader(1, 42)
        mat = material(1, [pixel["uid"]], world("road"), world("concrete"))
        self.assertEqual(registry.classify([mat, pixel]), [42])
        for sources in ([], [world("road"), world("grass")], [world("road"), ""]):
            mat["sources"] = sources
            self.assertEqual(registry.classify([mat, pixel]), [])

    def test_separate_cohort_reuses_closure_without_entering_world(self):
        pixel = shader(1, 42)
        mat = material(1, [pixel["uid"]], "assets/noodle/materials/vehicle/interior_surface.material")
        cabin_source = lambda source: source.startswith("assets/noodle/materials/vehicle/")
        self.assertEqual(registry.classify([mat, pixel]), [])
        self.assertEqual(registry.classify([mat, pixel], cabin_source), [42])
        road = material(2, [pixel["uid"]], world("road"))
        self.assertEqual(registry.classify([mat, road, pixel], cabin_source), [])

    def test_missing_provenance_and_non_pixel_stages_are_excluded(self):
        a, b = shader(1, 42), shader(2, 43)
        b["names"] = ["VertexShader"]
        mat = material(1, [a["uid"], b["uid"]], world("road"))
        self.assertEqual(registry.classify([mat, a, b]), [42])
        mat["sources"] = []
        self.assertEqual(registry.classify([mat, a, b]), [])
        for source in (world("../road"), world("road/nested"),
                       world("road").replace(".material", ".unknown"),
                       "assets/noodle/materials/tests/road.material"):
            self.assertFalse(registry.is_world_surface_source(source))

    def test_truncated_program_metadata_is_rejected(self):
        pixel = shader(1, 42)
        pixel["size"] -= 1
        mat = material(1, [pixel["uid"]], world("road"))
        with self.assertRaises(ValueError):
            registry.classify([mat, pixel])

    def test_generation_is_deterministic(self):
        a, b = shader(1, 9), shader(2, 2)
        mat = material(1, [a["uid"], b["uid"]], world("road"))
        self.assertEqual(registry.classify([mat, a, b]), registry.classify([b, a, mat]))
        self.assertEqual(registry.render([2, 9]), registry.render([2, 9]))
        self.assertNotIn("arbitrary", registry.render([2, 9]))
        self.assertNotIn(world("road"), registry.render([2, 9]))


if __name__ == "__main__":
    unittest.main()
