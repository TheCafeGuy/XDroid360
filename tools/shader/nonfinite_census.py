"""Static census over Xenos ucode: which SM3 products can see +-inf/NaN.
Taint = temp component may hold inf/NaN given finite constants, fetches and
interpolants. Flow-sensitive replay for straight-line code, union fixpoint
otherwise. Mirror of xdroid-core/src/main/cpp/xdroid_nonfinite.cc.
Usage: nonfinite_census.py <dir> [--keys]
--keys: one line per shader in the translator's unit (one guarded product per
differing component), keyed by FNV-1a-64 over the ucode dwords like the
XDROID-RMUL log line (xdroid_relaxed_mul bit 3)."""
import glob, os, struct, sys

V_OPS = 2  # default operand count
V_COUNT = {8: 1, 9: 1, 10: 1, 19: 1, 11: 3, 12: 3, 13: 3, 14: 3, 17: 3}
V_CLEAN = {4, 5, 6, 7, 24, 25, 26, 27}  # seq/sgt/sge/sne, kill*: 0 or 1
V_PRODUCTS = {1: 'mul', 11: 'mad', 15: 'dp4', 16: 'dp3', 17: 'dp2add', 28: 'dst'}
S_ZERO = {33, 50}  # setp_clr, retain_prev: no operand
S_TWO = {42, 43, 44, 45, 46, 47}
S_CLEAN = {7, 8, 9, 10, 27, 28, 29, 30, 35, 36, 37, 38, 39}  # sets, kills
# exp logc log rcp rsqc rsqf rsq sqrt, plus the +-FLT_MAX sentinels (rcpc,
# setp_clr, muls_prev2): FLT_MAX * |y| > 1 overflows to inf.
S_SOURCE = {14, 15, 16, 19, 20, 21, 22, 40, 17, 33, 4}
S_PREV = {1, 3, 4, 26, 50}
S_PRODUCTS = {2: 'muls', 3: 'muls_prev', 4: 'muls_prev2', 42: 'mulsc', 43: 'mulsc'}
V_COMPS = {16: [0, 1, 2], 17: [0, 1]}


def comps(swiz, used):
    return {((swiz >> (2 * i)) + i) & 3 for i in used}


class Instr:
    def __init__(self, d0, d1, d2, cond):
        self.d0, self.d1, self.d2 = d0, d1, d2
        self.vop, self.sop = (d2 >> 24) & 31, d0 >> 26
        self.regs = [(d2 >> 16) & 255, (d2 >> 8) & 255, d2 & 255]
        self.sels = [(d2 >> 31) & 1, (d2 >> 30) & 1, (d2 >> 29) & 1]
        self.swz = [(d1 >> 16) & 255, (d1 >> 8) & 255, d1 & 255]
        self.neg = [(d1 >> 26) & 1, (d1 >> 25) & 1, (d1 >> 24) & 1]
        self.may = cond or (d1 >> 28) & 1
        self.export = (d0 >> 15) & 1
        vmask, smask = (d0 >> 16) & 15, (d0 >> 20) & 15
        self.vused = vmask & ~smask if self.export else vmask

    def identical(self):  # vector operands 0/1, GetIdenticalComponents
        if (self.regs[0], self.sels[0], self.neg[0]) != (self.regs[1], self.sels[1], self.neg[1]):
            return 0
        return sum(1 << i for i in range(4)
                   if comps(self.swz[0], [i]) == comps(self.swz[1], [i]))


class State:
    def __init__(self):
        self.taint, self.prev, self.rel = [0] * 64, False, False

    def opnd(self, ins, n, used):
        if not ins.sels[n]:
            return False
        r = ins.regs[n]
        if r & 0x40:
            return self.rel or any(self.taint)
        return bool(self.taint[r & 63] & sum(1 << c for c in comps(ins.swz[n], used)))

    def results(self, ins):
        vop, sop = ins.vop, ins.sop
        cnt = V_COUNT.get(vop, V_OPS)
        used = V_COMPS.get(vop, [0, 1, 2, 3])
        vt = vop not in V_CLEAN and any(
            self.opnd(ins, n, [0] if (vop == 17 and n == 2) else used) for n in range(cnt))
        if sop in S_TWO:
            tr = (sop & 1) | (ins.sels[2] << 1) | (ins.swz[2] & 0x3C)
            so = bool(self.taint[tr] & 1 << (ins.swz[2] & 3))
        elif sop in S_ZERO:
            so = False
        else:
            so = self.opnd(ins, 2, [3, 0])
        st = (so or sop in S_SOURCE) and sop not in S_CLEAN
        if sop in S_PREV:
            st = st or self.prev
        return vt, st

    def products(self, ins):
        """[(units, op0 may be inf/NaN, op1 may be inf/NaN)] in the
        translator's unit, and the old census unit count with its taint."""
        out, old = [], []
        vop, sop = ins.vop, ins.sop
        if vop in V_PRODUCTS and ins.vused:
            c = V_COMPS.get(vop, [0, 1, 2, 3])
            b = (self.opnd(ins, 0, c), self.opnd(ins, 1, c))
            diff = {1: ins.vused, 11: ins.vused, 15: 15, 16: 7, 17: 3,
                    28: ins.vused & 2}[vop] & ~ins.identical()
            out.append((bin(diff).count('1'), *b))
            n = {15: 4, 16: 3, 17: 2, 28: 1}.get(vop, bin((ins.d0 >> 16) & 15).count('1') or 4)
            old.append((n, self.opnd(ins, 0, [0, 1, 2, 3]), self.opnd(ins, 1, [0, 1, 2, 3])))
        if sop in S_PRODUCTS:
            if sop in S_TWO:
                tr = (sop & 1) | (ins.sels[2] << 1) | (ins.swz[2] & 0x3C)
                b = (False, bool(self.taint[tr] & 1 << (ins.swz[2] & 3)))
                units = 1
            elif sop == 2:
                b = (self.opnd(ins, 2, [3]), self.opnd(ins, 2, [0]))
                units = int(comps(ins.swz[2], [3]) != comps(ins.swz[2], [0]))
            elif sop == 3:
                b, units = (self.opnd(ins, 2, [3]), self.prev), 1
            else:  # muls_prev2: the branch already requires a finite ps
                b, units = (self.opnd(ins, 2, [3, 0]), False), 1
            out.append((units, *b))
            old.append((1, *b))
        return out, old

    def write(self, ins, vt, st, union):
        if not ins.export:
            d0 = ins.d0
            for dest, rel, mask, t in ((d0 & 63, (d0 >> 6) & 1, (d0 >> 16) & 15, vt),
                                       ((d0 >> 8) & 63, (d0 >> 14) & 1, (d0 >> 20) & 15, st)):
                if not mask:
                    continue
                if rel:
                    if t:
                        self.rel = True
                        self.taint = [x | mask for x in self.taint]
                elif union or ins.may:
                    self.taint[dest] |= mask if t else 0
                else:
                    self.taint[dest] = (self.taint[dest] & ~mask) | (mask if t else 0)
        if ins.sop != 50:
            self.prev = (self.prev or st) if (union or ins.may) else st


def parse(raw):
    d = list(struct.unpack('>%dI' % (len(raw) // 4), raw))
    alus, loopy = [], False
    bound = len(d) // 3
    i = 0
    while i < bound:
        w = d[i * 3:i * 3 + 3]
        cfs = [(w[0], w[1] & 0xFFFF), ((w[1] >> 16) | ((w[2] << 16) & 0xFFFFFFFF), w[2] >> 16)]
        for lo, hi in cfs:
            op = hi >> 12
            if op in (7, 8, 9, 10, 11):
                loopy = True
            if op in (1, 2, 3, 4, 5, 6, 13, 14):
                addr, cnt, seq = lo & 0xFFF, (lo >> 12) & 7, (lo >> 16) & 0xFFF
                bound = min(bound, addr) if addr else bound
                for k in range(cnt):
                    o = (addr + k) * 3
                    if not (seq >> (2 * k)) & 1 and o + 3 <= len(d):
                        alus.append(Instr(*d[o:o + 3], op not in (1, 2)))
        i += 1
    key = 0xCBF29CE484222325
    for x in d:
        key = ((key ^ x) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return alus, loopy, key


def analyze(path):
    alus, loopy, key = parse(open(path, 'rb').read())
    s = State()
    if loopy:  # union fixpoint, then record on the converged state, no writes
        for _ in range(64):
            before = (list(s.taint), s.prev, s.rel)
            for ins in alus:
                s.write(ins, *s.results(ins), True)
            if before == (s.taint, s.prev, s.rel):
                break
        else:
            return key, None, None
    tr = [0, 0, 0]  # translator unit: plain, guard1, sm3
    old = [0, 0, 0]  # census unit (write-mask components)
    for ins in alus:
        new, legacy = s.products(ins)
        for acc, recs in ((tr, new), (old, legacy)):
            for n, b0, b1 in recs:
                acc[int(b0) + int(b1)] += n
        if not loopy:
            s.write(ins, *s.results(ins), False)
    return key, tr, old


keys = '--keys' in sys.argv
tot = {False: [0] * 8, True: [0] * 8}
for p in sorted(glob.glob(os.path.join(sys.argv[1], '*.bin'))):
    ps = os.path.basename(p).startswith('ps')
    key, tr, old = analyze(p)
    if tr is None:  # not converged: all SM3, like the translator
        print(f'{os.path.basename(p)}: fixpoint not converged')
        continue
    t = tot[ps]
    for i in range(3):
        t[i] += old[i]; t[4 + i] += tr[i]
    t[3] += 1 if old[1] or old[2] else 0
    if keys:
        print(f"{key:016X} {'ps' if ps else 'vs'} plain={tr[0]} guard1={tr[1]} sm3={tr[2]}"
              f" {os.path.basename(p)}")
for ps in (False, True):
    t = tot[ps]
    safe, need = t[0], t[1] + t[2]
    print(f"{'PS' if ps else 'VS'}: shaders_with_sm3_products={t[3]} products sm3={need} "
          f"relaxed_ok={safe} ({100*safe/max(1,need+safe):.0f}% relaxed) one-operand={t[1]} | "
          f"translator unit plain={t[4]} guard1={t[5]} sm3={t[6]}")
