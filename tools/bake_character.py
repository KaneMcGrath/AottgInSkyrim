# SPDX-License-Identifier: GPL-3.0-only
"""Convert the validated GLB to a bounded, dependency-free native rig package.

The game uses only hero.bin and PNGs. Python and GLB parsing are build tools.
Matrices retain glTF column-major byte ordering (= System.Numerics row-major).
"""
import argparse, bisect, hashlib, json, math, pathlib, struct

def bake(source, target):
    manifest = json.loads((source / 'manifest.json').read_text())
    raw = (source / manifest['asset']).read_bytes()
    if not manifest['complete'] or hashlib.sha256(raw).hexdigest() != manifest['sha256']:
        raise ValueError('Incomplete or mismatched character package')
    magic, version, size = struct.unpack_from('<III', raw)
    assert magic == 0x46546c67 and version == 2 and size == len(raw)
    offset, chunks = 12, {}
    while offset < size:
        length, kind = struct.unpack_from('<II', raw, offset)
        chunks[kind] = raw[offset+8:offset+8+length]; offset += 8+length
    gltf = json.loads(chunks[0x4e4f534a]); data = chunks[0x004e4942]
    def view(i):
        v = gltf['bufferViews'][i]; start = v.get('byteOffset', 0)
        return data[start:start+v['byteLength']]
    def accessor(i):
        a = gltf['accessors'][i]; v = gltf['bufferViews'][a['bufferView']]
        assert 'sparse' not in a and not a.get('normalized', False)
        fmt = {5121:'B',5123:'H',5125:'I',5126:'f'}[a['componentType']]
        n = {'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4,'MAT4':16}[a['type']]
        stride = v.get('byteStride', struct.calcsize('<'+fmt*n))
        start = v.get('byteOffset', 0) + a.get('byteOffset', 0)
        return [struct.unpack_from('<'+fmt*n, data, start+j*stride) for j in range(a['count'])]
    nodes = gltf['nodes']; parents = [-1]*len(nodes)
    for i,n in enumerate(nodes):
        assert 'matrix' not in n, 'Bake requires TRS nodes'
        for c in n.get('children', []): parents[c] = i
    order = []
    def visit(i):
        order.append(i)
        for c in nodes[i].get('children', []): visit(c)
    for i,p in enumerate(parents):
        if p < 0: visit(i)
    rests = [tuple(n.get('translation',[0,0,0])+n.get('rotation',[0,0,0,1])+n.get('scale',[1,1,1])) for n in nodes]
    target.mkdir(parents=True, exist_ok=True)
    out = bytearray(b'AOTRIG02')
    def ints(*v): out.extend(struct.pack('<'+'I'*len(v), *v))
    def signed(v): out.extend(struct.pack('<i',v))
    def floats(v): out.extend(struct.pack('<'+'f'*len(v),*v))
    def string(v):
        b=v.encode(); ints(len(b)); out.extend(b)
    materials = gltf.get('materials',[])
    primitives = [(i,p) for i,n in enumerate(nodes) if 'mesh' in n for p in gltf['meshes'][n['mesh']]['primitives']]
    ints(len(nodes),len(primitives),len(materials),len(gltf['animations']))
    floats([manifest['metersPerSourceUnit']])
    for i,n in enumerate(nodes): string(n.get('name','')); signed(parents[i]); floats(rests[i])
    for i in order: ints(i)
    for i,m in enumerate(materials):
        pbr=m.get('pbrMetallicRoughness',{}); floats(pbr.get('baseColorFactor',[1,1,1,1]))
        ints({'OPAQUE':0,'MASK':1,'BLEND':2}[m.get('alphaMode','OPAQUE')]); floats([m.get('alphaCutoff',0.5)])
        filename=''
        if 'baseColorTexture' in pbr:
            t=gltf['textures'][pbr['baseColorTexture']['index']]; im=gltf['images'][t['source']]
            assert im['mimeType']=='image/png'
            filename=f'material-{i}.png'; (target/filename).write_bytes(view(im['bufferView']))
        string(filename)
    for ni,p in primitives:
        attrs=p['attributes']; positions=accessor(attrs['POSITION']); normals=accessor(attrs['NORMAL']); uv=accessor(attrs['TEXCOORD_0'])
        indices=[v[0] for v in accessor(p['indices'])]
        skin=gltf['skins'][nodes[ni]['skin']] if 'skin' in nodes[ni] else None
        joints=accessor(attrs['JOINTS_0']) if skin else [(0,0,0,0)]*len(positions)
        weights=accessor(attrs['WEIGHTS_0']) if skin else [(0,0,0,0)]*len(positions)
        string(nodes[ni].get('name','')); ints(ni); signed(p.get('material',-1))
        ints(len(positions),len(indices),len(skin['joints']) if skin else 0)
        for pos,norm,tex,j,w in zip(positions,normals,uv,joints,weights):
            floats(pos+norm+tex); ints(*j); floats(w)
        ints(*indices)
        if skin:
            binds=accessor(skin['inverseBindMatrices'])
            for j,b in zip(skin['joints'],binds): ints(j); floats(b)
    for a in gltf['animations']:
        tracks=[]; duration=0; key_times={0.0}
        for ch in a['channels']:
            s=a['samplers'][ch['sampler']]; assert s.get('interpolation','LINEAR')=='LINEAR'
            times=[v[0] for v in accessor(s['input'])]; values=accessor(s['output']); duration=max(duration,times[-1])
            tracks.append((ch['target']['node'],ch['target']['path'],times,values))
            key_times.update(times)
        times_grid=sorted(key_times); frames=len(times_grid); string(a['name']); floats([duration]); ints(frames); floats(times_grid)
        for time in times_grid:
            locals_=[list(r) for r in rests]
            for ni,path,times,values in tracks:
                hi=min(len(times)-1,bisect.bisect_right(times,time)); lo=max(0,hi-1)
                t=0 if times[hi]==times[lo] else max(0,min(1,(time-times[lo])/(times[hi]-times[lo])))
                v1,v2=values[lo],values[hi]
                if path=='rotation':
                    dot=sum(x*y for x,y in zip(v1,v2)); v2=tuple(-v for v in v2) if dot<0 else v2; dot=abs(dot)
                    if dot<0.9995:
                        angle=math.acos(max(-1,min(1,dot))); d=math.sin(angle)
                        value=[(x*math.sin((1-t)*angle)+y*math.sin(t*angle))/d for x,y in zip(v1,v2)]
                    else: value=[x+(y-x)*t for x,y in zip(v1,v2)]
                    length=math.sqrt(sum(x*x for x in value)); value=[x/length for x in value]
                    locals_[ni][3:7]=value
                elif path=='translation': locals_[ni][:3]=[x+(y-x)*t for x,y in zip(v1,v2)]
                elif path=='scale': locals_[ni][7:]=[x+(y-x)*t for x,y in zip(v1,v2)]
                else: raise ValueError(path)
            for local in locals_: floats(local)
    (target/'hero.bin').write_bytes(out)
    report={'sourceSha256':manifest['sha256'],'nativeSha256':hashlib.sha256(out).hexdigest(),'bytes':len(out),'nodes':len(nodes),'primitives':len(primitives),'clips':len(gltf['animations']),'sampling':'original key timestamps, local TRS','sourceCapture':manifest['sourceCapture']}
    (target/'manifest.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))

if __name__=='__main__':
    p=argparse.ArgumentParser(); p.add_argument('source',type=pathlib.Path); p.add_argument('target',type=pathlib.Path)
    args=p.parse_args(); bake(args.source,args.target)
