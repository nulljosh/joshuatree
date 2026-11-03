/* AAC-LC decoder: see aac.h. Syntax and tools from ISO/IEC 14496-3 subpart 4 (raw_data_block, individual_channel_stream,
   the filterbank). Written for Joshua Tree; the codebooks and band tables in aac_tab.inc are the standard's numbers. */
#include "aac.h"
#include "aac_tab.inc"

typedef unsigned char u8; typedef unsigned int u32;

/* ---- a little maths, no libm ---- */
static const double PI = 3.14159265358979323846;
static double msin(double x) {   /* range-reduced Taylor, good to ~1e-15 */
    double t = x / (2 * PI); t -= (double)(long long)t; if (t < 0) t += 1; x = t * 2 * PI;
    int neg = 0; if (x > PI) { x -= PI; neg = 1; } if (x > PI / 2) x = PI - x;
    double x2 = x * x, r = x, term = x;
    for (int k = 1; k < 12; k++) { term *= -x2 / ((2 * k) * (2 * k + 1)); r += term; }
    return neg ? -r : r;
}
static double mcos(double x) { return msin(x + PI / 2); }
static double msqrt(double v) { return v > 0 ? __builtin_sqrt(v) : 0; }
static double cbrt_(double v) { if (v <= 0) return 0; double r = v > 1 ? v / 3 : 1; for (int i = 0; i < 60; i++) r = r - (r * r * r - v) / (3 * r * r); return r; }
static double bessel_i0(double x) { double s = 1, t = 1; for (int k = 1; k < 50; k++) { t *= (x / 2) * (x / 2) / ((double)k * k); s += t; } return s; }

/* tables built once */
static int ready;
static float iq[8192];                      /* |q|^(4/3) */
static float win_sin_l[1024], win_kbd_l[1024], win_sin_s[128], win_kbd_s[128];   /* rising halves */
static float tw_l[512][2], tw_s[64][2];     /* DCT-IV post-twiddles: exp(-i pi (k + 1/4) / M) */
static float pre_l[512][2], pre_s[64][2];   /* and pre-twiddles: exp(-i pi k / M) */
static float fft_l[256][2], fft_s[32][2];   /* FFT twiddles for M/2 points */
static void kbd(float *w, int n, double alpha) {   /* the rising half of a Kaiser-Bessel derived window, n samples */
    double sum = 0, c[1025];
    for (int j = 0; j <= n; j++) { double r = (j - n / 2.0) / (n / 2.0); sum += bessel_i0(PI * alpha * msqrt(1 - r * r)); c[j] = sum; }
    for (int j = 0; j < n; j++) w[j] = (float)msqrt(c[j] / sum);
}
static void setup(void) {
    for (int i = 0; i < 8192; i++) iq[i] = (float)(i * cbrt_(i));
    for (int i = 0; i < 1024; i++) win_sin_l[i] = (float)msin(PI / 2048 * (i + 0.5));
    for (int i = 0; i < 128; i++) win_sin_s[i] = (float)msin(PI / 256 * (i + 0.5));
    kbd(win_kbd_l, 1024, 4); kbd(win_kbd_s, 128, 6);
    for (int k = 0; k < 512; k++) { double a = -PI * (4 * k + 1) / (4.0 * 1024); tw_l[k][0] = (float)mcos(a); tw_l[k][1] = (float)msin(a); }
    for (int k = 0; k < 64; k++) { double a = -PI * (4 * k + 1) / (4.0 * 128); tw_s[k][0] = (float)mcos(a); tw_s[k][1] = (float)msin(a); }
    for (int k = 0; k < 512; k++) { double a = -PI * k / 1024; pre_l[k][0] = (float)mcos(a); pre_l[k][1] = (float)msin(a); }
    for (int k = 0; k < 64; k++) { double a = -PI * k / 128; pre_s[k][0] = (float)mcos(a); pre_s[k][1] = (float)msin(a); }
    for (int k = 0; k < 256; k++) { double a = -2 * PI * k / 512; fft_l[k][0] = (float)mcos(a); fft_l[k][1] = (float)msin(a); }
    for (int k = 0; k < 32; k++) { double a = -2 * PI * k / 64; fft_s[k][0] = (float)mcos(a); fft_s[k][1] = (float)msin(a); }
    ready = 1;
}

/* in-place radix-2 FFT of n complex points; tw[k] = exp(-2 pi i k / n), k < n/2 */
static void fft(float (*z)[2], int n, const float (*tw)[2]) {
    for (int i = 1, j = 0; i < n; i++) {
        int b = n >> 1; for (; j & b; b >>= 1) j ^= b; j ^= b;
        if (i < j) { float t0 = z[i][0], t1 = z[i][1]; z[i][0] = z[j][0]; z[i][1] = z[j][1]; z[j][0] = t0; z[j][1] = t1; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        int step = n / len;
        for (int i = 0; i < n; i += len) for (int k = 0; k < len / 2; k++) {
            float wr = tw[k * step][0], wi = tw[k * step][1];
            float *a = z[i + k], *b = z[i + k + len / 2];
            float br = b[0] * wr - b[1] * wi, bi = b[0] * wi + b[1] * wr;
            b[0] = a[0] - br; b[1] = a[1] - bi; a[0] += br; a[1] += bi;
        }
    }
}
/* DCT-IV of M points (M = 1024 or 128) via an M/2-point FFT: y[n] = sum X[k] cos(pi/M (n+1/2)(k+1/2)) */
static void dct4(const float *X, float *y, int M) {
    static float z[512][2];
    /* Y[n] = y[2n] - i y[M-1-2n] = exp(-i pi (n+1/4)/M) FFT( (X[2m] + i X[M-1-2m]) exp(-i pi m/M) )[n] */
    const float (*tw)[2] = M == 1024 ? tw_l : tw_s, (*pre)[2] = M == 1024 ? pre_l : pre_s;
    int h = M / 2;
    for (int k = 0; k < h; k++) {
        float re = X[2 * k], im = X[M - 1 - 2 * k];
        z[k][0] = re * pre[k][0] - im * pre[k][1]; z[k][1] = re * pre[k][1] + im * pre[k][0];
    }
    fft(z, h, M == 1024 ? fft_l : fft_s);
    for (int n = 0; n < h; n++) {
        float re = z[n][0] * tw[n][0] - z[n][1] * tw[n][1], im = z[n][0] * tw[n][1] + z[n][1] * tw[n][0];
        y[2 * n] = re; y[M - 1 - 2 * n] = -im;
    }
}
/* IMDCT, N = 2M outputs (14496-3 4.6.11.3.1, 2/N scaling), from the DCT-IV by symmetry */
static void imdct(const float *X, float *out, int M) {
    static float y[1024];
    dct4(X, y, M);
    float sc = 2.0f / (2 * M);   /* the standard's 2/N */
    for (int i = 0; i < 2 * M; i++) {
        int t = i + M / 2;
        float v = t < M ? y[t] : t < 2 * M ? -y[2 * M - 1 - t] : -y[t - 2 * M];
        out[i] = v * sc;
    }
}

/* ---- bits ---- */
struct bits { const u8 *p; u32 len, pos; };
static u32 gb(struct bits *b, int n) { u32 v = 0; while (n--) { u32 q = b->pos++; v = v << 1 | (q < b->len ? (b->p[q >> 3] >> (7 - (q & 7))) & 1 : 0); } return v; }
static int over(const struct bits *b) { return b->pos > b->len; }

static int huff_sf(struct bits *b) {
    u32 c = 0; int l = 0;
    for (int i = 0; i < 121; i++) { while (l < hcb_sf[i][0]) { c = c << 1 | gb(b, 1); l++; } if ((u32)hcb_sf[i][1] == c) return hcb_sf[i][2]; }
    return -1;
}
/* one codeword of spectral book cb (1..11): values into v (4 or 2), signs and escapes applied */
static int huff_spec(struct bits *b, int cb, int *v) {
    static const int *const books[11] = {&hcb1[0][0], &hcb2[0][0], &hcb3[0][0], &hcb4[0][0], &hcb5[0][0], &hcb6[0][0], &hcb7[0][0], &hcb8[0][0], &hcb9[0][0], &hcb10[0][0], &hcb11[0][0]};
    static const int sizes[11] = {81, 81, 81, 81, 81, 81, 64, 64, 169, 169, 289};
    int dims = cb <= 4 ? 4 : 2, w = 2 + dims, unsig = cb == 3 || cb == 4 || cb >= 7;
    const int *t = books[cb - 1];
    u32 c = 0; int l = 0, hit = -1;
    for (int i = 0; i < sizes[cb - 1]; i++) { while (l < t[i * w]) { c = c << 1 | gb(b, 1); l++; } if ((u32)t[i * w + 1] == c) { hit = i; break; } }
    if (hit < 0) return -1;
    for (int k = 0; k < dims; k++) v[k] = t[hit * w + 2 + k];
    if (unsig) for (int k = 0; k < dims; k++) if (v[k] && gb(b, 1)) v[k] = -v[k];
    if (cb == 11) for (int k = 0; k < 2; k++) if (v[k] == 16 || v[k] == -16) {
        int n = 0; while (gb(b, 1)) if (++n > 8) return -1;
        int e = (int)((1u << (n + 4)) + gb(b, n + 4));
        v[k] = v[k] < 0 ? -e : e;
    }
    return 0;
}

/* ---- one channel's stream ---- */
enum { ONLY_LONG, LONG_START, EIGHT_SHORT, LONG_STOP };
struct ics {
    int seq, shape, max_sfb, ngroups, glen[8], nwin, nswb;
    const unsigned short *swb;
    u8 cb[8][64]; int sf[8][64];
    int q[1024];
    int tns_on, nfilt[8], tlen[8][3], torder[8][3], tdir[8][3]; float tlpc[8][3][21];
};
static const unsigned short *swb_tab(int srix, int sh) {
    static const unsigned short *const L[12] = {swb1024_96, swb1024_96, swb1024_64, swb1024_48, swb1024_48, swb1024_32, swb1024_24, swb1024_24, swb1024_16, swb1024_16, swb1024_16, swb1024_8};
    static const unsigned short *const S[12] = {swb128_96, swb128_96, swb128_64, swb128_48, swb128_48, swb128_48, swb128_24, swb128_24, swb128_16, swb128_16, swb128_16, swb128_8};
    return sh ? S[srix] : L[srix];
}
static const u8 nswb_l[12] = {41, 41, 47, 49, 49, 51, 47, 47, 43, 43, 43, 40}, nswb_s[12] = {12, 12, 12, 14, 14, 14, 15, 15, 15, 15, 15, 15};

static int ics_info(struct aac *a, struct bits *b, struct ics *c) {
    gb(b, 1);
    c->seq = (int)gb(b, 2); c->shape = (int)gb(b, 1);
    if (c->seq == EIGHT_SHORT) {
        c->max_sfb = (int)gb(b, 4); u32 g = gb(b, 7);
        c->ngroups = 1; c->glen[0] = 1;
        for (int i = 6; i >= 0; i--) { if (g >> i & 1) c->glen[c->ngroups - 1]++; else c->glen[c->ngroups++] = 1; }
        c->nwin = 8; c->swb = swb_tab(a->srix, 1); c->nswb = nswb_s[a->srix];
    } else {
        c->max_sfb = (int)gb(b, 6);
        if (gb(b, 1)) { a->err = "AAC Main prediction is not supported"; return -1; }
        c->ngroups = 1; c->glen[0] = 1; c->nwin = 1; c->swb = swb_tab(a->srix, 0); c->nswb = nswb_l[a->srix];
    }
    if (c->max_sfb > c->nswb) { a->err = "bad max_sfb"; return -1; }
    return 0;
}

static int ics(struct aac *a, struct bits *b, struct ics *c, int common) {
    int gain = (int)gb(b, 8);
    if (!common && ics_info(a, b, c) < 0) return -1;
    /* section data */
    int sbits = c->nwin == 8 ? 3 : 5, esc = (1 << sbits) - 1;
    for (int g = 0; g < c->ngroups; g++) {
        for (int k = 0; k < c->max_sfb;) {
            int cb = (int)gb(b, 4), len = 0, inc;
            if (cb == 12) { a->err = "bad codebook"; return -1; }
            while ((inc = (int)gb(b, sbits)) == esc) { len += esc; if (over(b)) return -1; }
            len += inc;
            if (!len || k + len > c->max_sfb) { a->err = "bad section"; return -1; }
            for (int i = 0; i < len; i++) c->cb[g][k + i] = (u8)cb;
            k += len;
        }
        for (int k = c->max_sfb; k < 64; k++) c->cb[g][k] = 0;
    }
    /* scale factors */
    int sf = gain, is = 0, noise = gain - 90, first_noise = 1;
    for (int g = 0; g < c->ngroups; g++) for (int k = 0; k < c->max_sfb; k++) {
        int cb = c->cb[g][k];
        if (!cb) c->sf[g][k] = 0;
        else if (cb == 14 || cb == 15) { int d = huff_sf(b); if (d < 0) return -1; is += d - 60; c->sf[g][k] = is; }
        else if (cb == 13) {
            if (first_noise) { noise += (int)gb(b, 9) - 256; first_noise = 0; }
            else { int d = huff_sf(b); if (d < 0) return -1; noise += d - 60; }
            c->sf[g][k] = noise;
        } else { int d = huff_sf(b); if (d < 0) return -1; sf += d - 60; c->sf[g][k] = sf; }
    }
    /* pulses, TNS, gain control */
    int pulse = (int)gb(b, 1), pstart = 0, np = 0, poff[4] = {0}, pamp[4] = {0};
    if (pulse) {
        if (c->nwin == 8) { a->err = "pulses in a short window"; return -1; }
        np = (int)gb(b, 2) + 1; pstart = (int)gb(b, 6);
        for (int i = 0; i < np; i++) { poff[i] = (int)gb(b, 5); pamp[i] = (int)gb(b, 4); }
    }
    c->tns_on = (int)gb(b, 1);
    if (c->tns_on) for (int w = 0; w < c->nwin; w++) {
        int sh = c->nwin == 8;
        c->nfilt[w] = (int)gb(b, sh ? 1 : 2);
        int res = c->nfilt[w] ? (int)gb(b, 1) : 0;
        for (int f = 0; f < c->nfilt[w]; f++) {
            c->tlen[w][f] = (int)gb(b, sh ? 4 : 6);
            int order = c->torder[w][f] = (int)gb(b, sh ? 3 : 5);
            if (!order) continue;
            c->tdir[w][f] = (int)gb(b, 1);
            int compress = (int)gb(b, 1), bits = res + 3 - compress;
            float par[20];
            for (int i = 0; i < order; i++) {
                int v = (int)gb(b, bits);
                if (v & (1 << (bits - 1))) v -= 1 << bits;   /* two's complement */
                double iqf = v >= 0 ? ((1 << (res + 2)) - 0.5) / (PI / 2) : ((1 << (res + 2)) + 0.5) / (PI / 2);
                par[i] = (float)msin(v / iqf);
            }
            float *lp = c->tlpc[w][f], tmp[21];   /* parcor to LPC, the step-up recursion */
            lp[0] = 1;
            for (int m = 1; m <= order; m++) {
                for (int i = 1; i < m; i++) tmp[i] = lp[i] + par[m - 1] * lp[m - i];
                for (int i = 1; i < m; i++) lp[i] = tmp[i];
                lp[m] = par[m - 1];
            }
        }
    }
    if (gb(b, 1)) { a->err = "gain control is not AAC-LC"; return -1; }
    /* spectral data */
    for (int i = 0; i < 1024; i++) c->q[i] = 0;
    int win0 = 0;
    for (int g = 0; g < c->ngroups; g++) {
        for (int k = 0; k < c->max_sfb; k++) {
            int cb = c->cb[g][k], lo = c->swb[k], hi = c->swb[k + 1];
            for (int w = 0; w < c->glen[g]; w++) {
                int base = (win0 + w) * 128 * (c->nwin == 8) ;
                if (cb == 0 || cb >= 13) continue;
                int dims = cb <= 4 ? 4 : 2, v[4];
                for (int i = lo; i < hi; i += dims) {
                    if (huff_spec(b, cb, v) < 0) { a->err = "bad spectral codeword"; return -1; }
                    for (int j = 0; j < dims; j++) c->q[base + i + j] = v[j];
                }
            }
        }
        win0 += c->glen[g];
    }
    if (pulse) {
        int k = c->swb[pstart];
        for (int i = 0; i < np; i++) { k += poff[i]; if (k >= 1024) break; c->q[k] += c->q[k] < 0 ? -pamp[i] : pamp[i]; }
    }
    return over(b) ? -1 : 0;
}

static unsigned rng_state = 0x1f2e3d4cu;
/* quantised values to spectrum (dequantisation, noise bands) */
static void dequant(struct ics *c, float *x) {
    static const float p4[4] = {1.0f, 1.18920711500272f, 1.41421356237310f, 1.68179283050743f};
    for (int i = 0; i < 1024; i++) x[i] = 0;
    int win0 = 0;
    for (int g = 0; g < c->ngroups; g++) {
        for (int k = 0; k < c->max_sfb; k++) {
            int cb = c->cb[g][k], lo = c->swb[k], hi = c->swb[k + 1];
            if (!cb || cb == 14 || cb == 15) continue;
            int e = c->sf[g][k] - 100;
            float sc = p4[e & 3];
            for (int s = e >> 2; s > 0; s--) sc *= 2;
            for (int s = e >> 2; s < 0; s++) sc *= 0.5f;
            for (int w = 0; w < c->glen[g]; w++) {
                float *o = x + (win0 + w) * 128 * (c->nwin == 8);
                if (cb == 13) {   /* perceptual noise: random values with the band's energy */
                    float en = 0;
                    for (int i = lo; i < hi; i++) { rng_state = rng_state * 1664525u + 1013904223u; o[i] = (float)(int)rng_state; en += o[i] * o[i]; }
                    float nsc = 1.0f;
                    int ne = c->sf[g][k];   /* 2^(0.25 * nrg) / sqrt(energy) */
                    float t = p4[ne & 3]; for (int s = ne >> 2; s > 0; s--) t *= 2; for (int s = ne >> 2; s < 0; s++) t *= 0.5f;
                    nsc = t / (float)msqrt(en > 0 ? en : 1);
                    for (int i = lo; i < hi; i++) o[i] *= nsc;
                    continue;
                }
                for (int i = lo; i < hi; i++) {
                    int q = c->q[(win0 + w) * 128 * (c->nwin == 8) + i], aq = q < 0 ? -q : q;
                    float v = aq < 8192 ? iq[aq] : (float)(aq * cbrt_(aq));
                    o[i] = (q < 0 ? -v : v) * sc;
                }
            }
        }
        win0 += c->glen[g];
    }
}

static void tns(struct aac *a, struct ics *c, float *x) {
    if (!c->tns_on) return;
    int sh = c->nwin == 8, mx = sh ? tns_max_128[a->srix] : tns_max_1024[a->srix];
    for (int w = 0; w < c->nwin; w++) {
        float *s = x + w * 128 * sh;
        int bottom = c->nswb;
        for (int f = 0; f < c->nfilt[w]; f++) {
            int top = bottom; bottom = top - c->tlen[w][f]; if (bottom < 0) bottom = 0;
            int order = c->torder[w][f];
            if (!order) continue;
            int lo = bottom < mx ? bottom : mx; lo = lo < c->max_sfb ? lo : c->max_sfb;
            int hi = top < mx ? top : mx; hi = hi < c->max_sfb ? hi : c->max_sfb;
            int start = c->swb[lo], end = c->swb[hi], size = end - start;
            if (size <= 0) continue;
            int inc = c->tdir[w][f] ? -1 : 1, k = c->tdir[w][f] ? end - 1 : start;
            float st[20] = {0}; const float *lp = c->tlpc[w][f];
            for (int n = 0; n < size; n++, k += inc) {
                float y = s[k];
                for (int j = 0; j < order; j++) y -= st[j] * lp[j + 1];
                for (int j = order - 1; j > 0; j--) st[j] = st[j - 1];
                st[0] = y; s[k] = y;
            }
        }
    }
}

/* the filterbank: windows, IMDCT, overlap-add */
static void filterbank(struct aac *a, int ch, struct ics *c, const float *x, float *out) {
    static float buf[2048], t[256];
    const float *lp = a->prev_shape[ch] ? win_kbd_l : win_sin_l, *lc = c->shape ? win_kbd_l : win_sin_l;
    const float *sp = a->prev_shape[ch] ? win_kbd_s : win_sin_s, *sc = c->shape ? win_kbd_s : win_sin_s;
    float *ov = a->overlap[ch];
    if (c->seq == EIGHT_SHORT) {
        for (int i = 0; i < 2048; i++) buf[i] = 0;
        for (int w = 0; w < 8; w++) {
            imdct(x + w * 128, t, 128);
            const float *r = w ? sc : sp;
            for (int i = 0; i < 128; i++) { buf[448 + w * 128 + i] += t[i] * r[i]; buf[448 + w * 128 + 128 + i] += t[128 + i] * sc[127 - i]; }
        }
    } else {
        imdct(x, buf, 1024);
        for (int i = 0; i < 1024; i++) {
            float wr = c->seq == LONG_STOP ? (i < 448 ? 0 : i < 576 ? sp[i - 448] : 1) : lp[i];
            buf[i] *= wr;
        }
        for (int i = 0; i < 1024; i++) {
            float wf = c->seq == LONG_START ? (i < 448 ? 1 : i < 576 ? sc[127 - (i - 448)] : 0) : lc[1023 - i];
            buf[1024 + i] *= wf;
        }
    }
    for (int i = 0; i < 1024; i++) { out[i] = buf[i] + ov[i]; ov[i] = buf[1024 + i]; }
    a->prev_shape[ch] = c->shape;
}

int aac_init(struct aac *a, const unsigned char *asc, unsigned len) {
    for (unsigned i = 0; i < sizeof *a; i++) ((u8 *)a)[i] = 0;
    if (!ready) setup();
    if (len < 2) return -1;
    int ot = asc[0] >> 3, sr = (asc[0] & 7) << 1 | asc[1] >> 7, ch = (asc[1] >> 3) & 15;
    static const int rates[12] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000};
    if (ot != 2 || sr > 11 || ch < 1 || ch > 2) { a->err = "only AAC-LC, mono or stereo"; return -1; }
    a->srix = sr; a->rate = rates[sr]; a->channels = ch;
    return 0;
}

int aac_frame(struct aac *a, const unsigned char *data, unsigned len, short *out) {
    static struct ics c[2]; static float pcm[2][1024];
    struct bits b = {data, len * 8, 0};
    int nch = 0;
    for (;;) {
        int id = (int)gb(&b, 3);
        if (over(&b)) break;
        if (id == 7) break;   /* END */
        if (id == 0 || id == 3) {   /* SCE, LFE */
            gb(&b, 4);
            if (nch >= a->channels || ics(a, &b, &c[nch], 0) < 0) return -1;
            dequant(&c[nch], a->spec[nch]); tns(a, &c[nch], a->spec[nch]);
            filterbank(a, nch, &c[nch], a->spec[nch], pcm[nch]); nch++;
        } else if (id == 1) {   /* CPE */
            gb(&b, 4);
            if (nch + 2 > 2) return -1;
            int common = (int)gb(&b, 1), ms = 0; u8 msu[8][64] = {{0}};
            if (common) {
                if (ics_info(a, &b, &c[0]) < 0) return -1;
                c[1].seq = c[0].seq; c[1].shape = c[0].shape; c[1].max_sfb = c[0].max_sfb; c[1].ngroups = c[0].ngroups;
                for (int g = 0; g < 8; g++) c[1].glen[g] = c[0].glen[g];
                c[1].nwin = c[0].nwin; c[1].swb = c[0].swb; c[1].nswb = c[0].nswb;
                ms = (int)gb(&b, 2);
                if (ms == 1) { for (int g = 0; g < c[0].ngroups; g++) for (int k = 0; k < c[0].max_sfb; k++) msu[g][k] = (u8)gb(&b, 1); }
                else if (ms == 2) for (int g = 0; g < 8; g++) for (int k = 0; k < 64; k++) msu[g][k] = 1;
            }
            if (ics(a, &b, &c[0], common) < 0 || ics(a, &b, &c[1], common) < 0) return -1;
            dequant(&c[0], a->spec[0]); dequant(&c[1], a->spec[1]);
            /* M/S, then intensity, on the spectrum before TNS (14496-3 4.6.8, 4.6.9) */
            int win0 = 0;
            for (int g = 0; g < c[1].ngroups; g++) {
                for (int k = 0; k < c[1].max_sfb; k++) {
                    int lo = c[1].swb[k], hi = c[1].swb[k + 1], cbr = c[1].cb[g][k];
                    for (int w = 0; w < c[1].glen[g]; w++) {
                        float *l = a->spec[0] + (win0 + w) * 128 * (c[1].nwin == 8), *r = a->spec[1] + (win0 + w) * 128 * (c[1].nwin == 8);
                        if (cbr == 14 || cbr == 15) {
                            int e = -c[1].sf[g][k];   /* 0.5^(0.25 * is_position) */
                            float sc = 1; static const float p4[4] = {1.0f, 1.18920711500272f, 1.41421356237310f, 1.68179283050743f};
                            sc = p4[e & 3]; for (int s = e >> 2; s > 0; s--) sc *= 2; for (int s = e >> 2; s < 0; s++) sc *= 0.5f;
                            int sign = cbr == 15 ? 1 : -1; if (ms == 1 && msu[g][k]) sign = -sign;
                            for (int i = lo; i < hi; i++) r[i] = l[i] * sc * (float)sign;
                        } else if (msu[g][k] && cbr != 13 && c[0].cb[g][k] != 13) {
                            for (int i = lo; i < hi; i++) { float m = l[i], s = r[i]; l[i] = m + s; r[i] = m - s; }
                        }
                    }
                }
                win0 += c[1].glen[g];
            }
            for (int ch = 0; ch < 2; ch++) { tns(a, &c[ch], a->spec[ch]); filterbank(a, ch, &c[ch], a->spec[ch], pcm[ch]); }
            nch = 2;
        } else if (id == 4) {   /* DSE: skip */
            gb(&b, 4); int align = (int)gb(&b, 1), cnt = (int)gb(&b, 8);
            if (cnt == 255) cnt += (int)gb(&b, 8);
            if (align) b.pos = (b.pos + 7) & ~7u;
            b.pos += (u32)cnt * 8;
        } else if (id == 6) {   /* FIL: skip */
            int cnt = (int)gb(&b, 4);
            if (cnt == 15) cnt += (int)gb(&b, 8) - 1;
            b.pos += (u32)cnt * 8;
        } else { a->err = "unsupported AAC element"; return -1; }
    }
    if (nch < a->channels) { a->err = "no audio in the frame"; return -1; }
    for (int i = 0; i < 1024; i++) for (int ch = 0; ch < a->channels; ch++) {
        float v = pcm[ch][i];
        int s = (int)(v < 0 ? v - 0.5f : v + 0.5f);
        out[i * a->channels + ch] = (short)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
    return 1024;
}
