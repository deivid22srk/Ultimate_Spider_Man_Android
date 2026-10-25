/* x87 helpers that are too large to inline. */
#include "rt.h"

double f80_load(uint32_t a) {
    uint64_t mant = M64(a);
    uint16_t se = M16(a + 8);
    int sign = se >> 15;
    int exp = se & 0x7FFF;
    double v;
    if (exp == 0 && mant == 0) v = 0.0;
    else if (exp == 0x7FFF) v = (mant << 1) ? NAN : INFINITY;
    else v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}

void f80_store(uint32_t a, double v) {
    uint16_t se = signbit(v) ? 0x8000 : 0;
    uint64_t mant = 0;
    if (v != v) { se |= 0x7FFF; mant = 0xC000000000000000ull; }
    else if (isinf(v)) { se |= 0x7FFF; mant = 0x8000000000000000ull; }
    else if (v != 0) {
        int e;
        double m = frexp(fabs(v), &e); /* m in [0.5,1) */
        mant = (uint64_t)ldexp(m, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    M64(a) = mant;
    M16(a + 8) = se;
}

void fxam(CPU *c) {
    double v = ST(0);
    c->c1 = signbit(v) ? 1 : 0;
    if (v != v) { c->c3 = 0; c->c2 = 0; c->c0 = 1; }
    else if (isinf(v)) { c->c3 = 0; c->c2 = 1; c->c0 = 1; }
    else if (v == 0) { c->c3 = 1; c->c2 = 0; c->c0 = 0; }
    else if (fpclassify(v) == FP_SUBNORMAL) { c->c3 = 1; c->c2 = 1; c->c0 = 0; }
    else { c->c3 = 0; c->c2 = 1; c->c0 = 0; }
}

void fprem(CPU *c, int ieee) {
    double a = ST(0), b = ST(1);
    double q = ieee ? nearbyint(a / b) : trunc(a / b);
    ST(0) = ieee ? remainder(a, b) : fmod(a, b);
    uint64_t qi = (uint64_t)fabs(q);
    c->c2 = 0;
    c->c0 = (qi >> 2) & 1;
    c->c3 = (qi >> 1) & 1;
    c->c1 = qi & 1;
}

void fpu_env(CPU *c, uint32_t a, int op) {
    switch (op) {
    case 0: /* fnstenv: 28-byte protected-mode environment */
    case 2: /* fnsave */
        memset(MEM + a, 0, op == 0 ? 28 : 108);
        M16(a) = c->fpu_cw;
        M16(a + 4) = fstatus(c);
        M16(a + 8) = 0xFFFF;
        if (op == 2) {
            for (int i = 0; i < 8; i++) f80_store(a + 28 + 10 * i, ST(i));
            c->top = 0;
            c->fpu_cw = 0x37F;
            c->c0 = c->c1 = c->c2 = c->c3 = 0;
        }
        break;
    case 1: /* fldenv */
    case 3: { /* frstor */
        c->fpu_cw = M16(a);
        uint16_t sw = M16(a + 4);
        c->c0 = (sw >> 8) & 1;
        c->c1 = (sw >> 9) & 1;
        c->c2 = (sw >> 10) & 1;
        c->c3 = (sw >> 14) & 1;
        c->top = (sw >> 11) & 7;
        if (op == 3)
            for (int i = 0; i < 8; i++) ST(i) = f80_load(a + 28 + 10 * i);
        break;
    }
    }
}
