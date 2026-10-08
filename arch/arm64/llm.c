/* A small language model in the kernel: Karpathy's llama2.c (run.c) forward pass, float32, no network. The checkpoint
   and tokenizer are llama2.c's own file formats (model.bin from export.py, tokenizer.bin), baked into the image by
   llm_model.S when the build is given LLM_MODEL= and LLM_TOK=; with neither, `llm` says there is no model.

   Built with FPCC like text.c: this is the only other file that uses floating point, and the interface below is
   integers only, so ask.c (built -mgeneral-regs-only) can call it. Every buffer comes from the bump heap between
   heap_mark and heap_release, so a run leaves the heap as it found it. The weights stay where the image put them.

   ponytail: greedy sampling only (argmax, no temperature or top-p): the same prompt always gives the same text, which
   is what tools/checks/arm64-llm-check.py compares against its own reference. Add a sampler when someone wants variety. */

void *kmalloc(unsigned int n);
unsigned long heap_mark(void);
void heap_release(unsigned long m);

extern const unsigned char llm_model_start[], llm_model_end[], llm_tok_start[], llm_tok_end[];   /* llm_model.S */

#define LLM_STEPS 128   /* positions per run, prompt included, capped by the model's seq_len */

static int rd32(const unsigned char *p) { return (int)((unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24); }
static float rdf(const unsigned char *p) { union { unsigned u; float f; } v; v.u = (unsigned)rd32(p); return v.f; }

/* e^x in double: x = n ln2 + r with |r| <= ln2/2, a Taylor series for e^r, then 2^n into the exponent bits. */
static double dexp(double x) {
    if (x > 700) x = 700;
    if (x < -700) return 0;
    double fn = x * 1.4426950408889634;
    long n = (long)(fn < 0 ? fn - 0.5 : fn + 0.5);
    double r = x - (double)n * 0.6931471805599453, t = 1, s = 1;
    for (int k = 1; k <= 13; k++) { t *= r / k; s += t; }
    union { unsigned long u; double d; } v; v.u = (unsigned long)(n + 1023) << 52;
    return s * v.d;
}
/* sin and cos of x in double: fold x into [-pi, pi], then Taylor series (error under 1e-8 there). */
static void dsincos(double x, double *sn, double *cs) {
    const double twopi = 6.283185307179586;
    double k = x / twopi; long n = (long)(k < 0 ? k - 0.5 : k + 0.5);
    double r = x - (double)n * twopi, r2 = r * r, ts = r, tc = 1, s = r, c = 1;
    for (int i = 1; i <= 10; i++) {
        ts *= -r2 / ((2 * i) * (2 * i + 1)); s += ts;
        tc *= -r2 / ((2 * i - 1) * (2 * i)); c += tc;
    }
    *sn = s; *cs = c;
}

struct cfg { int dim, hidden, layers, heads, kv_heads, vocab, seq; };
struct w {
    const float *emb, *rms_att, *wq, *wk, *wv, *wo, *rms_ffn, *w1, *w2, *w3, *rms_final, *wcls;
};

static void rmsnorm(float *o, const float *x, const float *w, int n) {
    float ss = 0;
    for (int j = 0; j < n; j++) ss += x[j] * x[j];
    ss = 1.0f / __builtin_sqrtf(ss / n + 1e-5f);
    for (int j = 0; j < n; j++) o[j] = w[j] * (ss * x[j]);
}
static void matmul(float *o, const float *x, const float *w, int n, int d) {   /* W (d,n) @ x (n,) -> o (d,) */
    for (int i = 0; i < d; i++) {
        float v = 0; const float *r = w + (unsigned long)i * n;
        for (int j = 0; j < n; j++) v += r[j] * x[j];
        o[i] = v;
    }
}
static void softmax(float *x, int n) {
    float m = x[0], sum = 0;
    for (int i = 1; i < n; i++) if (x[i] > m) m = x[i];
    for (int i = 0; i < n; i++) { x[i] = (float)dexp(x[i] - m); sum += x[i]; }
    for (int i = 0; i < n; i++) x[i] /= sum;
}

static struct cfg c;
static struct w W;
static float *x, *xb, *xb2, *hb, *hb2, *q, *kc, *vc, *att, *logits;

static float *forward(int token, int pos) {
    int dim = c.dim, kv_dim = c.dim * c.kv_heads / c.heads, kv_mul = c.heads / c.kv_heads, hs = c.dim / c.heads;
    for (int i = 0; i < dim; i++) x[i] = W.emb[(unsigned long)token * dim + i];
    for (int l = 0; l < c.layers; l++) {
        unsigned long loff = (unsigned long)l * c.seq * kv_dim;
        float *k = kc + loff + (unsigned long)pos * kv_dim, *v = vc + loff + (unsigned long)pos * kv_dim;
        rmsnorm(xb, x, W.rms_att + (unsigned long)l * dim, dim);
        matmul(q, xb, W.wq + (unsigned long)l * dim * dim, dim, dim);
        matmul(k, xb, W.wk + (unsigned long)l * dim * kv_dim, dim, kv_dim);
        matmul(v, xb, W.wv + (unsigned long)l * dim * kv_dim, dim, kv_dim);
#ifndef LLM_BREAK   /* the check's control build leaves out the rotary position step and must then disagree */
        for (int i = 0; i < dim; i += 2) {   /* RoPE: rotate each pair of q (and k) by an angle that grows with pos */
            double sn, cs;
            dsincos(pos * dexp(-(double)(i % hs) / hs * 9.210340371976184), &sn, &cs);   /* pos / 10000^(i/hs) */
            float fr = (float)cs, fi = (float)sn;
            for (int t = 0; t < (i < kv_dim ? 2 : 1); t++) {
                float *vec = t ? k : q, a = vec[i], b = vec[i + 1];
                vec[i] = a * fr - b * fi; vec[i + 1] = a * fi + b * fr;
            }
        }
#endif
        for (int h = 0; h < c.heads; h++) {
            float *qh = q + h * hs, *a = att + (unsigned long)h * c.seq, *o = xb + h * hs;
            for (int t = 0; t <= pos; t++) {
                const float *kt = kc + loff + (unsigned long)t * kv_dim + (h / kv_mul) * hs;
                float s = 0;
                for (int i = 0; i < hs; i++) s += qh[i] * kt[i];
                a[t] = s / __builtin_sqrtf((float)hs);
            }
            softmax(a, pos + 1);
            for (int i = 0; i < hs; i++) o[i] = 0;
            for (int t = 0; t <= pos; t++) {
                const float *vt = vc + loff + (unsigned long)t * kv_dim + (h / kv_mul) * hs;
                for (int i = 0; i < hs; i++) o[i] += a[t] * vt[i];
            }
        }
        matmul(xb2, xb, W.wo + (unsigned long)l * dim * dim, dim, dim);
        for (int i = 0; i < dim; i++) x[i] += xb2[i];
        rmsnorm(xb, x, W.rms_ffn + (unsigned long)l * dim, dim);
        matmul(hb, xb, W.w1 + (unsigned long)l * dim * c.hidden, dim, c.hidden);
        matmul(hb2, xb, W.w3 + (unsigned long)l * dim * c.hidden, dim, c.hidden);
        for (int i = 0; i < c.hidden; i++) { float v2 = hb[i]; hb[i] = v2 / (1.0f + (float)dexp(-v2)) * hb2[i]; }   /* SwiGLU */
        matmul(xb, hb, W.w2 + (unsigned long)l * dim * c.hidden, c.hidden, dim);
        for (int i = 0; i < dim; i++) x[i] += xb[i];
    }
    rmsnorm(x, x, W.rms_final, dim);
    matmul(logits, x, W.wcls, dim, c.vocab);
    return logits;
}

/* The tokenizer: pointers into tokenizer.bin, not copies. */
static const unsigned char **voc;
static int *vlen;
static float *vscore;

static int lookup(const char *s, int n) {   /* ponytail: linear scan; a 32000-token vocab and a long prompt take a while. Sort plus binary search if it shows. */
    for (int i = 0; i < c.vocab; i++) {
        if (vlen[i] != n) continue;
        int j = 0; while (j < n && voc[i][j] == (unsigned char)s[j]) j++;
        if (j == n) return i;
    }
    return -1;
}

/* llama2.c's encode(): BOS, the dummy-prefix space, one token per UTF-8 character (raw bytes + 3 when a character is
   not in the vocab), then merge the best-scoring adjacent pair until none is left. Returns the token count. */
static int encode(const char *text, unsigned n, int *tok, char *buf) {
    int nt = 0, bl = 0;
    tok[nt++] = 1;
    if (n) { int sp = lookup(" ", 1); if (sp >= 0) tok[nt++] = sp; }
    for (unsigned i = 0; i < n; i++) {
        buf[bl++] = text[i];
        if (i + 1 < n && ((unsigned char)text[i + 1] & 0xC0) == 0x80 && bl < 4) continue;
        int id = lookup(buf, bl);
        if (id >= 0) tok[nt++] = id;
        else for (int k = 0; k < bl; k++) tok[nt++] = (unsigned char)buf[k] + 3;
        bl = 0;
    }
    for (;;) {
        float best = -1e10f; int bi = -1, bid = -1;
        for (int i = 0; i + 1 < nt; i++) {
            int a = tok[i], b = tok[i + 1], m = 0;
            for (int k = 0; k < vlen[a]; k++) buf[m++] = (char)voc[a][k];
            for (int k = 0; k < vlen[b]; k++) buf[m++] = (char)voc[b][k];
            int id = lookup(buf, m);
            if (id >= 0 && vscore[id] > best) { best = vscore[id]; bi = i; bid = id; }
        }
        if (bi < 0) break;
        tok[bi] = bid;
        for (int i = bi + 1; i + 1 < nt; i++) tok[i] = tok[i + 1];
        nt--;
    }
    return nt;
}

static int hexv(unsigned char ch) {
    return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
}

static int load(void) {   /* 0 ok, -1 no model, -2 out of memory, -3 the files do not fit together */
    unsigned long msz = (unsigned long)(llm_model_end - llm_model_start), tsz = (unsigned long)(llm_tok_end - llm_tok_start);
    if (msz < 28 || tsz < 4) return -1;
    const unsigned char *m = llm_model_start;
    c.dim = rd32(m); c.hidden = rd32(m + 4); c.layers = rd32(m + 8); c.heads = rd32(m + 12);
    c.kv_heads = rd32(m + 16); c.vocab = rd32(m + 20); c.seq = rd32(m + 24);
    int shared = c.vocab > 0;
    if (c.vocab < 0) c.vocab = -c.vocab;
    if (c.dim <= 0 || c.heads <= 0 || c.kv_heads <= 0 || c.dim % c.heads || c.heads % c.kv_heads || c.layers <= 0 ||
        c.hidden <= 0 || c.vocab < 259 || c.seq <= 1) return -3;
    if (((unsigned long)m & 3) != 0) return -3;
    unsigned long dim = c.dim, kv = dim * c.kv_heads / c.heads, L = c.layers, hid = c.hidden, hs = dim / c.heads;
    const float *p = (const float *)(m + 28);
    W.emb = p; p += c.vocab * dim;
    W.rms_att = p; p += L * dim;
    W.wq = p; p += L * dim * dim;
    W.wk = p; p += L * dim * kv;
    W.wv = p; p += L * dim * kv;
    W.wo = p; p += L * dim * dim;
    W.rms_ffn = p; p += L * dim;
    W.w1 = p; p += L * dim * hid;
    W.w2 = p; p += L * hid * dim;
    W.w3 = p; p += L * dim * hid;
    W.rms_final = p; p += dim;
    p += c.seq * hs;   /* the old freq_cis tables, unused: RoPE is computed */
    W.wcls = shared ? W.emb : p;
    if (!shared) p += c.vocab * dim;
    if ((const unsigned char *)p > llm_model_end) return -3;

    unsigned long seq = c.seq;
    x = kmalloc(dim * 4); xb = kmalloc(dim * 4); xb2 = kmalloc(dim * 4); q = kmalloc(dim * 4);
    hb = kmalloc(hid * 4); hb2 = kmalloc(hid * 4);
    kc = kmalloc(L * seq * kv * 4); vc = kmalloc(L * seq * kv * 4);
    att = kmalloc(c.heads * seq * 4); logits = kmalloc(c.vocab * 4);
    voc = kmalloc(c.vocab * sizeof *voc); vlen = kmalloc(c.vocab * 4); vscore = kmalloc(c.vocab * 4);
    if (!x || !xb || !xb2 || !q || !hb || !hb2 || !kc || !vc || !att || !logits || !voc || !vlen || !vscore) return -2;

    const unsigned char *t = llm_tok_start + 4, *te = llm_tok_end;   /* max_token_length, then (score, len, bytes) each */
    for (int i = 0; i < c.vocab; i++) {
        if (t + 8 > te) return -3;
        vscore[i] = rdf(t); vlen[i] = rd32(t + 4); voc[i] = t + 8;
        if (vlen[i] < 0 || vlen[i] > 64 || t + 8 + vlen[i] > te) return -3;
        t += 8 + vlen[i];
    }
    return 0;
}

/* Runs the model on a prompt and writes the text (the prompt, then what it generated) to out.
   Returns the length written, or -1 no model, -2 out of memory, -3 bad model files, -4 prompt too long.
   *npos is how many positions ran, *tps10 the speed in tenths of a token per second, from the generic counter. */
int llm_generate(const char *prompt, unsigned n, char *out, unsigned cap, unsigned *npos, unsigned *tps10) {
    unsigned long mark = heap_mark();
    int r = load(), o = 0;
    *npos = 0; *tps10 = 0;
    if (r) { heap_release(mark); return r; }
    int steps = c.seq < LLM_STEPS ? c.seq : LLM_STEPS;
    int *tok = kmalloc((n + 3) * 4);
    char *buf = kmalloc(160);
    if (!tok || !buf) { heap_release(mark); return -2; }
    int nt = encode(prompt, n, tok, buf);
    if (nt >= steps) { heap_release(mark); return -4; }

    unsigned long f, t0, t1;
    __asm__ volatile ("mrs %0, cntfrq_el0\n mrs %1, cntpct_el0" : "=r"(f), "=r"(t0));
    int token = tok[0], pos = 0;
    while (pos < steps) {
        float *lg = forward(token, pos);
        int next;
        if (pos < nt - 1) next = tok[pos + 1];
        else { next = 0; for (int i = 1; i < c.vocab; i++) if (lg[i] > lg[next]) next = i; }
        pos++;
        if (next == 1) break;   /* BOS: the model ended the story */
        const unsigned char *pc = voc[next]; int pl = vlen[next];
        if (token == 1 && pl && pc[0] == ' ') { pc++; pl--; }
        if (pl == 6 && pc[0] == '<' && pc[1] == '0' && pc[2] == 'x' && pc[5] == '>' && hexv(pc[3]) >= 0 && hexv(pc[4]) >= 0) {
            if ((unsigned)o < cap) out[o++] = (char)(hexv(pc[3]) * 16 + hexv(pc[4]));
        } else for (int i = 0; i < pl && (unsigned)o < cap; i++) out[o++] = (char)pc[i];
        token = next;
    }
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(t1));
    unsigned long dt = t1 - t0;
    *npos = (unsigned)pos;
    *tps10 = dt ? (unsigned)((unsigned long)pos * f * 10 / dt) : 0;
    heap_release(mark);
    return o;
}
