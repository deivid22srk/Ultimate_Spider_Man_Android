"""Differential test of the lifter against a reference x86 emulator (unicorn).

Usage: difftest.py LEGOBatman.exe functions.json workdir [max_per_shape]

Harvests one or more samples of every distinct instruction shape the game
uses, wraps each in a snippet that loads known register/x87/SSE/MMX state
and stores the results, lifts the snippets with lift.py, runs them natively
and under unicorn, and reports every instruction whose results differ.
"""
import os
import random
import struct
import subprocess
import sys

EXE, FUNCS, WORK = sys.argv[1:4]
PER_SHAPE = int(sys.argv[4]) if len(sys.argv) > 4 else 3
sys.argv = [sys.argv[0], EXE, FUNCS, WORK]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lift  # noqa: E402
from capstone import x86  # noqa: E402
import unicorn as U  # noqa: E402
from unicorn import x86_const as UX  # noqa: E402

CODE = 0x7F000000
SLOT = 0x100
CTRL = 0x0F000000
CW, FIN, XIN, MIN, FLG, FSW, FOUT, XOUT, MOUT = (CTRL + o for o in
                                                (0, 0x10, 0x40, 0xC0, 0x100, 0x104, 0x110, 0x140, 0x1C0))
SCR = 0x10000000
SCR_SIZE = 0x10000
PAGE = 0x1000

SKIP = {"int3", "int", "cpuid", "rdtsc", "div", "idiv", "hlt", "ud2", "wait", "fwait",
        "fldenv", "fnstenv", "fnsave", "frstor", "fxsave", "fxrstor", "fldcw", "ldmxcsr",
        "femms", "pswapd", "pi2fd", "pf2id", "fnclex", "fninit", "sysenter", "leave",
        "popfd", "popal", "pushal", "lock cmpxchg", "lock xadd", "lock inc", "lock dec",
        "fnstcw", "stmxcsr", "lahf", "sahf", "prefetchnta", "prefetcht0", "prefetch",
        "prefetchw", "lfence", "sfence", "mfence", "nop", "fptan", "fsincos"}

rng = random.Random(1234)


def rand_reg():
    k = rng.randrange(6)
    if k == 0:
        return rng.getrandbits(32)
    if k == 1:
        return rng.randrange(0, 300)
    if k == 2:
        return rng.choice([0, 1, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 0xFF, 0x100, 0x8000])
    if k == 3:
        return struct.unpack("<I", struct.pack("<f", rng.uniform(-1000, 1000)))[0]
    if k == 4:
        return (-rng.randrange(1, 300)) & 0xFFFFFFFF
    return rng.getrandbits(rng.choice([4, 8, 12, 16]))


def rand_double():
    k = rng.randrange(6)
    if k == 0:
        return rng.uniform(-1000, 1000)
    if k == 1:
        return float(rng.randrange(-100, 100))
    if k == 2:
        return rng.choice([0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 1e-3, 3.25, -2.5, 1e6])
    if k == 3:
        return rng.uniform(-1, 1)
    if k == 4:
        return rng.uniform(0, 7)
    return rng.uniform(-1e5, 1e5)


def rand_bytes_floaty(n):
    out = bytearray()
    while len(out) < n:
        k = rng.randrange(4)
        if k == 0:
            out += struct.pack("<f", rng.uniform(-100, 100))
        elif k == 1:
            out += struct.pack("<d", rng.uniform(-100, 100))
        elif k == 2:
            out += struct.pack("<I", rng.getrandbits(32))
        else:
            out += struct.pack("<I", rng.randrange(0, 1000))
    return bytes(out[:n])


def harvest():
    funcs = lift.data["functions"]
    shapes = {}
    for fa, blocks in funcs.items():
        for a, b in blocks:
            try:
                insns = lift.decode_block(a, b)
            except Exception:
                continue
            for i in insns:
                mn = i.mnemonic
                if mn in SKIP or mn in lift.JUNK or mn.startswith("rep") or mn.startswith("pf"):
                    continue
                g = set(i.groups)
                if mn in ("call", "loop", "jecxz", "ret", "retn") or x86.X86_GRP_JUMP in g or x86.X86_GRP_RET in g:
                    continue
                if mn in ("movsb", "movsw", "movsd", "stosb", "stosw", "stosd", "lodsb", "lodsd",
                          "scasb", "cmpsb", "cmpsd") and not i.operands:
                    pass
                ok = True
                for op in i.operands:
                    if op.type == x86.X86_OP_MEM:
                        m = op.mem
                        if m.segment and i.reg_name(m.segment) not in ("ds", "ss", "es"):
                            ok = False
                        if m.base and abs(m.disp) > 0x3000:
                            ok = False
                if not ok:
                    continue
                key = (mn, tuple((op.type, op.size) for op in i.operands))
                lst = shapes.setdefault(key, [])
                if len(lst) < PER_SHAPE and all(x.bytes != i.bytes for x in lst):
                    lst.append(i)
    return shapes


def asm_mem(op, reg, addr):
    return bytes(op) + bytes([(reg << 3) | 5]) + struct.pack("<I", addr)


def build_snippet(i, mmx):
    s = bytearray()
    s += bytes([0xD9, 0x2D]) + struct.pack("<I", CW)  # fldcw
    if not mmx:
        for k in range(4):
            s += bytes([0xDD, 0x05]) + struct.pack("<I", FIN + 8 * k)
    for k in range(8):
        s += asm_mem([0x0F, 0x10], k, XIN + 16 * k)
    if mmx:
        for k in range(8):
            s += asm_mem([0x0F, 0x6F], k, MIN + 8 * k)
    s += i.bytes
    s += bytes([0x9C, 0x8F, 0x05]) + struct.pack("<I", FLG)
    s += bytes([0xDD, 0x3D]) + struct.pack("<I", FSW)
    if not mmx:
        for k in range(4):
            s += bytes([0xDD, 0x1D]) + struct.pack("<I", FOUT + 8 * k)
    for k in range(8):
        s += asm_mem([0x0F, 0x11], k, XOUT + 16 * k)
    if mmx:
        for k in range(8):
            s += asm_mem([0x0F, 0x7F], k, MOUT + 8 * k)
        s += bytes([0x0F, 0x77])
    s += b"\xC3"
    return bytes(s)


def is_mmx(i):
    for op in i.operands:
        if op.type == x86.X86_OP_REG and i.reg_name(op.reg).startswith("mm"):
            return True
    return False


def make_tests(shapes):
    tests = []
    for key, lst in sorted(shapes.items(), key=lambda kv: kv[0][0]):
        for i in lst:
            base_regs, idx_regs = set(), set()
            pages = set()
            for op in i.operands:
                if op.type == x86.X86_OP_MEM:
                    m = op.mem
                    if m.base:
                        base_regs.add(i.reg_name(m.base))
                    if m.index:
                        idx_regs.add(i.reg_name(m.index))
                    if not m.base:
                        d = m.disp & 0xFFFFFFFF
                        span = 64 * m.scale + 16 if m.index else 16
                        for p in range(d & ~(PAGE - 1), d + span + PAGE, PAGE):
                            pages.add(p)
            regs = []
            for r in lift.REG32:
                if r in idx_regs:
                    v = rng.randrange(0, 64)
                elif r in base_regs or r in ("esp", "esi", "edi", "ebp"):
                    v = SCR + 0x4000 + rng.randrange(0, 0x8000) // 16 * 16
                else:
                    v = rand_reg()
                regs.append(v)
            mmx = is_mmx(i)
            tests.append(dict(i=i, regs=regs, mmx=mmx, pages=sorted(pages),
                              fin=[rand_double() for _ in range(4)],
                              xin=rand_bytes_floaty(128), min=rand_bytes_floaty(64)))
    return tests


def lift_all(tests):
    code = bytearray()
    for k, t in enumerate(tests):
        sn = build_snippet(t["i"], t["mmx"])
        assert len(sn) <= SLOT, len(sn)
        t["snip"] = sn
        code += sn + b"\xCC" * (SLOT - len(sn))
    lift.img = bytes(code)
    lift.BASE = CODE
    lift.iat = {}
    lines = ['#include "x86.h"', '#include "lift_prelude.h"']
    ok = []
    for k, t in enumerate(tests):
        a = CODE + k * SLOT
        try:
            L = lift.Lifter(a, [(a, a + len(t["snip"]))], {a})
            lines += L.emit()
            ok.append(k)
            t["lifted"] = True
        except lift.Unsupported as ex:
            t["lifted"] = False
            t["why"] = str(ex)
    return code, lines, ok


HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <setjmp.h>
#include "x86.h"
uint8_t *MEM;
static jmp_buf g_jb; static const char *g_why;
void rt_call(CPU *c, uint32_t t) { (void)c; (void)t; g_why = "rt_call"; longjmp(g_jb, 1); }
void rt_unimpl(CPU *c, uint32_t a, const char *w) { (void)c; (void)a; g_why = w; longjmp(g_jb, 1); }
void rt_int3(CPU *c, uint32_t a) { (void)c; (void)a; g_why = "int3"; longjmp(g_jb, 1); }
void rt_cpuid(CPU *c, uint32_t *a, uint32_t *b, uint32_t *cc, uint32_t *d) { (void)c; *a = *b = *cc = *d = 0; }
uint64_t rt_rdtsc(void) { return 0; }
jmp_buf *rt_setjmp_slot(CPU *c, uint32_t g) { (void)c; (void)g; return &g_jb; }
void rt_setjmp3(CPU *c) { (void)c; }
#include "tests.inc"
int main(int argc, char **argv) {
    MEM = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    FILE *f = fopen(argv[1], "rb");
    uint32_t n, npages; fread(&n, 4, 1, f); fread(&npages, 4, 1, f);
    static uint8_t scr[0x10000]; fread(scr, 1, sizeof scr, f);
    uint32_t *paddr = malloc(npages * 4); uint8_t *pdata = malloc((size_t)npages * 4096);
    for (uint32_t p = 0; p < npages; p++) { fread(&paddr[p], 4, 1, f); fread(pdata + (size_t)p * 4096, 1, 4096, f); }
    for (uint32_t k = 0; k < n; k++) {
        uint32_t id, regs[8]; double fin[4]; uint8_t xin[128], min[64];
        fread(&id, 4, 1, f); fread(regs, 4, 8, f); fread(fin, 8, 4, f); fread(xin, 1, 128, f); fread(min, 1, 64, f);
        memcpy(MEM + 0x10000000u, scr, sizeof scr);
        for (uint32_t p = 0; p < npages; p++) memcpy(MEM + paddr[p], pdata + (size_t)p * 4096, 4096);
        memset(MEM + 0x0F000000u, 0, 4096);
        *(uint16_t *)(MEM + 0x0F000000u) = 0x27F;
        memcpy(MEM + 0x0F000010u, fin, 32); memcpy(MEM + 0x0F000040u, xin, 128); memcpy(MEM + 0x0F0000C0u, min, 64);
        CPU c; memset(&c, 0, sizeof c); memcpy(c.r, regs, 32); c.fpu_cw = 0x27F; c.mxcsr = 0x1F80;
        printf("T %u", id);
        g_why = NULL;
        if (setjmp(g_jb)) { printf(" TRAP %s\n", g_why); continue; }
        g_tests[id](&c);
        for (int r = 0; r < 8; r++) printf(" %08x", r == 4 ? c.r[r] - 4 : c.r[r]);
        printf(" |");
        for (int o = 0x100; o < 0x200; o += 4) printf(" %08x", *(uint32_t *)(MEM + 0x0F000000u + o));
        printf(" |");
        for (int o = 0; o < 0x10000; o += 4)
            if (*(uint32_t *)(MEM + 0x10000000u + o) != *(uint32_t *)(scr + o))
                printf(" %x=%08x", 0x10000000u + o, *(uint32_t *)(MEM + 0x10000000u + o));
        for (uint32_t p = 0; p < npages; p++)
            for (int o = 0; o < 4096; o += 4)
                if (*(uint32_t *)(MEM + paddr[p] + o) != *(uint32_t *)(pdata + (size_t)p * 4096 + o))
                    printf(" %x=%08x", paddr[p] + o, *(uint32_t *)(MEM + paddr[p] + o));
        printf("\n");
    }
    return 0;
}
'''


def run_unicorn(tests, code, scr, pages):
    mu = U.Uc(U.UC_ARCH_X86, U.UC_MODE_32)
    mu.mem_map(CODE, (len(code) + 0xFFFF) & ~0xFFFF)
    mu.mem_write(CODE, bytes(code))
    mu.mem_map(CTRL, PAGE)
    mu.mem_map(SCR, SCR_SIZE)
    for p in pages:
        try:
            mu.mem_map(p, PAGE)
        except U.UcError:
            pass
    mu.reg_write(UX.UC_X86_REG_CR0, (mu.reg_read(UX.UC_X86_REG_CR0) & ~4) | 2)
    mu.reg_write(UX.UC_X86_REG_CR4, mu.reg_read(UX.UC_X86_REG_CR4) | 0x600)
    regs_ids = [UX.UC_X86_REG_EAX, UX.UC_X86_REG_ECX, UX.UC_X86_REG_EDX, UX.UC_X86_REG_EBX,
                UX.UC_X86_REG_ESP, UX.UC_X86_REG_EBP, UX.UC_X86_REG_ESI, UX.UC_X86_REG_EDI]
    out = {}
    for k, t in enumerate(tests):
        if not t["lifted"]:
            continue
        mu.mem_write(SCR, scr)
        for p, d in pages.items():
            mu.mem_write(p, d)
        ctrl = bytearray(PAGE)
        ctrl[0:2] = struct.pack("<H", 0x27F)
        ctrl[0x10:0x30] = struct.pack("<4d", *t["fin"])
        ctrl[0x40:0xC0] = t["xin"]
        ctrl[0xC0:0x100] = t["min"]
        mu.mem_write(CTRL, bytes(ctrl))
        for r, v in zip(regs_ids, t["regs"]):
            mu.reg_write(r, v)
        mu.reg_write(UX.UC_X86_REG_EFLAGS, 0x202)
        a = CODE + k * SLOT
        end = a + len(t["snip"]) - 1
        try:
            mu.emu_start(a, end, count=200)
        except U.UcError as ex:
            out[k] = "T %d TRAP uc:%s" % (k, ex)
            continue
        s = "T %d" % k
        for r in regs_ids:
            s += " %08x" % mu.reg_read(r)
        s += " |"
        c2 = mu.mem_read(CTRL, PAGE)
        for o in range(0x100, 0x200, 4):
            s += " %08x" % struct.unpack_from("<I", c2, o)[0]
        s += " |"
        m2 = mu.mem_read(SCR, SCR_SIZE)
        if m2 != scr:
            for o in range(0, SCR_SIZE, 4):
                if m2[o:o + 4] != scr[o:o + 4]:
                    s += " %x=%08x" % (SCR + o, struct.unpack_from("<I", m2, o)[0])
        for p, d in pages.items():
            m2 = mu.mem_read(p, PAGE)
            if m2 != d:
                for o in range(0, PAGE, 4):
                    if m2[o:o + 4] != d[o:o + 4]:
                        s += " %x=%08x" % (p + o, struct.unpack_from("<I", m2, o)[0])
        out[k] = s
    return out


FLAG_MASK = 0x8C5


def compare(a, b, t):
    """Return a list of differences between two result lines (ours, reference)."""
    if "TRAP" in a or "TRAP" in b:
        return [] if ("TRAP" in a) == ("TRAP" in b) else ["trap: ours=%s ref=%s" % (a, b)]
    ra, ca, ma = a.split("|")
    rb, cb, mb = b.split("|")
    diffs = []
    ga, gb = ra.split()[2:], rb.split()[2:]
    for n, (x, y) in enumerate(zip(ga, gb)):
        if x != y:
            diffs.append("%s ours=%s ref=%s" % (lift.REG32[n], x, y))
    wa, wb = [int(x, 16) for x in ca.split()], [int(x, 16) for x in cb.split()]
    if (wa[0] ^ wb[0]) & FLAG_MASK:
        diffs.append("flags ours=%03x ref=%03x" % (wa[0] & FLAG_MASK, wb[0] & FLAG_MASK))
    mn = t["i"].mnemonic
    fmask = 0x4700 if mn == "fxam" else 0x4500 if mn.startswith(("fcom", "fucom", "ftst", "fprem")) else 0
    if (wa[1] ^ wb[1]) & fmask:
        diffs.append("fsw ours=%04x ref=%04x" % (wa[1] & 0x4700, wb[1] & 0x4700))
    if not t["mmx"]:
        for k in range(4):
            x = struct.unpack("<d", struct.pack("<2I", wa[4 + 2 * k], wa[5 + 2 * k]))[0]
            y = struct.unpack("<d", struct.pack("<2I", wb[4 + 2 * k], wb[5 + 2 * k]))[0]
            if y != y:
                continue  # reference stack underflow / NaN: not comparable
            if x != y and not (abs(x - y) <= 1e-12 * max(abs(x), abs(y))):
                diffs.append("st%d ours=%r ref=%r" % (k, x, y))
    for k in range(8):
        xa, xb = wa[16 + 4 * k:20 + 4 * k], wb[16 + 4 * k:20 + 4 * k]
        if xa != xb:
            diffs.append("xmm%d ours=%s ref=%s" % (k, " ".join("%08x" % v for v in xa),
                                                   " ".join("%08x" % v for v in xb)))
    if t["mmx"]:
        for k in range(8):
            xa, xb = wa[48 + 2 * k:50 + 2 * k], wb[48 + 2 * k:50 + 2 * k]
            if xa != xb:
                diffs.append("mm%d ours=%s ref=%s" % (k, xa, xb))
    # pushfd in the epilogue leaves the flags word just below esp
    sa = {m for m in ma.split() if int(m.split("=")[0], 16) != int(gb[4], 16) - 4}
    sb = {m for m in mb.split() if int(m.split("=")[0], 16) != int(gb[4], 16) - 4}
    if sa != sb:
        diffs.append("mem ours-only=%s ref-only=%s" % (sorted(sa - sb)[:4], sorted(sb - sa)[:4]))
    return diffs


def main():
    os.makedirs(WORK, exist_ok=True)
    shapes = harvest()
    tests = make_tests(shapes)
    print("shapes", len(shapes), "tests", len(tests))
    code, lines, ok = lift_all(tests)
    print("lifted", len(ok))
    scr = rand_bytes_floaty(SCR_SIZE)
    pages = {}
    for t in tests:
        for p in t["pages"]:
            if p not in pages and not (CTRL <= p < CTRL + PAGE) and not (SCR <= p < SCR + SCR_SIZE) \
                    and not (CODE <= p < CODE + len(code) + 0x10000):
                pages[p] = rand_bytes_floaty(PAGE)
    open(os.path.join(WORK, "tests_body.c"), "w").write("\n".join(lines) + "\n")
    tbl = "#include \"tests_body.c\"\ntypedef void (*TF)(CPU *);\nstatic TF g_tests[] = {\n"
    tbl += "".join("    f_%X,\n" % (CODE + k * SLOT) if t["lifted"] else "    0,\n" for k, t in enumerate(tests))
    tbl += "};\n"
    open(os.path.join(WORK, "tests.inc"), "w").write(tbl)
    open(os.path.join(WORK, "harness.c"), "w").write(HARNESS)
    rt = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "runtime")
    exe = os.path.join(WORK, "harness")
    subprocess.check_call(["gcc", "-O1", "-w", "-I", rt, "-I", WORK, "-o", exe,
                           os.path.join(WORK, "harness.c"), os.path.join(rt, "fpu.c"), "-lm"])
    with open(os.path.join(WORK, "input.bin"), "wb") as f:
        f.write(struct.pack("<II", len(ok), len(pages)))
        f.write(scr)
        for p, d in pages.items():
            f.write(struct.pack("<I", p) + d)
        for k in ok:
            t = tests[k]
            f.write(struct.pack("<I8I4d", k, *t["regs"], *t["fin"]) + t["xin"] + t["min"])
    res = subprocess.run([exe, os.path.join(WORK, "input.bin")], capture_output=True, text=True)
    ours = {}
    for line in res.stdout.splitlines():
        ours[int(line.split()[1])] = line
    ref = run_unicorn(tests, code, scr, pages)
    bad = {}
    for k in ok:
        t = tests[k]
        if k not in ours:
            bad.setdefault(t["i"].mnemonic, []).append((t, ["no output (crash?)"]))
            continue
        d = compare(ours[k], ref.get(k, "TRAP missing"), t)
        if d:
            bad.setdefault(t["i"].mnemonic, []).append((t, d))
    with open(os.path.join(WORK, "report.txt"), "w") as f:
        for mn in sorted(bad):
            for t, d in bad[mn]:
                f.write("%-10s %-40s %s\n" % (mn, t["i"].mnemonic + " " + t["i"].op_str, "; ".join(d)))
    print("failing mnemonics:", len(bad), "of", len({tests[k]["i"].mnemonic for k in ok}))
    print(" ".join("%s(%d)" % (mn, len(v)) for mn, v in sorted(bad.items())))
    if res.returncode:
        print("harness exit", res.returncode, res.stderr[-500:])


main()
