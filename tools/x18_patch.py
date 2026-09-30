#!/usr/bin/env python3
"""
Generate load-time patches that remove x18 usage from an aarch64 Linux ELF.

On Apple Silicon, x18 is platform-reserved: the kernel zeroes it on syscalls
and on some preemptions. GCC on Linux uses x18 as an ordinary scratch register
(only under heavy register pressure, so there is never a free GPR to rename
it to). Unpatched, such code would see rare, silent corruption.

Strategy: keep x18's value in an unused caller-saved SIMD register V. Every
instruction I that references x18 is replaced in place by `b stub`, and the
loader emits a stub:

    fmov dT, xS      // park a scratch GPR S (not referenced by I) in SIMD reg T
    fmov xS, dV      // S := "x18"
    I'               // I with x18/w18 -> xS/wS
    fmov dV, xS      // "x18" := S  (harmless if I didn't write it)
    fmov xS, dT      // restore S
    b    next

fmov touches neither memory nor flags, and SIMD state survives context
switches. Function bodies stay at their original addresses, so .eh_frame,
LSDAs and jump tables remain valid. cbz/cbnz/tbz/tbnz on x18 get a two-exit
stub (see loader).

V and T must be caller-saved (v0-v7, v16-v31), unused in the function, and
unused by any local callee that GCC's IPA-RA may have relied on (callees whose
transitive closure makes no external/indirect call).

Each I' is verified by disassembling it and comparing to the original text with
x18/w18 replaced. Output is a C header consumed by src/elf_loader.c.
"""
import hashlib
import os
import re
import struct
import subprocess
import sys
import tempfile

OBJDUMP = "/opt/homebrew/opt/binutils/bin/gobjdump"

FUNC_RE = re.compile(r"^([0-9a-f]+) <(.+)>:$")
INSN_RE = re.compile(r"^\s*([0-9a-f]+):\t([0-9a-f]{8}) \t(.*)$")
GPR_RE = re.compile(r"(?<![0-9a-zA-Z_.])[xw]([0-9]|[12][0-9]|30)(?![0-9a-zA-Z_])")
VEC_RE = re.compile(r"(?<![0-9a-zA-Z_.])[vqdsbh]([0-9]|[12][0-9]|3[01])(?![0-9a-zA-Z_])")
X18_RE = re.compile(r"(?<![0-9a-zA-Z_.])[xw]18(?![0-9a-zA-Z_])")
CALL_RE = re.compile(r"^bl\t([0-9a-f]+) <(.+)>")
CALLER_SAVED_VEC = list(range(16, 32)) + list(range(0, 8))
SCRATCH_GPRS = list(range(9, 16)) + list(range(0, 9)) + [16, 17]

KIND_PLAIN, KIND_CBZ, KIND_TBZ, KIND_ADDR = 0, 1, 2, 3


def disasm_lib(path):
    raw = subprocess.run([OBJDUMP, "-d", path], capture_output=True, text=True, check=True).stdout
    funcs, order, cur = {}, [], None
    for line in raw.splitlines():
        m = FUNC_RE.match(line)
        if m:
            cur = (int(m.group(1), 16), m.group(2))
            funcs[cur] = []
            order.append(cur)
            continue
        m = INSN_RE.match(line)
        if m and cur is not None:
            text = m.group(3).split("\t//")[0].strip()
            funcs[cur].append((int(m.group(1), 16), int(m.group(2), 16), text))
    return funcs, order


def regs(insns, rx):
    s = set()
    for _, _, t in insns:
        s.update(int(n) for n in rx.findall(t))
    return s


def disasm_words(words):
    if not words:
        return []
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        for w in words:
            f.write(struct.pack("<I", w))
        name = f.name
    try:
        out = subprocess.run([OBJDUMP, "-D", "-b", "binary", "-m", "aarch64", "-EL", name],
                             capture_output=True, text=True, check=True).stdout
    finally:
        os.unlink(name)
    res = {}
    for line in out.splitlines():
        m = INSN_RE.match(line)
        if m:
            res[int(m.group(1), 16) // 4] = m.group(3).split("\t//")[0].strip()
    return [res.get(i, "?") for i in range(len(words))]


def classify(word, text):
    mnem = text.split("\t")[0]
    if mnem in ("cbz", "cbnz"):
        return KIND_CBZ
    if mnem in ("tbz", "tbnz"):
        return KIND_TBZ
    if mnem in ("adr", "adrp"):
        return KIND_ADDR
    if mnem.startswith("ldr") and (word >> 24) & 0x3B == 0x18:
        raise ValueError("pc-relative literal load into x18 is not supported")
    if mnem in ("br", "blr", "ret", "b", "bl") or mnem.startswith("b."):
        raise ValueError("control transfer through x18 is not supported")
    return KIND_PLAIN


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: x18_patch.py <elf> <c-ident> <out.h>")
    path, ident, outpath = sys.argv[1:]
    sha = hashlib.sha256(open(path, "rb").read()).hexdigest()
    funcs, order = disasm_lib(path)
    by_addr = {a: (a, n) for (a, n) in order}

    info = {}
    for key in order:
        callees, external = set(), False
        for _, _, t in funcs[key]:
            m = CALL_RE.match(t)
            if m:
                tgt = int(m.group(1), 16)
                if "@plt" in m.group(2) or tgt not in by_addr:
                    external = True
                else:
                    callees.add(by_addr[tgt])
            elif t.startswith(("blr", "svc")):
                external = True
        info[key] = (callees, external, regs(funcs[key], VEC_RE))

    def closure(key, memo={}):
        if key not in memo:
            seen, stack, vec, ext = set(), [key], set(), False
            while stack:
                k = stack.pop()
                if k in seen or k not in info:
                    continue
                seen.add(k)
                c, e, v = info[k]
                vec |= v
                ext |= e
                stack.extend(c)
            memo[key] = (vec, ext)
        return memo[key]

    pending = []  # addr, word, text, kind, S, V, T, fn
    for key in order:
        insns = funcs[key]
        if not any(X18_RE.search(t) for _, _, t in insns):
            continue
        callees, _, vused = info[key]
        forbidden = set(vused)
        for c in callees:
            cvec, cext = closure(c)
            if not cext:
                forbidden |= cvec
        free = [r for r in CALLER_SAVED_VEC if r not in forbidden]
        if len(free) < 2:
            sys.exit(f"ERROR: fewer than two free SIMD registers in {key[1]}")
        V, T = free[0], free[1]
        for addr, word, text in insns:
            if not X18_RE.search(text):
                continue
            try:
                kind = classify(word, text)
            except ValueError as e:
                sys.exit(f"ERROR: {addr:#x} '{text}' in {key[1]}: {e}")
            in_insn = {int(n) for n in GPR_RE.findall(text)}
            S = next(r for r in SCRATCH_GPRS if r not in in_insn)
            pending.append((addr, word, text, kind, S, V, T, key[1]))

    # Substitute S for 18 in every subset of register fields holding 18 and keep
    # the candidate whose disassembly equals the original with x18 -> xS.
    fields = (0, 5, 10, 16)
    words, meta = [], []
    for i, (addr, word, text, kind, S, V, T, fn) in enumerate(pending):
        if kind == KIND_ADDR:
            continue
        hits = [f for f in fields if (word >> f) & 31 == 18]
        if kind != KIND_PLAIN:
            hits = [0]  # cbz/tbz: only Rt; retarget to +12 (the "taken" exit)
        for mask in range(1, 1 << len(hits)):
            nw = word
            for j, f in enumerate(hits):
                if mask & (1 << j):
                    nw = (nw & ~(31 << f)) | (S << f)
            if kind == KIND_CBZ:
                nw = (nw & ~(0x7FFFF << 5)) | (3 << 5)
            elif kind == KIND_TBZ:
                nw = (nw & ~(0x3FFF << 5)) | (3 << 5)
            words.append(nw)
            meta.append(i)
    texts = disasm_words(words)
    chosen = {}
    for i, nw, t in zip(meta, words, texts):
        if i in chosen:
            continue
        addr, word, text, kind, S, V, T, fn = pending[i]
        want = X18_RE.sub(lambda m: m.group(0)[0] + str(S), text)
        if kind != KIND_PLAIN:
            # Branch target becomes stub-relative +12; compare everything else.
            want = re.sub(r",\s*[0-9a-f]+ <[^>]*>$", "", want)
            t = re.sub(r",\s*0x[0-9a-f]+$", "", t)
        if t == want:
            chosen[i] = nw
    for i, p in enumerate(pending):
        if p[3] == KIND_ADDR:
            if p[1] & 31 != 18:
                sys.exit(f"ERROR: unexpected {p[2]}")
            chosen[i] = p[1]
        if i not in chosen:
            sys.exit(f"ERROR: cannot rewrite {p[0]:#x} '{p[2]}' in {p[7]}")

    def branch_target(word, kind):
        if kind == KIND_ADDR:
            imm = ((word >> 5) & 0x7FFFF) << 2 | ((word >> 29) & 3)
            imm = imm - (1 << 21) if imm & (1 << 20) else imm
            return imm
        if kind == KIND_CBZ:
            imm = (word >> 5) & 0x7FFFF
            return (imm - (1 << 19) if imm & (1 << 18) else imm) * 4
        imm = (word >> 5) & 0x3FFF
        return (imm - (1 << 14) if imm & (1 << 13) else imm) * 4

    nfuncs = len({p[7] for p in pending})
    with open(outpath, "w") as f:
        f.write(f"/* Generated by tools/x18_patch.py from {os.path.basename(path)} -- do not edit. */\n")
        f.write(f"/* {len(pending)} x18 instructions in {nfuncs} functions redirected to SIMD-backed stubs. */\n")
        f.write(f"static const char {ident}_sha256[] = \"{sha}\";\n")
        f.write(f"static const struct x18_patch {ident}_x18[] = {{\n")
        for i, (addr, word, text, kind, S, V, T, fn) in enumerate(pending):
            if kind == KIND_ADDR and word & 0x80000000:  # adrp: page-relative
                tgt = (addr & ~0xFFF) + (branch_target(word, kind) << 12)
            elif kind != KIND_PLAIN:
                tgt = addr + branch_target(word, kind)
            else:
                tgt = 0
            f.write(f"    {{0x{addr:x}, 0x{word:08x}, 0x{chosen[i]:08x}, 0x{tgt:x}, {kind}, {S}, {V}, {T}}},"
                    f" /* {text.replace('*/', '')} ; {fn[:48]} */\n")
        f.write("};\n")
    print(f"{os.path.basename(path)}: {len(pending)} x18 instructions in {nfuncs} functions")


if __name__ == "__main__":
    main()
