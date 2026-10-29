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


def material(uid, dependencies, source):
    return dict(uid=(6 << 48) | uid, dependencies=dependencies,
                sources=[source], names=["arbitrary-renamed-material"])


class OwnershipTests(unittest.TestCase):
    def test_world_programs_are_derived_without_material_or_texture_names(self):
        pixel = shader(1, 42)
        world = material(1, [pixel["uid"], pixel["uid"]], "assets/noodle/materials/atg1/arbitrary.material")
        self.assertEqual(registry.classify([world, pixel]), [42])

    def test_shared_program_is_excluded_even_across_different_resource_ids(self):
        a, b = shader(1, 42), shader(2, 42)
        world = material(1, [a["uid"]], "assets/noodle/materials/atg1/a.material")
        vehicle = material(2, [b["uid"]], "assets/noodle/materials/vehicle/b.material")
        self.assertEqual(registry.classify([world, vehicle, a, b]), [])

    def test_missing_provenance_and_non_pixel_stages_are_excluded(self):
        a, b = shader(1, 42), shader(2, 43)
        b["names"] = ["VertexShader"]
        mat = material(1, [a["uid"], b["uid"]], "")
        self.assertEqual(registry.classify([mat, a, b]), [])

    def test_generation_is_deterministic(self):
        self.assertEqual(registry.render([2, 9]), registry.render([2, 9]))
        self.assertNotIn("arbitrary", registry.render([2, 9]))


if __name__ == "__main__":
    unittest.main()
