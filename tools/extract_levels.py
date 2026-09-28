#!/usr/bin/env python3
"""Extract named first-frame placements of every Levels.Level_N sprite from a
JPEXS -swf2xml dump. Output: JSON {level: {name: {tx,ty,a,b,c,d,char}}} with
tx/ty in twips and a,b,c,d the raw 16.16 matrix (a=scaleX, b=rotateSkew0,
c=rotateSkew1, d=scaleY; identity = 65536,0,0,65536)."""
import json, re, sys
x = open(sys.argv[1]).read()
syms = {}
for m in re.finditer(r'<item type="SymbolClassTag">\s*<tags>(.*?)</tags>\s*<names>(.*?)</names>', x, re.S):
    for i, n in zip(re.findall(r'<item>(\d+)</item>', m.group(1)), re.findall(r'<item>([^<]*)</item>', m.group(2))):
        syms[n] = int(i)
out = {}
for n, sid in syms.items():
    mm = re.match(r'Levels\.Level_(\d+)$', n)
    if not mm:
        continue
    start = x.index(f'<item type="DefineSpriteTag" frameCount=')
    start = re.search(rf'<item type="DefineSpriteTag"[^>]*spriteId="{sid}">', x).start()
    end = x.index('<item type="ShowFrameTag"/>', start)  # first frame only
    body = x[start:end]
    places = {}
    for p in re.finditer(r'<item type="PlaceObject[23]?Tag"([^>]*)>\s*<matrix([^>]*)/>', body):
        attrs = dict(re.findall(r'(\w+)="([^"]*)"', p.group(1)))
        mat = dict(re.findall(r'(\w+)="([^"]*)"', p.group(2)))
        if 'name' not in attrs:
            continue
        a = int(mat['scaleX']) if mat['hasScale'] == 'true' else 65536
        d = int(mat['scaleY']) if mat['hasScale'] == 'true' else 65536
        b = int(mat['rotateSkew0']) if mat['hasRotate'] == 'true' else 0
        c = int(mat['rotateSkew1']) if mat['hasRotate'] == 'true' else 0
        places[attrs['name']] = dict(tx=int(mat['translateX']), ty=int(mat['translateY']), a=a, b=b, c=c, d=d,
                                     char=int(attrs.get('characterId', -1)), depth=int(attrs['depth']))
    out[int(mm.group(1))] = places
json.dump(out, sys.stdout, indent=1, sort_keys=True)
