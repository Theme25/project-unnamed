#!/usr/bin/env python3
"""Insert a '#ENV' line into the logging mod's dumps WITHOUT recompiling Game.

Recompiling Game.as with a decompiler changes unrelated methods (docs/STATS_LOGGING.md section 2), so this tool
patches bytecode directly. Into three methods of Game it inserts a call to Shipik.envLine(level), a static
function that must already exist in the SWF (docs/mathspikes2/Shipik.as, compiled in by FFDec):

  dbgLevelCalib (key ], rb1_calib_L<id>.tsv): after `L = this.m_currLevel`  ->  out.push(Shipik.envLine(L))
  dbgSpikeCalib (E8 spike dump in rb1_calib.tsv): at entry             ->  out.push(Shipik.envLine(m_currLevel))
  dbgExport     (rb1_stats.tsv):              hdr = Shipik.envLine(m_currLevel) + "\\n" + hdr

Every inserted sequence sits where no branch, exception range or later code depends on byte offsets across it
(checked below), so nothing else in the method moves relative to itself.

usage: abc_patch_env.py in.swf out.swf
"""
import sys, zlib, struct

# ---------------------------------------------------------------- byte helpers
class R:
    def __init__(s, b, p=0): s.b, s.p = b, p
    def u8(s): v = s.b[s.p]; s.p += 1; return v
    def u16(s): v = struct.unpack_from('<H', s.b, s.p)[0]; s.p += 2; return v
    def u32(s): v = struct.unpack_from('<I', s.b, s.p)[0]; s.p += 4; return v
    def d64(s): s.p += 8
    def u30(s):
        v = 0; sh = 0
        while True:
            c = s.b[s.p]; s.p += 1
            v |= (c & 0x7f) << sh; sh += 7
            if not c & 0x80 or sh >= 35: return v
    def s24(s):
        v = s.b[s.p] | s.b[s.p+1] << 8 | s.b[s.p+2] << 16; s.p += 3
        return v - (1 << 24) if v & 0x800000 else v
    def take(s, n): v = s.b[s.p:s.p+n]; s.p += n; return v

def enc_u30(v):
    out = bytearray()
    while True:
        c = v & 0x7f; v >>= 7
        if v: out.append(c | 0x80)
        else: out.append(c); return bytes(out)

# ---------------------------------------------------------------- ABC parse (enough to find names and bodies)
def parse_abc(b):
    r = R(b)
    r.u16(); r.u16()
    n = r.u30(); [r.u30() for _ in range(max(n - 1, 0))]          # int
    n = r.u30(); [r.u30() for _ in range(max(n - 1, 0))]          # uint
    n = r.u30(); [r.d64() for _ in range(max(n - 1, 0))]          # double
    n = r.u30(); strings = [None]
    for _ in range(max(n - 1, 0)):
        l = r.u30(); strings.append(r.take(l).decode('utf-8'))
    n = r.u30(); nss = [None]
    for _ in range(max(n - 1, 0)):
        k = r.u8(); nss.append((k, strings[r.u30()]))
    n = r.u30(); nssets = [None]
    for _ in range(max(n - 1, 0)):
        c = r.u30(); nssets.append([r.u30() for _ in range(c)])
    n = r.u30(); mns = [None]
    for _ in range(max(n - 1, 0)):
        k = r.u8()
        if k in (0x07, 0x0D): mns.append(('QName', r.u30(), r.u30()))
        elif k in (0x0F, 0x10): mns.append(('RTQName', r.u30()))
        elif k in (0x11, 0x12): mns.append(('RTQNameL',))
        elif k in (0x09, 0x0E): mns.append(('Multiname', r.u30(), r.u30()))
        elif k in (0x1B, 0x1C): mns.append(('MultinameL', r.u30()))
        elif k == 0x1D:
            base = r.u30(); c = r.u30(); mns.append(('TypeName', base, [r.u30() for _ in range(c)]))
        else: raise SystemExit('unknown multiname kind %x' % k)
    nmeth = r.u30()
    for _ in range(nmeth):
        pc = r.u30(); r.u30(); [r.u30() for _ in range(pc)]; r.u30(); fl = r.u8()
        if fl & 0x08:
            oc = r.u30()
            for _ in range(oc): r.u30(); r.u8()
        if fl & 0x80: [r.u30() for _ in range(pc)]
    n = r.u30()
    for _ in range(n):
        r.u30(); c = r.u30(); [(r.u30(), r.u30()) for _ in range(c)]
    ncls = r.u30()
    def traits():
        out = []
        for _ in range(r.u30()):
            name = r.u30(); kind = r.u8(); k = kind & 0x0f
            if k in (0, 6):
                r.u30(); r.u30(); vi = r.u30()
                if vi: r.u8()
                out.append((name, k, None))
            elif k in (1, 2, 3):
                r.u30(); mi = r.u30(); out.append((name, k, mi))
            elif k in (4, 5):
                r.u30(); r.u30(); out.append((name, k, None))
            else: raise SystemExit('trait kind %d' % k)
            if kind & 0x40: [r.u30() for _ in range(r.u30())]
        return out
    instances = []
    for _ in range(ncls):
        name = r.u30(); r.u30(); fl = r.u8()
        if fl & 0x08: r.u30()
        [r.u30() for _ in range(r.u30())]
        r.u30()
        instances.append((name, traits()))
    for _ in range(ncls): r.u30(); traits()
    for _ in range(r.u30()): r.u30(); traits()
    bodies = {}
    nb = r.u30()
    for _ in range(nb):
        start = r.p
        mi = r.u30(); ms = r.u30(); lc = r.u30(); isd = r.u30(); msd = r.u30()
        clen_pos = r.p; clen = r.u30(); code_pos = r.p; r.p += clen
        ne = r.u30(); exc = [(r.u30(), r.u30(), r.u30(), r.u30(), r.u30()) for _ in range(ne)]
        tr = traits()
        bodies[mi] = dict(start=start, maxstack=ms, clen_pos=clen_pos, clen=clen, code_pos=code_pos, exc=exc, end=r.p)
    assert r.p == len(b), 'trailing bytes in ABC'
    return dict(strings=strings, nss=nss, mns=mns, instances=instances, bodies=bodies)

def qname(abc, i):
    m = abc['mns'][i]
    if m and m[0] == 'QName': return abc['strings'][m[2]]
    if m and m[0] == 'Multiname': return abc['strings'][m[1]]
    return None

def find_mn(abc, name, kind='QName'):
    hits = [i for i, m in enumerate(abc['mns']) if m and m[0] == kind and abc['strings'][m[2] if kind == 'QName' else m[1]] == name]
    return hits

# ---------------------------------------------------------------- instruction walk (to locate patch points)
# operand layouts for the opcodes that appear in these methods; anything else aborts
OPS = {
    0x09: '', 0x10: 's24', 0x0c: 's24', 0x0d: 's24', 0x0e: 's24', 0x0f: 's24', 0x11: 's24', 0x12: 's24', 0x13: 's24',
    0x14: 's24', 0x15: 's24', 0x16: 's24', 0x17: 's24', 0x18: 's24', 0x19: 's24', 0x1a: 's24',
    0x1d: '', 0x1e: '', 0x20: '', 0x21: '', 0x23: '', 0x24: 'u8', 0x25: 'u30', 0x26: '', 0x27: '', 0x28: '', 0x29: '',
    0x2a: '', 0x2b: '', 0x2c: 'u30', 0x2d: 'u30', 0x2e: 'u30', 0x2f: 'u30', 0x30: '', 0x32: 'u30u30',
    0x40: 'u30', 0x41: 'u30', 0x42: 'u30', 0x46: 'u30u30', 0x47: '', 0x48: '', 0x49: 'u30', 0x4a: 'u30u30',
    0x4c: 'u30u30', 0x4e: 'u30u30', 0x4f: 'u30u30', 0x55: 'u30', 0x56: 'u30', 0x57: '', 0x58: 'u30', 0x5a: 'u30',
    0x5d: 'u30', 0x5e: 'u30', 0x60: 'u30', 0x61: 'u30', 0x62: 'u30', 0x63: 'u30', 0x64: '', 0x65: 'u30', 0x66: 'u30',
    0x68: 'u30', 0x6c: 'u30', 0x6d: 'u30', 0x70: '', 0x73: '', 0x74: '', 0x75: '', 0x76: '', 0x80: 'u30', 0x82: '',
    0x85: '', 0x87: '', 0x90: '', 0x91: '', 0x93: '', 0x95: '', 0x96: '', 0xa0: '', 0xa1: '', 0xa2: '', 0xa3: '', 0xa4: '',
    0xa5: '', 0xa6: '', 0xa7: '', 0xa8: '', 0xa9: '', 0xaa: '', 0xab: '', 0xac: '', 0xad: '', 0xae: '', 0xaf: '',
    0xb0: '', 0xb1: '', 0xb3: '', 0xb4: '', 0xc0: '', 0xc1: '', 0xc2: 'u30', 0xc3: 'u30', 0xd0: '', 0xd1: '', 0xd2: '',
    0xd3: '', 0xd4: '', 0xd5: '', 0xd6: '', 0xd7: '', 0xef: 'u8u30u8u30', 0xf0: 'u30', 0xf1: 'u30',
}
BRANCH = {0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a}

def walk(code):
    r = R(code); out = []
    while r.p < len(code):
        at = r.p; op = r.u8()
        if op == 0x1b:  # lookupswitch
            base = at; d = r.s24(); c = r.u30(); offs = [r.s24() for _ in range(c + 1)]
            out.append((at, op, ('switch', base, d, offs), r.p)); continue
        if op not in OPS: raise SystemExit('opcode %02x at %d not handled' % (op, at))
        args = []
        f = OPS[op]
        while f:
            if f.startswith('u30'): args.append(r.u30()); f = f[3:]
            elif f.startswith('s24'): args.append(r.s24()); f = f[3:]
            elif f.startswith('u8'): args.append(r.u8()); f = f[2:]
        out.append((at, op, args, r.p))
    return out

def check_safe(ins, exc, pos):
    """No branch, switch or exception range may cross or target byte offset pos."""
    for at, op, args, nxt in ins:
        if op in BRANCH:
            tgt = nxt + args[0]
            if tgt == pos or min(nxt, tgt) < pos < max(nxt, tgt) or (at < pos < nxt):
                raise SystemExit('branch at %d crosses the patch point %d' % (at, pos))
        if op == 0x1b:
            raise SystemExit('lookupswitch in patched method')
    for (f, t, tgt, _, _) in exc:
        if f < pos < t or tgt == pos:
            raise SystemExit('exception range crosses the patch point')

# ---------------------------------------------------------------- main
def main(src, dst):
    data = open(src, 'rb').read()
    sig = data[:3]
    if sig == b'CWS': body = zlib.decompress(data[8:])
    elif sig == b'FWS': body = data[8:]
    else: raise SystemExit('unsupported SWF signature %r' % sig)
    hdr = bytearray(b'FWS' + data[3:4] + b'\0\0\0\0')
    # skip RECT + frame rate + frame count
    nbits = body[0] >> 3
    p = (5 + 4 * nbits + 7) // 8 + 4
    tags = []
    while p < len(body):
        th = struct.unpack_from('<H', body, p)[0]; code = th >> 6; ln = th & 0x3f; hp = p; p += 2
        if ln == 0x3f: ln = struct.unpack_from('<I', body, p)[0]; p += 4
        tags.append([code, body[p:p + ln], (p - hp) == 6]); p += ln
    done = False
    for t in tags:
        if t[0] not in (72, 82): continue
        tb = t[1]
        if t[0] == 82:
            fl = tb[:4]; q = 4
            while tb[q] != 0: q += 1
            pre = tb[:q + 1]; abcb = tb[q + 1:]
        else:
            pre = b''; abcb = tb
        abc = parse_abc(abcb)
        game = [ins for ins in abc['instances'] if qname(abc, ins[0]) == 'Game']
        if not game: continue
        traits = {qname(abc, n): mi for (n, k, mi) in game[0][1] if mi is not None}
        mn_shipik = [i for i in find_mn(abc, 'Shipik') if abc['nss'][abc['mns'][i][1]] == (0x16, '')]
        mn_env = find_mn(abc, 'envLine')
        if not mn_shipik or not mn_env: raise SystemExit('Shipik.envLine not found: compile docs/mathspikes2/Shipik.as in first')
        shipik, env = mn_shipik[0], mn_env[0]
        b = bytearray(abcb)
        patches = []  # (body, insert_pos_in_code, bytes)
        def body_of(name):
            return abc['bodies'][traits[name]]
        def code_of(bd): return bytes(b[bd['code_pos']:bd['code_pos'] + bd['clen']])
        def push_mn(ins):  # the 'push' multiname this method already uses
            for at, op, args, nxt in ins:
                if op == 0x4f and qname(abc, args[0]) == 'push': return args[0]
            raise SystemExit('no push call to copy')
        # dbgLevelCalib: after getlocal0; getproperty m_currLevel; coerce_a; setlocal1
        bd = body_of('dbgLevelCalib'); ins = walk(code_of(bd))
        pos = None
        for i in range(len(ins) - 3):
            a, b1, c, d = ins[i:i + 4]
            if a[1] == 0xd0 and b1[1] == 0x66 and qname(abc, b1[2][0]) == 'm_currLevel' and c[1] == 0x82 and d[1] == 0xd5:
                pos = d[3]; mn_curr = b1[2][0]; break
        if pos is None: raise SystemExit('dbgLevelCalib: L = m_currLevel not found')
        check_safe(ins, bd['exc'], pos)
        outloc = 6  # var out:Array (local 6 in this method; checked: debug info)
        seq = bytes([0x62]) + enc_u30(outloc) + bytes([0x60]) + enc_u30(shipik) + bytes([0xd1]) + \
              bytes([0x46]) + enc_u30(env) + enc_u30(1) + bytes([0x4f]) + enc_u30(push_mn(ins)) + enc_u30(1)
        patches.append((bd, pos, seq, 3))
        # dbgSpikeCalib(out:Array): at entry (after getlocal0; pushscope) -> out.push(envLine(this.m_currLevel))
        bd = body_of('dbgSpikeCalib'); ins = walk(code_of(bd))
        if not (ins[0][1] == 0xd0 and ins[1][1] == 0x30): raise SystemExit('dbgSpikeCalib: unexpected entry')
        pos = ins[1][3]
        check_safe(ins, bd['exc'], pos)
        seq = bytes([0xd1, 0x60]) + enc_u30(shipik) + bytes([0xd0, 0x66]) + enc_u30(mn_curr) + \
              bytes([0x46]) + enc_u30(env) + enc_u30(1) + bytes([0x4f]) + enc_u30(push_mn(ins)) + enc_u30(1)
        patches.append((bd, pos, seq, 3))
        # dbgExport: after `hdr = ...` (first setlocal1) -> hdr = envLine(m_currLevel) + "\n" + hdr
        bd = body_of('dbgExport'); ins = walk(code_of(bd))
        pos = None
        for at, op, args, nxt in ins:
            if op == 0xd5: pos = nxt; break
        if pos is None: raise SystemExit('dbgExport: hdr store not found')
        check_safe(ins, bd['exc'], pos)
        nl = [i for i, s in enumerate(abc['strings']) if s == '\n']
        if not nl: raise SystemExit('no "\\n" string in the pool')
        seq = bytes([0x60]) + enc_u30(shipik) + bytes([0xd0, 0x66]) + enc_u30(mn_curr) + bytes([0x46]) + enc_u30(env) + enc_u30(1) + \
              bytes([0x2c]) + enc_u30(nl[0]) + bytes([0xa0, 0xd1, 0xa0, 0xd5])
        patches.append((bd, pos, seq, 3))
        # apply from the back of the ABC so earlier offsets stay valid
        for bd, pos, seq, need in sorted(patches, key=lambda x: -x[0]['start']):
            cp = bd['code_pos']; clen = bd['clen']
            code = bytes(b[cp:cp + clen]); newcode = code[:pos] + seq + code[pos:]
            # exception table entries after pos shift
            exc_start = cp + clen
            r = R(b, exc_start); ne = r.u30()
            exc = bytearray(enc_u30(ne))
            for (f, to, tg, ty, vn) in bd['exc']:
                sh = lambda v: v + len(seq) if v >= pos else v
                exc += enc_u30(sh(f)) + enc_u30(sh(to)) + enc_u30(sh(tg)) + enc_u30(ty) + enc_u30(vn)
            for _ in range(ne): [r.u30() for _ in range(5)]
            # header: method, max_stack (raise if needed), locals, scope depths
            hr = R(b, bd['start']); mi = hr.u30(); ms = hr.u30(); lc = hr.u30(); isd = hr.u30(); msd = hr.u30()
            head = enc_u30(mi) + enc_u30(max(ms, ms + need)) + enc_u30(lc) + enc_u30(isd) + enc_u30(msd)
            new = head + enc_u30(len(newcode)) + newcode + bytes(exc) + bytes(b[r.p:bd['end']])
            b[bd['start']:bd['end']] = new
        t[1] = bytes(pre) + bytes(b)
        done = True
        break
    if not done: raise SystemExit('Game not found')
    out = bytearray(body[:(5 + 4 * nbits + 7) // 8 + 4])
    for code, tb, longhdr in tags:
        if len(tb) < 0x3f and not longhdr:
            out += struct.pack('<H', code << 6 | len(tb))
        else:
            out += struct.pack('<HI', code << 6 | 0x3f, len(tb))
        out += tb
    total = 8 + len(out)
    hdr[4:8] = struct.pack('<I', total)
    res = (b'CWS' + bytes(hdr[3:4]) + struct.pack('<I', total) + zlib.compress(bytes(out), 9)) if sig == b'CWS' else bytes(hdr) + bytes(out)
    open(dst, 'wb').write(res)
    print('patched dbgLevelCalib, dbgSpikeCalib, dbgExport ->', dst)

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
