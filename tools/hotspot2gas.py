#!/usr/bin/env python3
"""hotspot2gas.py - translate OpenJDK's Intel LIBM sin/cos stubs
(stubGenerator_x86_64_{sin,cos}.cpp + stubGenerator_x86_64_constants.cpp,
MacroAssembler DSL) into a GNU-assembler (.intel_syntax) source file that
exports `double rb_libm_sin(double)` and `double rb_libm_cos(double)`.

Each MacroAssembler call maps 1:1 to one x86-64 instruction, so the emitted
machine code performs exactly the same operations as Intel's routines.
"""
import re
import sys

REG64 = ["rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rsp", "rbp"] + [f"r{i}" for i in range(8, 16)]
REG32 = {"rax": "eax", "rbx": "ebx", "rcx": "ecx", "rdx": "edx", "rsi": "esi", "rdi": "edi",
         "rsp": "esp", "rbp": "ebp", **{f"r{i}": f"r{i}d" for i in range(8, 16)}}
JCC = {"above": "ja", "equal": "je", "greater": "jg", "less": "jl", "lessEqual": "jle",
       "notEqual": "jne", "below": "jb", "aboveEqual": "jae", "belowEqual": "jbe",
       "greaterEqual": "jge", "zero": "jz", "notZero": "jnz"}

# operand width of a memory operand, by mnemonic
MEMSIZE = {}
for m in "movl addl subl andl orl xorl cmpl testl".split():
    MEMSIZE[m] = "dword"
for m in "movq movsd mulsd addsd subsd movddup cvtsi2sdq addq subq".split():
    MEMSIZE[m] = "qword"
for m in "movdqu mulpd addpd subpd xorpd pand por unpcklpd unpckhpd movapd".split():
    MEMSIZE[m] = "xmmword"

# HotSpot mnemonic -> GAS mnemonic, and whether GPR operands are 32-bit
OPMAP = {
    "movl": ("mov", 32), "addl": ("add", 32), "subl": ("sub", 32), "andl": ("and", 32),
    "orl": ("or", 32), "xorl": ("xor", 32), "cmpl": ("cmp", 32), "testl": ("test", 32),
    "negl": ("neg", 32), "shll": ("shl", 32), "shrl": ("shr", 32), "sarl": ("sar", 32),
    "addq": ("add", 64), "subq": ("sub", 64), "andq": ("and", 64), "orq": ("or", 64),
    "cmpq": ("cmp", 64), "shlq": ("shl", 64), "shrq": ("shr", 64), "sbbq": ("sbb", 64),
    "imulq": ("imul", 64), "bsrq": ("bsr", 64), "mov64": ("movabs", 64),
    "movq": ("movq", 64), "movdq": ("movq", 64), "movsd": ("movsd", 64),
    "cvtsi2sdl": ("cvtsi2sd", 32), "cvtsi2sdq": ("cvtsi2sd", 64),
    "cvttsd2sil": ("cvttsd2si", 32), "cvttsd2siq": ("cvttsd2si", 64),
    "pextrw": ("pextrw", 32), "pinsrw": ("pinsrw", 32),
    "push_ppx": ("push", 64), "pop_ppx": ("pop", 64), "lea": ("lea", 64),
}
PASS = "addsd subsd mulsd addpd subpd mulpd xorpd pand por unpcklpd unpckhpd movdqu movddup " \
       "pshufd movlhps movapd psrlq psllq".split()


def split_args(s):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def translate(path, prefix):
    src = open(path).read()
    body_start = src.index("start = __ pc();")
    body_end = src.index("__ ret(0);", body_start)
    body = src[body_start:body_end + len("__ ret(0);")]
    body = re.sub(r"#ifdef _WIN64.*?#endif", "", body, flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    out = []
    for m in re.finditer(r"__\s+([a-z_0-9]+)\((.*?)\);", body, flags=re.S):
        op, args = m.group(1), split_args(m.group(2).replace("\n", " "))
        if op == "pc":
            continue
        if op == "bind":
            out.append(f".L{prefix}_{args[0]}:")
            continue
        if op == "enter":
            out += ["    push rbp", "    mov rbp, rsp"]
            continue
        if op == "leave":
            out.append("    leave")
            continue
        if op == "ret":
            out.append("    ret")
            continue
        if op == "jmp":
            out.append(f"    jmp .L{prefix}_{args[0]}")
            continue
        if op == "jcc":
            cond = args[0].split("::")[1]
            out.append(f"    {JCC[cond]} .L{prefix}_{args[1]}")
            continue
        has_ext = any(a.startswith("ExternalAddress") for a in args)
        if has_ext and len(args) == 3 and args[2] in REG64:
            args = args[:2]  # drop rscratch register
        if op in OPMAP:
            mnem, width = OPMAP[op]
        elif op in PASS:
            mnem, width = op, 64
        else:
            raise SystemExit(f"unhandled op {op} in {path}")

        def conv(a):
            a = a.strip()
            if a.startswith("ExternalAddress("):
                inner = a[len("ExternalAddress("):-1].strip()
                parts = [p.strip() for p in inner.split("+")]
                sym = f"{prefix}_{parts[0]}" if parts[0] == "ALL_ONES" else f"libm_{parts[0]}"
                disp = "".join(f" + {p}" for p in parts[1:])
                size = "" if mnem == "lea" else f"{MEMSIZE.get(op, 'qword')} ptr "
                return f"{size}[rip + {sym}{disp}]"
            if a.startswith("Address("):
                inner = split_args(a[len("Address("):-1])
                base = inner[0]
                disp = inner[1] if len(inner) > 1 else "0"
                size = "" if mnem == "lea" else f"{MEMSIZE.get(op, 'qword')} ptr "
                return f"{size}[{base} + {disp}]"
            if a in REG64:
                return REG32[a] if width == 32 else a
            if a == "INT_MIN":
                return "0x80000000"
            return a  # xmm register or immediate

        ops = [conv(a) for a in args]
        if mnem in ("shl", "shr", "sar") and len(ops) == 1:
            ops.append("cl")  # HotSpot one-operand shifts shift by CL
        out.append(f"    {mnem} " + ", ".join(ops))
    return out


def tables(path, only=None):
    src = open(path).read()
    res = []
    for m in re.finditer(r"ATTRIBUTE_ALIGNED\((\d+)\)\s+static const juint _([A-Za-z0-9_]+)\[\]\s*=\s*\{(.*?)\};", src, flags=re.S):
        align, name, vals = m.group(1), m.group(2), m.group(3)
        if only is not None and name not in only:
            continue
        nums = re.findall(r"0x[0-9a-fA-F]+", re.sub(r"//[^\n]*", "", vals))
        res.append((int(align), name, nums))
    return res


def main():
    d = sys.argv[1]
    out = ["# Generated by tools/hotspot2gas.py from OpenJDK's",
           "# src/hotspot/cpu/x86/stubGenerator_x86_64_{sin,cos,constants}.cpp",
           "#",
           "# Copyright (c) 2016, 2025, Intel Corporation. All rights reserved.",
           "# Intel Math Library (LIBM) Source Code",
           "#",
           "# This code is free software; you can redistribute it and/or modify it",
           "# under the terms of the GNU General Public License version 2 only, as",
           "# published by the Free Software Foundation.",
           "#",
           "# This code is distributed in the hope that it will be useful, but WITHOUT",
           "# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or",
           "# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License",
           "# version 2 for more details.",
           ".intel_syntax noprefix", "#if defined(_WIN32)", '.section .rdata,"dr"', "#else", ".section .rodata", "#endif"]
    for align, name, nums in tables(f"{d}/stubGenerator_x86_64_constants.cpp"):
        out.append(f".balign {max(align, 16)}")
        out.append(f"libm_{name}:")
        for i in range(0, len(nums), 4):
            out.append("    .long " + ", ".join(nums[i:i + 4]))
    for fn, prefix in (("sin", "sin"), ("cos", "cos")):
        for align, name, nums in tables(f"{d}/stubGenerator_x86_64_{fn}.cpp"):
            out.append(f".balign {max(align, 16)}")
            out.append(f"{prefix}_{name}:")
            for i in range(0, len(nums), 4):
                out.append("    .long " + ", ".join(nums[i:i + 4]))
    out.append(".text")
    for fn in ("sin", "cos"):
        U = fn.upper()
        out += ["#if defined(_WIN32)", f"#define RB_{U} rb_libm_{fn}_body", "#else", f"#define RB_{U} rb_libm_{fn}",
                f".globl rb_libm_{fn}", f".type rb_libm_{fn}, @function", "#endif", ".balign 16", f"RB_{U}:"]
        out += translate(f"{d}/stubGenerator_x86_64_{fn}.cpp", fn)
        out += ["#if !defined(_WIN32)", f".size rb_libm_{fn}, .-rb_libm_{fn}", "#endif"]
    out.append(WIN64_TAIL)
    print("\n".join(out))


WIN64_TAIL = '#if defined(_WIN32)\n# Windows x64: the bodies above follow the System V convention, which lets a function overwrite\n# rsi, rdi, xmm6 and xmm7. Windows requires them to be preserved, so the exported entry points\n# save and restore them around the bodies (argument and result stay in xmm0 on both ABIs).\n# Stack: entry rsp = 16k + 8; two pushes and sub 40 give a 16-byte aligned rsp at the call.\n.globl rb_libm_sin\n.def rb_libm_sin; .scl 2; .type 32; .endef\n.balign 16\nrb_libm_sin:\n    push rsi\n    push rdi\n    sub rsp, 40\n    movdqu xmmword ptr [rsp], xmm6\n    movdqu xmmword ptr [rsp + 16], xmm7\n    call rb_libm_sin_body\n    movdqu xmm6, xmmword ptr [rsp]\n    movdqu xmm7, xmmword ptr [rsp + 16]\n    add rsp, 40\n    pop rdi\n    pop rsi\n    ret\n.globl rb_libm_cos\n.def rb_libm_cos; .scl 2; .type 32; .endef\n.balign 16\nrb_libm_cos:\n    push rsi\n    push rdi\n    sub rsp, 40\n    movdqu xmmword ptr [rsp], xmm6\n    movdqu xmmword ptr [rsp + 16], xmm7\n    call rb_libm_cos_body\n    movdqu xmm6, xmmword ptr [rsp]\n    movdqu xmm7, xmmword ptr [rsp + 16]\n    add rsp, 40\n    pop rdi\n    pop rsi\n    ret\n#else\n.section .note.GNU-stack,"",@progbits\n#endif'


if __name__ == "__main__":
    main()
