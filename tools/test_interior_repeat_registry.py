import unittest

import generate_interior_repeat_registry as interior
from test_world_repeat_registry import material, shader


class InteriorOwnershipTests(unittest.TestCase):
    def test_plain_grain_cohort_is_source_based_not_display_name(self):
        pixel = shader(1, 42)
        mat = material(1, [pixel["uid"]],
                       "assets/noodle/materials/vehicle/leather.material")
        mat["names"] = ["renamed-opaque-panel"]
        self.assertEqual(interior.classify([mat, pixel]), [42])

    def test_textured_plastic_can_use_existing_grain_source(self):
        pixel = shader(1, 42)
        # Native car instances with these display names reference leather.material.
        for name in ("plastic_coloured_textured_fine", "plastic_black_weave"):
            mat = material(1, [pixel["uid"]],
                           "assets/noodle/materials/vehicle/leather.material")
            mat["names"] = [name]
            self.assertEqual(interior.classify([mat, pixel]), [42])
            # A plastic label cannot admit a different unqualified material.
            mat["sources"] = ["assets/noodle/materials/vehicle/plastic_normalmapped.material"]
            self.assertEqual(interior.classify([mat, pixel]), [])

    def test_every_material_owner_must_belong_to_cohort(self):
        a, b = shader(1, 42), shader(2, 42)
        leather = material(1, [a["uid"]],
                           "assets/noodle/materials/vehicle/leather.material")
        glow = material(2, [b["uid"]],
                        "assets/noodle/materials/vehicle/leather_glow.material")
        self.assertEqual(interior.classify([leather, glow, a, b]), [42])
        for excluded in ("leather_perforated", "leather_bump", "leather_rain",
                         "paint_gloss_interior", "glass_simple_interior",
                         "simple_bakedtextures_uvscale", "plastic_normalmapped"):
            glow["sources"] = ["assets/noodle/materials/vehicle/" + excluded + ".material"]
            self.assertEqual(interior.classify([leather, glow, a, b]), [])

    def test_every_source_of_each_owner_must_be_eligible(self):
        pixel = shader(1, 42)
        mat = material(1, [pixel["uid"]],
                       "assets/noodle/materials/vehicle/fuzzy_felt.material")
        mat["sources"].append("assets/noodle/materials/vehicle/glass_simple_interior.material")
        self.assertEqual(interior.classify([mat, pixel]), [])
        mat["sources"] = []
        self.assertEqual(interior.classify([mat, pixel]), [])

    def test_fabric_stitch_owner_only_grants_program_eligibility(self):
        pixel = shader(1, 42)
        mat = material(1, [pixel["uid"]],
                       "assets/noodle/materials/vehicle/fuzzy_felt_stitched.material")
        self.assertEqual(interior.classify([mat, pixel]), [42])
        text = interior.render([42])
        self.assertNotIn("fuzzy_felt", text)
        self.assertNotIn("texture_id", text)
        self.assertEqual(text, interior.render([42]))


if __name__ == "__main__":
    unittest.main()
