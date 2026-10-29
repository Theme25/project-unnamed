#!/usr/bin/env python3
"""pe2gas.py - re-assemble x86-64 math routines of a Windows (PE32+) Flash Player as GNU assembler.

usage: pe2gas.py <player.exe|.ocx> <prefix> [name=0xADDR | name=0xADDR:export] ... [stub=KIND@0xADDR] ...
                 [flag=SYMBOL@0xADDR] > out.S

Every reachable instruction of the listed functions is emitted unchanged (.intel_syntax), so the routines
compute exactly what the player computes. Changes made:
  - jump and call targets become local labels; calls to listed functions stay calls;
  - RIP-relative data becomes a reference into a copy of the bytes it reads (".rdata" chunks);
  - `stub=nan@ADDR`: a call to the CRT's error reporter becomes inline NaN/Inf handling (the reporter only
    sets errno and returns NaN; the sim never passes NaN or infinity);
  - `stub=ucrtnan@ADDR`: UCRT's reporter: quiets a NaN, turns an infinity into the default NaN, otherwise
    returns its argument unchanged (it never alters a finite value);
  - `flag=SYM@ADDR`: a 32-bit global the routines read (e.g. the UCRT's "use FMA3" switch) becomes the
    writable global SYM.
Exported functions keep the Win64 calling convention internally (double in xmm0, 32-byte home area); on
non-Windows hosts an exported `PREFIX_NAME` wrapper reserves the home area so it can be called as a plain
C function `double f(double)`.
"""
import sys, pefile, capstone
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP

pe = pefile.PE(sys.argv[1], fast_load=True)
IB = pe.OPTIONAL_HEADER.ImageBase
SIZE = pe.OPTIONAL_HEADER.SizeOfImage
prefix = sys.argv[2]
funcs, exports, stubs, flags = {}, set(), {}, {}
for a in sys.argv[3:]:
    k, v = a.split('=')
    if k == 'stub':
        kind, addr = v.split('@'); stubs[int(addr, 16)] = kind
    elif k == 'flag':
        sym, addr = v.split('@'); flags[int(addr, 16)] = sym
    else:
        addr, _, ex = v.partition(':')
        funcs[int(addr, 16)] = k
        if ex == 'export': exports.add(k)

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True
LEA_PAD = 1024
refs = {}   # data address -> min size


def collect(addr):
    seen, work, labels = {}, [addr], set()
    while work:
        a = work.pop()
        while a not in seen:
            ins = next(md.disasm(pe.get_data(a - IB, 16), a))
            seen[a] = ins
            m = ins.mnemonic.replace('bnd ', '').replace('repz ', '')
            if m.startswith('j'):
                op = ins.operands[0]
                if op.type != X86_OP_IMM: sys.exit('indirect jump at %#x' % a)
                if not (op.imm in funcs and op.imm != addr):
                    work.append(op.imm); labels.add(op.imm)
                if m == 'jmp': break
            if m in ('ret', 'int3', 'ud2'): break
            a += ins.size
    return seen, labels


def L(a): return '.L%s_%x' % (prefix, a)


out_text = []
for faddr, name in funcs.items():
    seen, labels = collect(faddr)
    out_text.append('.balign 16')
    out_text.append('%s_%s_body:' % (prefix, name))
    order = sorted(seen)
    for k, a in enumerate(order):
        ins = seen[a]
        if a in labels: out_text.append('%s:' % L(a))
        m = ins.mnemonic.replace('bnd ', '').replace('repz ', '')
        ops = ins.op_str
        if m == 'nop': continue
        if m.startswith('j') or m == 'call':
            t = ins.operands[0].imm
            if m == 'call' and t in stubs:
                kind = stubs[t]
                if kind == 'nan':      # AMD/VS2008: only reached for NaN/Inf: result x - x (NaN)
                    out_text.append('    subsd xmm0, xmm0  # was: call %#x (NaN/Inf reporter)' % t)
                elif kind == 'ucrtnan':
                    out_text.append('    call %s_ucrt_nan  # was: call %#x (UCRT error reporter)' % (prefix, t))
                else: sys.exit('stub kind')
            elif t in funcs and t != faddr:
                out_text.append('    %s %s_%s_body' % (m, prefix, funcs[t]))
            elif m == 'call':
                sys.exit('call to unlisted %#x from %#x' % (t, a))
            else:
                out_text.append('    %s %s' % (m, L(t)))
        else:
            for op in ins.operands:
                if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                    tgt = a + ins.size + op.mem.disp
                    if tgt in flags:
                        rep = flags[tgt]
                    else:
                        sz = LEA_PAD if m == 'lea' else max(op.size, 16)
                        refs[tgt] = max(refs.get(tgt, 0), sz)
                        rep = '%s_d_%x' % (prefix, tgt)
                    import re
                    ops = re.sub(r'\[rip [+-] 0x[0-9a-f]+\]', '[rip + %s]' % rep, ops)
            if m == 'movabs': m = 'movabs'
            out_text.append('    %s %s' % (m, ops))
        nxt = a + ins.size
        if m not in ('jmp', 'ret', 'int3', 'ud2') and (k + 1 >= len(order) or order[k + 1] != nxt):
            out_text.append('    jmp %s' % L(nxt))
            labels.add(nxt)
    # labels referenced as fallthrough targets that were emitted earlier are fine; check missing ones
    emitted = {int(x[len('.L%s_' % prefix):-1], 16) for x in out_text if x.startswith('.L%s_' % prefix) and x.endswith(':')}
    for t in labels:
        if t not in emitted and t in seen: sys.exit('label bookkeeping error %#x' % t)

# merge data chunks
items = sorted(refs.items())
chunks = []
for a, sz in items:
    if chunks and a <= chunks[-1][1] + 64: chunks[-1][1] = max(chunks[-1][1], a + sz)
    else: chunks.append([a, a + sz])

print('# Generated by tools/pe2gas.py from %s (image base %#x) -- do not edit.' % (__import__('os').path.basename(sys.argv[1]), IB))
print('# The routines below are the player\'s own machine code, re-assembled; see the script for what changed.')
print('.intel_syntax noprefix')
print('#if defined(_WIN32)\n.section .rdata,"dr"\n#else\n.section .rodata\n#endif')
for lo, hi in chunks:
    d = pe.get_data(lo - IB, hi - lo).ljust(hi - lo, b'\0')
    print('.balign 16')
    row = []
    def flush():
        if row: print('    .byte ' + ','.join('0x%02x' % b for b in row)); row.clear()
    for a in range(lo, hi):
        if a in refs:
            flush(); print('%s_d_%x:  # %#x' % (prefix, a, a))
        row.append(d[a - lo])
        if len(row) == 16: flush()
    flush()
if flags:
    print('.data')
    for sym in sorted(set(flags.values())):
        print('.balign 4\n.globl %s\n%s:\n    .long 0' % (sym, sym))
print('.text')
if any(k == 'ucrtnan' for k in stubs.values()):
    print("""# UCRT error reporter, value part only: NaN -> quiet NaN, +-Inf -> default NaN, finite -> unchanged.
.balign 16
%s_ucrt_nan:
    movq rax, xmm0
    mov rcx, rax
    movabs rdx, 0x7ff0000000000000
    and rcx, rdx
    cmp rcx, rdx
    jne 1f
    movabs rcx, 0xfffffffffffff
    test rax, rcx
    jz 2f
    movabs rcx, 0x8000000000000
    or rax, rcx
    movq xmm0, rax
    ret
2:  movabs rax, 0xfff8000000000000
    movq xmm0, rax
1:  ret""" % prefix)
print('\n'.join(out_text))
for name in sorted(exports):
    sym = '%s_%s' % (prefix, name)
    print("""
.globl %(s)s
#if defined(_WIN32)
.def %(s)s; .scl 2; .type 32; .endef
.balign 16
%(s)s:
    jmp %(s)s_body
#else
.type %(s)s, @function
.balign 16
%(s)s:
    sub rsp, 40
    call %(s)s_body
    add rsp, 40
    ret
#endif""" % {'s': sym})
print('#if !defined(_WIN32)\n.section .note.GNU-stack,"",@progbits\n#endif')
