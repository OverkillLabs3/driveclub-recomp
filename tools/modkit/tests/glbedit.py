import struct, json, sys
# usage: edit.py IN.glb OUT.glb halfNAME... dupNAME...   (suffix match on the object name's last "_N" number)
src, dst = sys.argv[1], sys.argv[2]
half = [a[4:] for a in sys.argv[3:] if a.startswith('half')]
dup = [a[3:] for a in sys.argv[3:] if a.startswith('dup')]
d = open(src, 'rb').read()
jl = struct.unpack('<I', d[12:16])[0]
j = json.loads(d[20:20 + jl])
bo = 20 + jl + 8
def acc(i):
    a = j['accessors'][i]; v = j['bufferViews'][a['bufferView']]; o = bo + v['byteOffset']
    t = {'VEC3': 3, 'VEC2': 2, 'SCALAR': 1}[a['type']]
    if a['componentType'] == 5126: return list(struct.unpack_from('<%df' % (a['count'] * t), d, o))
    return list(struct.unpack_from('<%dH' % (a['count'] * t), d, o))
binb = bytearray(); views = []; accs = []; meshes = []; nodes = []
def add(b):
    while len(binb) % 4: binb.append(0)
    o = len(binb); binb.extend(b); return o
for m in j['meshes']:
    pr = m['primitives'][0]
    pos = acc(pr['attributes']['POSITION']); idx = acc(pr['indices'])
    uv = acc(pr['attributes']['TEXCOORD_0']) if 'TEXCOORD_0' in pr['attributes'] else None
    key = m['name'].split('_')[1]
    n = len(pos) // 3
    if key in half:
        idx = idx[:(len(idx) // 6) * 3]
    if key in dup:
        up = [pos[i] + (float(__import__("os").environ.get("LIFT","0.5")) if i % 3 == 1 else 0.0) for i in range(len(pos))]
        idx = idx + [i + n for i in idx]
        pos = pos + up
        if uv: uv = uv + uv
        n *= 2
    po = add(struct.pack('<%df' % len(pos), *pos)); io = add(struct.pack('<%dH' % len(idx), *idx))
    vb = len(views)
    views.append({'buffer': 0, 'byteOffset': po, 'byteLength': len(pos) * 4}); views.append({'buffer': 0, 'byteOffset': io, 'byteLength': len(idx) * 2})
    a0 = len(accs)
    accs.append({'bufferView': vb, 'componentType': 5126, 'count': n, 'type': 'VEC3', 'min': [-99] * 3, 'max': [99] * 3})
    accs.append({'bufferView': vb + 1, 'componentType': 5123, 'count': len(idx), 'type': 'SCALAR'})
    attrs = {'POSITION': a0}
    if uv:
        uo = add(struct.pack('<%df' % len(uv), *uv)); views.append({'buffer': 0, 'byteOffset': uo, 'byteLength': len(uv) * 4})
        accs.append({'bufferView': len(views) - 1, 'componentType': 5126, 'count': n, 'type': 'VEC2'}); attrs['TEXCOORD_0'] = len(accs) - 1
    meshes.append({'name': m['name'], 'primitives': [{'attributes': attrs, 'indices': a0 + 1, 'mode': 4}]})
    nodes.append({'name': m['name'], 'mesh': len(meshes) - 1})
g = {'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': list(range(len(nodes)))}], 'nodes': nodes, 'meshes': meshes, 'accessors': accs, 'bufferViews': views, 'buffers': [{'byteLength': len(binb)}]}
js = json.dumps(g).encode()
while len(js) % 4: js += b' '
while len(binb) % 4: binb.append(0)
out = struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(binb)) + struct.pack('<II', len(js), 0x4E4F534A) + js + struct.pack('<II', len(binb), 0x004E4942) + bytes(binb)
open(dst, 'wb').write(out)
print('ok', len(nodes))
