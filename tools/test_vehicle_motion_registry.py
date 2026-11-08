import struct,unittest
import generate_vehicle_motion_registry as g
def shader(uid,program):
    blob=b'OrbShdr'+bytes(9)+struct.pack('<Q',program)
    return dict(uid=(29<<48)|uid,names=['PixelShader'],sources=[''],dependencies=[],package=blob,offset=0,size=len(blob))
def material(uid,shader_ids,sources,name='renamable instance'):
    return dict(uid=(6<<48)|uid,names=[name],sources=sources,dependencies=[(29<<48)|s for s in shader_ids])
class RegistryTests(unittest.TestCase):
    def test_vehicle_and_renamed_instance(self):
        e=[shader(1,17),material(1,[1],[g.ROOT+'paint_gloss.material'])]
        self.assertEqual(g.classify(e),[17]);e[1]['names']=['completely different'];self.assertEqual(g.classify(e),[17])
    def test_shared_vehicle_owners(self):
        self.assertEqual(g.classify([shader(1,17),material(1,[1],[g.ROOT+'paint_gloss.material']),material(2,[1],[g.ROOT+'plastic_normalmapped.material'])]),[17])
    def test_alias_shader_shared_world_denied(self):
        self.assertEqual(g.classify([shader(1,17),shader(2,17),material(1,[1],[g.ROOT+'paint_gloss.material']),material(2,[2],['assets/noodle/materials/atg1/terrain_canada.material'])]),[])
    def test_every_source_required(self):
        self.assertEqual(g.classify([shader(1,17),material(1,[1],[g.ROOT+'leather.material','unknown.material'])]),[])
    def test_missing_source_denied(self):
        self.assertEqual(g.classify([shader(1,17),material(1,[1],[])]),[])
    def test_unknown_owner_denied(self):
        self.assertEqual(g.classify([shader(1,17),material(1,[1],[g.ROOT+'leather.material']),material(2,[1],[''])]),[])
    def test_world_terrain_trees_unknown_denied(self):
        for source in ['assets/noodle/materials/atg1/terrain.material','assets/noodle/materials/atg1/tree.material','assets/noodle/materials/basic.material']:
            self.assertFalse(g.vehicle_source(source))
    def test_invalid_rpk_header(self):
        with self.assertRaises(ValueError):g.resources(b'notanrpk')
    def test_truncated_shader_metadata(self):
        e=shader(1,17);e['package']=b'OrbShdr';e['size']=7
        with self.assertRaises(ValueError):g.classify([e,material(1,[1],[g.ROOT+'paint.material'])])
    def test_deterministic_and_no_instructions(self):
        e=[shader(1,20),shader(2,10),material(1,[2,1],[g.ROOT+'paint.material'])]
        self.assertEqual(g.classify(e),[10,20]);self.assertEqual(g.classify(e[::-1]),[10,20]);self.assertNotIn('paint.material',g.render(g.classify(e)))
if __name__=='__main__':unittest.main()
