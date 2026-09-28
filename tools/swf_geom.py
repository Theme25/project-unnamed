import xml.etree.ElementTree as ET, sys, json
XML=sys.argv[1] if len(sys.argv)>1 and __name__=='__main__' else None

chars={}
def load(path):
    global root
    root=ET.parse(path).getroot(); walk(root)
def walk(node):
    for it in node.iter('item'):
        t=it.get('type','')
        if t.startswith('DefineShape'): chars[int(it.get('shapeId'))]=('shape',it)
        elif t.startswith('DefineMorphShape'): chars[int(it.get('characterId'))]=('morph',it)
        elif t=='DefineSpriteTag': chars[int(it.get('spriteId'))]=('sprite',it)
def rect(e):
    return None if e is None else (int(e.get('Xmin')),int(e.get('Ymin')),int(e.get('Xmax')),int(e.get('Ymax')))
def mat(it):
    m=it.find('matrix')
    if m is None: return (1,0,0,1,0,0)
    sx=sy=1.0; r0=r1=0.0
    if m.get('hasScale')=='true': sx=int(m.get('scaleX'))/65536.0; sy=int(m.get('scaleY'))/65536.0
    if m.get('hasRotate')=='true': r0=int(m.get('rotateSkew0'))/65536.0; r1=int(m.get('rotateSkew1'))/65536.0
    return (sx,r0,r1,sy,int(m.get('translateX')),int(m.get('translateY')))
def xrect(r,m):
    a,b,c,d,tx,ty=m
    xs=[];ys=[]
    for x,y in ((r[0],r[1]),(r[2],r[1]),(r[0],r[3]),(r[2],r[3])):
        xs.append(a*x+c*y+tx); ys.append(b*x+d*y+ty)
    return (min(xs),min(ys),max(xs),max(ys))
def frame1_children(sp):
    kids=[]
    for it in sp.find('subTags'):
        if it.get('type')=='ShowFrameTag': break
        if it.get('type','').startswith('PlaceObject'):
            kids.append(it)
    return kids
def bounds(cid,depth=0,ratio=0):
    if cid not in chars: return None
    kind,it=chars[cid]
    if kind=='shape': return rect(it.find('shapeBounds'))
    if kind=='morph':
        a=rect(it.find('startBounds')); b=rect(it.find('endBounds')); t=ratio/65535.0
        return tuple(a[i]+(b[i]-a[i])*t for i in range(4))
    u=None
    for k in frame1_children(it):
        if k.get('placeFlagHasCharacter')!='true': continue
        b=bounds(int(k.get('characterId')),depth+1,int(k.get('ratio') or 0))
        if b is None: continue
        b=xrect(b,mat(k))
        u=b if u is None else (min(u[0],b[0]),min(u[1],b[1]),max(u[2],b[2]),max(u[3],b[3]))
    return u
def level(sid):
    out={}
    for k in frame1_children(chars[sid][1]):
        if k.get('placeFlagHasCharacter')!='true': continue
        cid=int(k.get('characterId')); m=mat(k)
        out[k.get('name') or 'depth%s'%k.get('depth')]=dict(cid=cid,m=m,b=bounds(cid,0,int(k.get('ratio') or 0)))
    return out
