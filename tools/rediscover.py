"""Incremental re-discovery: re-explore functions with jump tables and retry
gap starts with the current explore_function, merging into functions.json.
usage: rediscover.py EXE functions.json [new_base]"""
import json, sys, time
from collections import deque
EXE, FJ = sys.argv[1], sys.argv[2]
sys.argv = ['discover', EXE, '/dev/null'] + sys.argv[3:]
import discover as d
from capstone import x86

data = json.load(open(FJ))
funcs = {int(k, 16): v for k, v in data['functions'].items()}
jt = {int(k, 16): v for k, v in data['jump_tables'].items()}
t0 = time.time()

def ranges(blocks):
    return sorted([b, b2['insns'][-1] + d.decode(b2['insns'][-1]).size] for b, b2 in blocks.items())

# 1. functions containing a jump table: re-explore with the fixed bounds
owner = {}
starts = sorted(funcs)
import bisect
jt_funcs = set()
for k in jt:
    for f in (funcs.keys() if False else []): pass
rng_index = []
for f, rs in funcs.items():
    for lo, hi in rs:
        rng_index.append((lo, hi, f))
rng_index.sort()
los = [r[0] for r in rng_index]
for k in jt:
    i = bisect.bisect_right(los, k) - 1
    while i >= 0 and rng_index[i][0] <= k:
        if k < rng_index[i][1]:
            jt_funcs.add(rng_index[i][2])
        i -= 1
        if len(jt_funcs) and i < bisect.bisect_right(los, k) - 50: break
print('functions with jump tables', len(jt_funcs), flush=True)
changed = 0
for f in sorted(jt_funcs):
    b, c = d.explore_function(f)
    if b is None:
        continue
    r = ranges(b)
    if r != funcs[f]:
        changed += 1
    funcs[f] = r
print('re-explored, changed', changed, 'time', int(time.time() - t0), flush=True)

# 2. coverage, then gap starts
TL, TH = d.TEXT_LO, d.TEXT_HI
def build_cov():
    cov = bytearray(TH - TL)
    for rs in funcs.values():
        for lo, hi in rs:
            cov[lo - TL:hi - TL] = b'\x01' * (hi - lo)
    for k, v in d.jump_tables.items():
        tbl = d.decode(k).operands[0].mem.disp & 0xFFFFFFFF
        if d.in_text(tbl):
            cov[tbl - TL:tbl - TL + 4 * len(v)] = b'\x01' * (4 * len(v))
    return cov
def skip_pad(a):
    while a < TH and d.img[a - d.BASE] in (0xCC, 0x90):
        a += 1
    return a
bad = set()
def add_closure(work):
    n = 0
    while work:
        f = work.popleft()
        if f in funcs or f in bad or not d.in_text(f):
            continue
        b, c = d.explore_function(f)
        if b is None:
            bad.add(f); continue
        funcs[f] = ranges(b); n += 1
        for t in c:
            if t not in funcs: work.append(t)
    return n
for rnd in range(6):
    cov = build_cov()
    added = 0
    a = TL
    while a < TH:
        if cov[a - TL]:
            a += 1; continue
        s0 = skip_pad(a)
        if s0 >= TH or cov[s0 - TL]:
            a = max(s0, a + 1); continue
        clean = d.img[s0 - d.BASE - 1] in (0xCC, 0x90, 0xC3)
        ok = False
        if s0 not in funcs and s0 not in bad:
            b, c = d.explore_function(s0)
            ok = b is not None
            if ok and not clean:
                for bb in b.values():
                    if any(cov[x - TL] for x in bb['insns']):
                        ok = False; break
            if ok:
                funcs[s0] = r = ranges(b); added += 1
                for lo, hi in r:
                    cov[lo - TL:hi - TL] = b'\x01' * (hi - lo)
                added += add_closure(deque(t for t in c if t not in funcs))
            else:
                bad.add(s0)
        e = s0
        while e < TH and not cov[e - TL]:
            e += 1
            if not ok: break
        a = max(e, s0 + 1)
    print('gap round', rnd, 'added', added, 'time', int(time.time() - t0), flush=True)
    if not added:
        break

# 3. merge jump tables (new explorer results override)
for k, v in d.jump_tables.items():
    jt[k] = v
data['functions'] = {hex(f): r for f, r in sorted(funcs.items())}
data['jump_tables'] = {hex(k): [t if isinstance(t, str) else hex(t) for t in v] for k, v in jt.items()}
json.dump(data, open(FJ, 'w'))
print('functions', len(funcs), 'jump tables', len(jt), 'time', int(time.time() - t0))
