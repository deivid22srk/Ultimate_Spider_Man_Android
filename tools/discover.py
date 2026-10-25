"""Function discovery for LEGOBatman.exe (x86-32 PE).

Recursive descent from the entry point, direct call targets, and code
pointers found in data sections (vtables, callback tables). Jump tables of
the MSVC form `jmp [reg*4+table]` are bounded by the preceding `cmp reg, N`.

Output: analysis/functions.json
  { "image_base", "functions": {addr: {"blocks": {addr: {"end", "succ"}}, ...}},
    "jump_tables": {insn_addr: [targets]}, "indirect_jumps": [...] }
"""
import json
import os
import struct
import sys
from array import array
from collections import deque

import capstone
import pefile
from capstone import x86

import peimg

EXE = sys.argv[1]
OUT = sys.argv[2]
NEW_BASE = int(sys.argv[3], 16) if len(sys.argv) > 3 else None

pe, BASE, img = peimg.load(EXE, NEW_BASE)
EXPORTS = peimg.exports(pe, BASE)
text = next(s for s in pe.sections if s.Name.startswith(b".text"))
# Every executable section counts as code (binkw32 splits its codecs into
# BINK*, sections); TEXT_LO/HI span them all for the linear scans.
CODE = [(BASE + s.VirtualAddress, BASE + s.VirtualAddress + s.Misc_VirtualSize)
        for s in pe.sections if s.Characteristics & 0x20000000]
TEXT_LO = min(lo for lo, _ in CODE)
TEXT_HI = max(hi for _, hi in CODE)

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.detail = True

_cache = {}


def decode(addr):
    i = _cache.get(addr)
    if i is None:
        off = addr - BASE
        try:
            i = next(md.disasm(bytes(img[off:off + 16]), addr, 1))
        except StopIteration:
            i = False
        # Bounded: capstone detail objects are ~4 KB each, and caching every
        # instruction of a 12 MB .text runs out of memory.
        if len(_cache) > int(os.environ.get("DISCOVER_CACHE", "60000")):
            _cache.clear()
        _cache[addr] = i
    return i


def in_text(a):
    for lo, hi in CODE:
        if lo <= a < hi:
            return True
    return False


def u32(a):
    return struct.unpack_from("<I", img, a - BASE)[0]


# Imported function slots (IAT): calls through them are external.
iat = {}
for imp in pe.DIRECTORY_ENTRY_IMPORT:
    for e in imp.imports:
        name = e.name.decode() if e.name else "#%d" % e.ordinal
        iat[e.address] = imp.dll.decode() + "!" + name

jump_tables = {}
NORETURN = {"_CxxThrowException", "abort", "exit", "_exit", "_amsg_exit", "ExitProcess", "_invoke_watson"}


# Likely function starts (targets of `call rel32` that follow padding or are
# 16-aligned). Only used for DLLs: an unconditional jmp there is a tail call.
# Treating one as a call is always safe (the lifter emits f_X(c); return).
TAIL_STARTS = set()
# Privileged or long-obsolete instructions: decoding one means the "function"
# is data (strings and tables live in .text in some DLLs).
JUNK = {"in", "out", "insb", "insd", "insw", "outsb", "outsd", "outsw", "hlt", "iretd", "arpl", "cli",
        "sti", "bound", "into", "lcall", "ljmp", "salc", "sldt"}


def scan_tail_starts():
    for lo, hi in CODE:
        for a in range(lo, hi - 5):
            if img[a - BASE] != 0xE8:
                continue
            t = (a + 5 + struct.unpack_from("<i", img, a + 1 - BASE)[0]) & 0xFFFFFFFF
            if not in_text(t) or t <= BASE:
                continue
            if t % 16 == 0 or img[t - 1 - BASE] in (0xCC, 0x90, 0xC3):
                TAIL_STARTS.add(t)


def noreturn_call(op):
    """call [iat] or call stub-that-jmps-[iat] to a function that never returns."""
    if op.type == x86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
        slot = op.mem.disp & 0xFFFFFFFF
    elif op.type == x86.X86_OP_IMM and in_text(op.imm):
        j = decode(op.imm)
        if not j or j.id != x86.X86_INS_JMP or j.operands[0].type != x86.X86_OP_MEM:
            return False
        slot = j.operands[0].mem.disp & 0xFFFFFFFF
    else:
        return False
    return iat.get(slot, "!").split("!")[-1] in NORETURN
jt_ranges = []  # (lo, hi) of jump tables read, to tell them from code pointers
indirect_jumps = []
bad_starts = set()


def find_table_bound(prev_insns, reg):
    """Look back for `cmp reg, imm` / `ja` to bound the jump table."""
    for k, p in reversed(list(enumerate(prev_insns))):
        if p.id in (x86.X86_INS_CMP, x86.X86_INS_AND) and len(p.operands) == 2:
            a, b = p.operands
            if a.type == x86.X86_OP_REG and b.type == x86.X86_OP_IMM and \
                    p.reg_name(a.reg) == reg:
                return b.imm + 1
        # MSVC two-level switch: movzx reg, byte [idx + bytetbl]; the
        # pointer table has max(bytetbl[0..N]) + 1 entries.
        if p.id == x86.X86_INS_MOVZX and len(p.operands) == 2 and \
                p.operands[0].type == x86.X86_OP_REG and p.reg_name(p.operands[0].reg) == reg and \
                p.operands[1].type == x86.X86_OP_MEM and p.operands[1].size == 1 and \
                p.operands[1].mem.base != 0 and p.operands[1].mem.index == 0:
            n = find_table_bound(prev_insns[:k], p.reg_name(p.operands[1].mem.base))
            if n is None or n > 4096:
                return None
            bt = p.operands[1].mem.disp & 0xFFFFFFFF
            return max(img[bt - BASE + j] for j in range(n)) + 1
    return None


def explore_function(start):
    """Return dict of blocks, or None if decoding fails."""
    blocks = {}
    work = deque([start])
    leaders = {start}
    calls = set()
    seen_insn = {}
    carry = {}  # fallthrough of a conditional jump keeps the history (switch bounds)
    while work:
        a = work.popleft()
        if a in seen_insn:
            continue
        prev = carry.pop(a, [])
        while True:
            if a in seen_insn:
                break
            i = decode(a)
            if not i or not in_text(a) or i.mnemonic in JUNK:
                # Just after a call (and maybe alignment filler) this is the
                # end of the function: the call does not return.
                if any(p.id == x86.X86_INS_CALL for p in prev[-3:]):
                    break
                return None, None
            seen_insn[a] = i.size
            prev.append(i)
            if len(prev) > 8:
                del prev[0]
            nxt = a + i.size
            g = set(i.groups)
            if i.id == x86.X86_INS_CALL:
                op = i.operands[0]
                if op.type == x86.X86_OP_IMM:
                    calls.add(op.imm)
                if noreturn_call(op):
                    break  # code after it belongs to the next function
                a = nxt
                continue
            if x86.X86_GRP_RET in g or i.id in (x86.X86_INS_INT3, x86.X86_INS_HLT):
                break
            if x86.X86_GRP_JUMP in g:
                op = i.operands[0]
                if op.type == x86.X86_OP_IMM:
                    t = op.imm
                    if i.id == x86.X86_INS_JMP:
                        if TAIL_STARTS and t != start and t in TAIL_STARTS:
                            calls.add(t)  # tail call: the target is a function of its own
                            break
                        if t not in leaders:
                            leaders.add(t)
                            work.append(t)
                        break
                    for t2 in (t, nxt):
                        if t2 not in leaders:
                            leaders.add(t2)
                            work.append(t2)
                    carry.setdefault(nxt, list(prev))
                    break
                if op.type == x86.X86_OP_MEM and op.mem.base == 0 and op.mem.scale == 4 \
                        and op.mem.index != 0:
                    reg = i.reg_name(op.mem.index)
                    n = find_table_bound(prev, reg)
                    tbl = op.mem.disp & 0xFFFFFFFF
                    targets = []
                    k = 0
                    while (n is None or k < n) and k < 1024:
                        t = u32(tbl + 4 * k)
                        k += 1
                        if not in_text(t):
                            if n is None:
                                break
                            continue  # bounded table: index may never be this value
                        targets.append(t)
                    jump_tables[i.address] = targets
                    jt_ranges.append((tbl, tbl + 4 * k))
                    for t in targets:
                        if t not in leaders:
                            leaders.add(t)
                            work.append(t)
                    break
                if op.type == x86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 \
                        and (op.mem.disp & 0xFFFFFFFF) in iat:
                    break  # tail call to import
                indirect_jumps.append(i.address)
                break
            a = nxt
            if a in leaders:
                break
    # Split instruction stream into blocks at leaders.
    addrs = sorted(seen_insn)
    for a in addrs:
        pass
    cur = None
    for idx, a in enumerate(addrs):
        if cur is None or a in leaders or (idx and addrs[idx - 1] + seen_insn[addrs[idx - 1]] != a):
            cur = a
            blocks[cur] = {"insns": array("I")}
        blocks[cur]["insns"].append(a)
    return blocks, calls


def main():
    if NEW_BASE is not None:
        scan_tail_starts()
        print("tail-call starts %d" % len(TAIL_STARTS), flush=True)
    entry = BASE + pe.OPTIONAL_HEADER.AddressOfEntryPoint
    roots = {entry} | {a for a in EXPORTS.values() if in_text(a)}
    # Code pointers in data sections.
    for s in pe.sections:
        if s.Name.startswith(b".text") or s.Name.startswith(b".rsrc"):
            continue
        lo = BASE + s.VirtualAddress
        hi = lo + min(s.Misc_VirtualSize, s.SizeOfRawData)
        for a in range(lo, hi - 3, 4):
            v = u32(a)
            if in_text(v):
                roots.add(v)
    text_relocs = []
    # Relocated absolute addresses that point at code: in DLLs this covers
    # vtables and function-pointer tables merged into .text.
    if hasattr(pe, "DIRECTORY_ENTRY_BASERELOC"):
        for blk in pe.DIRECTORY_ENTRY_BASERELOC:
            for e in blk.entries:
                if e.type == 3:
                    v = u32(BASE + e.rva)
                    if in_text(v):
                        if in_text(BASE + e.rva):
                            text_relocs.append((BASE + e.rva, v))
                        else:
                            roots.add(v)
    # Immediate code pointers in instructions (push offset func, mov [x], func).
    off = text.VirtualAddress
    for a in range(TEXT_LO, TEXT_HI - 4):
        if img[a - BASE] in (0x68, 0xB8, 0xB9, 0xBA, 0xBB, 0xBE, 0xBF) or \
                (img[a - BASE] == 0xC7):
            pass
    funcs = {}
    work = deque(sorted(roots))
    data_roots = set(roots)
    while work:
        f = work.popleft()
        if f in funcs or f in bad_starts:
            continue
        blocks, calls = explore_function(f)
        if blocks is None:
            bad_starts.add(f)
            continue
        funcs[f] = blocks
        for c in calls:
            if in_text(c) and c not in funcs:
                work.append(c)
    # Relocated pointers stored in code: vtables and function tables merged
    # into .text are roots, but switch jump tables (also there) are not:
    # every case of a big parser switch would become a copy of the function.
    def explore_all(work):
        while work:
            f = work.popleft()
            if f in funcs or f in bad_starts:
                continue
            blocks, calls = explore_function(f)
            if blocks is None:
                bad_starts.add(f)
                continue
            funcs[f] = blocks
            for c in calls:
                if in_text(c) and c not in funcs:
                    work.append(c)
    import bisect
    rng = sorted(jt_ranges)
    starts = [lo for lo, _ in rng]
    def in_jt(a):
        k = bisect.bisect_right(starts, a) - 1
        return k >= 0 and rng[k][0] <= a < rng[k][1]
    skipped = 0
    later = deque()
    for loc, v in sorted(text_relocs):
        if in_jt(loc):
            skipped += 1
        else:
            later.append(v)
    print("text relocs %d, %d in jump tables skipped" % (len(text_relocs), skipped), flush=True)
    explore_all(later)
    # Second pass: immediates inside discovered code that point at text.
    imm_roots = set()
    for f, blocks in funcs.items():
        for b in blocks.values():
            for a in b["insns"]:
                i = decode(a)
                if i.id in (x86.X86_INS_CALL,) or x86.X86_GRP_JUMP in i.groups:
                    continue
                for op in i.operands:
                    if op.type == x86.X86_OP_IMM and in_text(op.imm & 0xFFFFFFFF):
                        imm_roots.add(op.imm & 0xFFFFFFFF)
    work = deque(sorted(imm_roots - set(funcs)))
    while work:
        f = work.popleft()
        if f in funcs or f in bad_starts:
            continue
        blocks, calls = explore_function(f)
        if blocks is None:
            bad_starts.add(f)
            continue
        funcs[f] = blocks
        for c in calls:
            if in_text(c) and c not in funcs:
                work.append(c)

    # Gap filling: undiscovered code that follows padding (functions only
    # reachable through tables we have not resolved yet). Iterate to a
    # fixed point; jump-table data inside .text is excluded.
    table_bytes = set()
    for k, v in jump_tables.items():
        i = decode(k)
        tbl = i.operands[0].mem.disp & 0xFFFFFFFF
        table_bytes.update(range(tbl, tbl + 4 * len(v)))
    def coverage():
        cov = bytearray(TEXT_HI - TEXT_LO)
        for blocks in funcs.values():
            for b in blocks.values():
                for a in b["insns"]:
                    for x in range(a, a + decode(a).size):
                        cov[x - TEXT_LO] = 1
        for x in table_bytes:
            if in_text(x):
                cov[x - TEXT_LO] = 1
        return cov
    def skip_pad(a):
        while a < TEXT_HI:
            if img[a - BASE] in (0xCC, 0x90):
                a += 1
                continue
            i = decode(a)
            if i and i.mnemonic == "lea" and len(i.operands) == 2 and \
                    i.operands[1].type == x86.X86_OP_MEM and i.operands[1].mem.disp == 0 and \
                    i.operands[1].mem.base == i.operands[0].reg and \
                    i.operands[1].mem.index in (0, i.operands[0].reg):
                a += i.size
                continue
            if i and i.mnemonic == "nop":
                a += i.size
                continue
            break
        return a
    for rnd in range(8):
        cov = coverage()
        added = 0
        a = TEXT_LO
        while a < TEXT_HI:
            if cov[a - TEXT_LO]:
                a += 1
                continue
            s0 = skip_pad(a)
            if s0 >= TEXT_HI or cov[s0 - TEXT_LO]:
                a = max(s0, a + 1)
                continue
            # only accept starts right after a ret/jmp/padding
            blocks, calls = explore_function(s0) if s0 not in funcs and s0 not in bad_starts else (None, None)
            ok = blocks is not None
            # A start right after padding or a ret is a real function entry,
            # even when its body overlaps code already found from a loop head.
            clean = img[s0 - BASE - 1] in (0xCC, 0x90, 0xC3)
            if ok and not clean:
                for b in blocks.values():
                    for x in b["insns"]:
                        if cov[x - TEXT_LO]:
                            ok = False
                            break
                    if not ok:
                        break
            if ok:
                funcs[s0] = blocks
                added += 1
                for b in blocks.values():
                    for x in b["insns"]:
                        for y in range(x, x + decode(x).size):
                            cov[y - TEXT_LO] = 1
                for k, v in jump_tables.items():
                    tbl = decode(k).operands[0].mem.disp & 0xFFFFFFFF
                    for y in range(tbl, tbl + 4 * len(v)):
                        if in_text(y):
                            table_bytes.add(y)
                            cov[y - TEXT_LO] = 1
            else:
                bad_starts.add(s0)
            # advance to next unexplored point after this run
            b = s0
            while b < TEXT_HI and not cov[b - TEXT_LO]:
                b += 1
                if not ok:
                    break
            a = max(b, s0 + 1)
        print("gap round", rnd, "added", added)
        if not added:
            break

    # Functions found by gap filling make direct calls and take addresses
    # of their own; follow those too (a call target may so far only be
    # known as the tail-jump block of another function).
    for rnd in range(8):
        targets = set()
        for f, blocks in funcs.items():
            for b in blocks.values():
                for a in b["insns"]:
                    i = decode(a)
                    if x86.X86_GRP_JUMP in i.groups:
                        continue
                    for op in i.operands:
                        if op.type == x86.X86_OP_IMM and in_text(op.imm & 0xFFFFFFFF):
                            targets.add(op.imm & 0xFFFFFFFF)
        work = deque(sorted(targets - set(funcs) - bad_starts))
        added = 0
        while work:
            f = work.popleft()
            if f in funcs or f in bad_starts:
                continue
            blocks, calls = explore_function(f)
            if blocks is None:
                bad_starts.add(f)
                continue
            funcs[f] = blocks
            added += 1
            for c in calls:
                if in_text(c) and c not in funcs:
                    work.append(c)
        print("call closure round", rnd, "added", added)
        if not added:
            break

    # Coverage of .text.
    covered = set()
    for blocks in funcs.values():
        for b in blocks.values():
            for a in b["insns"]:
                covered.update(range(a, a + decode(a).size))
    pad = sum(1 for a in range(TEXT_LO, TEXT_HI) if img[a - BASE] in (0xCC, 0x90))
    cov = bytearray(TEXT_HI - TEXT_LO)
    for a in covered:
        cov[a - TEXT_LO] = 1
    gaps = []
    a = TEXT_LO
    while a < TEXT_HI:
        if not cov[a - TEXT_LO] and img[a - BASE] not in (0xCC, 0x90):
            b = a
            while b < TEXT_HI and not cov[b - TEXT_LO]:
                b += 1
            while b > a and img[b - 1 - BASE] in (0xCC, 0x90):
                b -= 1
            gaps.append((a, b))
            a = b
        a += 1
    print("gaps", len(gaps), "gap bytes", sum(b - a for a, b in gaps))
    json.dump(gaps, open(OUT + ".gaps", "w"))
    print("functions", len(funcs), "bad data roots", len(bad_starts),
          "jump tables", len(jump_tables), "unresolved indirect jmps", len(indirect_jumps))
    print("text bytes", TEXT_HI - TEXT_LO, "covered", len(covered), "cc/nop bytes", pad)
    out = {
        "image_base": BASE,
        "exports": {k: hex(v) for k, v in EXPORTS.items()},
        "entry": entry,
        "functions": {hex(f): sorted([b, b2["insns"][-1] + decode(b2["insns"][-1]).size] for b, b2 in blocks.items()) for f, blocks in funcs.items()},
        "jump_tables": {hex(k): [hex(t) for t in v] for k, v in jump_tables.items()},
        "indirect_jumps": [hex(a) for a in indirect_jumps],
        "iat": {hex(k): v for k, v in iat.items()},
    }
    json.dump(out, open(OUT, "w"))


if __name__ == "__main__":
    main()
