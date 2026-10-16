# SPDX-License-Identifier: GPL-3.0-only
"""Bake the original NGUI HUD manifests into bounded native packages; verify source PNG hashes."""
import hashlib, json, shutil, struct, sys
from pathlib import Path
source, destination = map(Path, sys.argv[1:])
for name in ('aottg-damage-score','aottg-kill-info','aottg-hero-hud'):
    src=source/name; out=destination/name; out.mkdir(parents=True,exist_ok=True)
    m=json.loads((src/'manifest.json').read_text())
    data=bytearray(b'AOHUD001')
    def integer(v): data.extend(struct.pack('<i',int(v)))
    def floats(*v): data.extend(struct.pack('<'+'f'*len(v),*v))
    def string(v): b=v.encode('utf-8'); integer(len(b)); data.extend(b)
    def color(v): floats(*(v[k] for k in ('r','g','b','a')))
    def texture(v):
        file=src/v['texture']; assert file.parent==src
        assert hashlib.sha256(file.read_bytes()).hexdigest()==v['sha256']
        shutil.copy2(file,out/file.name)
    integer(len(m['fonts']))
    for key,f in m['fonts'].items():
        texture(f);integer(key);string(f['texture'])
        for k in ('size','spacingX','spacingY'):integer(f[k])
        integer(len(f['glyphs']))
        for g in f['glyphs']:
            for k in ('index','x','y','width','height','offsetX','offsetY','advance'):integer(g[k])
            integer(len(g['kerning'])//2)
            for v in g['kerning']:integer(v)
    integer(len(m['sprites']))
    for key,s in m['sprites'].items():
        texture(s);string(key);string(s['texture']);integer(s['width']);integer(s['height'])
        floats(*(s['sourceSprite'][k] for k in ('paddingLeft','paddingRight','paddingTop','paddingBottom')))
    widgets={k:v for k,v in m['objects'].items() if v['components'] and 'mColor' in v['components'][0]}
    roles={}
    if name=='aottg-kill-info':
        for k,v in m['objects']['KillInfo']['components'][0].items():
            if isinstance(v,dict) and 'm_PathID' in v:
                for obj,o in m['objects'].items():
                    if o['pathId']==v['m_PathID']:roles[k]=obj
    integer(len(widgets))
    for key,w in widgets.items():
        string(key);t=w['transform'];c=w['components'][0];p=t['m_LocalPosition'];s=t['m_LocalScale']
        x,y,sx,sy=p['x'],p['y'],s['x'],s['y']
        if name=='aottg-kill-info':
            g=m['objects']['small' if key.endswith('_s') else 'big']['transform'];gp=g['m_LocalPosition'];gs=g['m_LocalScale']
            x,y=gp['x']+x*gs['x'],gp['y']+y*gs['y'];sx*=gs['x'];sy*=gs['y']
        floats(x,y,sx,sy);integer(c['mPivot']);integer(c['mDepth']);color(c['mColor'])
        integer(c.get('mFont',{}).get('m_PathID',0));integer(c.get('mEffectStyle',0));color(c.get('mEffectColor',dict(r=0,g=0,b=0,a=0)))
        floats(c.get('mEffectDistance',{}).get('x',0),c.get('mEffectDistance',{}).get('y',0));string(c.get('mSpriteName',''))
    integer(len(roles))
    for k,v in roles.items():string(k);string(v)
    (out/'hud.bin').write_bytes(data);shutil.copy2(src/'manifest.json',out/'manifest.json')
    print(name,len(data),'bytes')
