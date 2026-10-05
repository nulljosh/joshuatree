/* Hamurabi: rule Sumeria for 10 years. Header-only C implementation.
 * Mirrors hamurabi.py, web/play/rules.js, and app/App/Game.swift.
 * No libc, no malloc, no floats, no 64-bit division/modulo.
 * SplitMix64, rules, check/step/grade, the robot Ruler, and story logic.
 * `tools/checks/hamurabi-rules-check.sh` holds it to the other ports: two golden numbers
 * (classic 639940, story 390695) and an A+ count of exactly 878 in 1000 reigns.
 */
#ifndef HAMURABI_RULES_H
#define HAMURABI_RULES_H

#include "hamurabi_story.h"

#define HAMURABI_YEARS 10
#define HAMURABI_FOOD 20  /* bushels per person per year */
#define HAMURABI_TEND 10  /* acres one person can tend */

/* Minimal memset to avoid libgcc dependency in freestanding build */
static inline void *hamurabi_memset(void *s, int c, unsigned long n) {
    unsigned char *p = (unsigned char *)s;
    for (unsigned long i = 0; i < n; i++) p[i] = (unsigned char)c;
    return s;
}

/* SplitMix64: 64-bit state PRNG, same seed = same sequence on every platform */
typedef struct {
    unsigned long long x;
} hamurabi_rng;

static hamurabi_rng hamurabi_rng_new(unsigned long long seed) {
    hamurabi_rng r;
    r.x = seed;
    return r;
}

static unsigned long long hamurabi_rng_next(hamurabi_rng *r) {
    r->x += 0x9E3779B97F4A7C15ULL;
    unsigned long long z = r->x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/* n % d for a 64-bit n and a 32-bit d, in 32-bit maths only: shift the bits of n in one at a time,
   subtracting d whenever the remainder reaches it. Exact for every d up to 2^31, so no __umoddi3. */
static unsigned hamurabi_mod64_u32(unsigned long long n, unsigned d) {
    unsigned r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | (unsigned)((n >> i) & 1u);
        if (r >= d) r -= d;
    }
    return r;
}

/* Draw int in [lo, hi] inclusive. One draw is always used, even for a range of one. */
static int hamurabi_rng_int(hamurabi_rng *r, int lo, int hi) {
    return lo + (int)hamurabi_mod64_u32(hamurabi_rng_next(r), (unsigned)(hi - lo + 1));
}

/* True with probability p. The other ports test (draw >> 11) / 2^53 < p for a double p; that is the
   same as (draw >> 11) < ceil(p * 2^53), and thr is that integer, worked out once with exact fractions. */
static int hamurabi_rng_below(hamurabi_rng *r, unsigned long long thr) {
    return (hamurabi_rng_next(r) >> 11) < thr;
}
#define HAMURABI_P25 2251799813685248ULL /* 0.25 */
#define HAMURABI_P40 3602879701896397ULL /* 0.4 */
#define HAMURABI_P45 4053239664633447ULL /* 0.45 */
#define HAMURABI_P50 4503599627370496ULL /* 0.5 */
#define HAMURABI_P15 1351079888211149ULL /* 0.15 */
#define HAMURABI_P70 6305039478318694ULL /* 0.7 */

typedef struct {
    int year;
    int people;
    int grain;
    int acres;
    int price;
    int starved_total;
    int starved_pct;  /* sum of yearly starvation percent, in ten-thousandths of a percent (the reference keeps fractions) */
    int over;         /* 0 = ongoing, 1 = impeached, 2 = term ended */
    int guarded;      /* cats at the barn: this year's rats find nothing */
    int yield_bonus;  /* added to this year's harvest roll, then cleared */
} hamurabi_city;

typedef struct {
    int buy;
    int feed;
    int plant;
} hamurabi_orders;

typedef struct {
    int grain;
    int acres;
    unsigned long long rat_thr; /* chance rats come: HAMURABI_P40 and friends */
    int mercy;                  /* percent: starving more than this share of the people in a year ends the reign */
} hamurabi_rules;

static const hamurabi_rules hamurabi_rules_normal = {
    .grain = 2800, .acres = 1000, .rat_thr = HAMURABI_P40, .mercy = 45
};

static const hamurabi_rules hamurabi_rules_easy = {
    .grain = 3600, .acres = 1200, .rat_thr = HAMURABI_P25, .mercy = 60
};

static const hamurabi_rules hamurabi_rules_hard = {
    .grain = 2400, .acres = 900, .rat_thr = HAMURABI_P50, .mercy = 35
};

static hamurabi_city hamurabi_new_city(const hamurabi_rules *r) {
    hamurabi_city c = {0};
    c.year = 1;
    c.people = 100;
    c.grain = r->grain;
    c.acres = r->acres;
    c.price = 19;
    return c;
}

/* Check if orders are legal. Return error code: 0 = ok, nonzero = error */
static int hamurabi_check(const hamurabi_city *c, const hamurabi_orders *o) {
    int cost = o->buy * c->price;
    if (o->buy < 0 && -o->buy > c->acres) return 1; /* don't own that much */
    if (cost > c->grain) return 2; /* not enough grain to buy */
    int left = c->grain - cost;
    if (o->feed < 0 || o->plant < 0) return 3; /* negative orders */
    if (o->feed > left) return 4; /* not enough grain to feed */
    if (o->plant > c->acres + o->buy) return 5; /* don't own that much land */
    if (o->plant > left - o->feed) return 6; /* not enough grain for seed */
    if (o->plant > c->people * HAMURABI_TEND) return 7; /* not enough people */
    return 0;
}

typedef struct {
    int year;
    int people_before;
    int yield;
    int harvest;
    int rats;
    int starved;
    int born;
    int plague;
} hamurabi_year_report;

/* Play one year. Assumes check() passed. Order of dice is critical. */
static hamurabi_year_report hamurabi_step(hamurabi_city *c, const hamurabi_orders *o,
                                          hamurabi_rng *rng, const hamurabi_rules *r) {
    hamurabi_year_report rep = {0};
    rep.year = c->year;
    rep.people_before = c->people;

    c->acres += o->buy;
    c->grain -= o->buy * c->price + o->feed + o->plant;

    rep.yield = hamurabi_rng_int(rng, 1, 5) + c->yield_bonus;
    c->yield_bonus = 0;
    rep.harvest = o->plant * rep.yield;

    rep.rats = 0;
    if (hamurabi_rng_below(rng, r->rat_thr)) {
        int eaten = c->grain / (hamurabi_rng_int(rng, 0, 1) ? 4 : 2);
        if (!c->guarded) rep.rats = eaten;
    }
    c->guarded = 0;

    c->grain += rep.harvest - rep.rats;

    int fed = o->feed / HAMURABI_FOOD;
    rep.starved = c->people > fed ? c->people - fed : 0;

    /* Check if starved > mercy * people: starved * 100 > people * mercy */
    if (rep.starved * 100 > c->people * r->mercy) {
        c->over = 1;  /* impeached */
    }

    c->starved_total += rep.starved;
    int denom = c->people > 0 ? c->people : 1;
    {   /* starved * 100 / denom with four decimals, in 32-bit maths only (denom stays far under 400000) */
        unsigned whole = (unsigned)rep.starved * 100u / (unsigned)denom;
        unsigned rem   = (unsigned)rep.starved * 100u % (unsigned)denom;
        c->starved_pct += (int)(whole * 10000u + rem * 10000u / (unsigned)denom);
    }
    c->people -= rep.starved;

    rep.born = 0;
    if (rep.starved == 0) {
        int born_roll = hamurabi_rng_int(rng, 1, 5);
        int numerator = born_roll * (20 * c->acres + c->grain);
        int denom = c->people > 0 ? c->people : 1;
        rep.born = (numerator / denom) / 100 + 1;
    }
    c->people += rep.born;

    rep.plague = hamurabi_rng_below(rng, HAMURABI_P15);
    if (rep.plague) c->people = c->people / 2;

    c->year += 1;
    c->price = hamurabi_rng_int(rng, 17, 26);

    if (!c->over && c->year > HAMURABI_YEARS) {
        c->over = 2;  /* term ended */
    }

    return rep;
}

/* Grade: F, C, B, or A+ (returned as 0, 1, 2, 3) */
static int hamurabi_grade(const hamurabi_city *c) {
    int years = c->year - 1;
    if (years < 1) years = 1;
    /* p1 = starved_pct / years, compared as sum > limit * years so nothing is rounded */
    int people = c->people > 0 ? c->people : 1;

    if (c->over == 1 || c->starved_pct > 330000 * years || c->acres < 7 * people) return 0;  /* F */
    if (c->starved_pct > 100000 * years || c->acres < 9 * people) return 1;  /* C */
    if (c->starved_pct > 30000 * years || c->acres < 10 * people) return 2;  /* B */
    return 3;  /* A+ */
}

/* The robot king: orders for the given city state */
typedef struct {
    int gate;    /* 16 */
    int cap;     /* 19 */
    int share;   /* 6 */
} hamurabi_ruler_knobs;

static const hamurabi_ruler_knobs hamurabi_ruler_default = {
    .gate = 16, .cap = 19, .share = 6
};

static hamurabi_orders hamurabi_ruler_orders(const hamurabi_city *c,
                                             const hamurabi_ruler_knobs *knobs) {
    hamurabi_orders o = {0};
    int tend = c->people * HAMURABI_TEND;
    int seed = c->acres < tend ? c->acres : tend;
    int hungry = c->acres < knobs->gate * c->people ? 1 : 0;
    int feed = (c->people - hungry) * HAMURABI_FOOD;

    if (c->year == HAMURABI_YEARS) {
        o.buy = c->grain > feed ? (c->grain - feed) / c->price : 0;
        o.feed = feed < c->grain ? feed : c->grain;
        o.plant = 0;
        return o;
    }

    int short_grain = feed + seed - c->grain;
    if (short_grain > 0) {
        int sell = (short_grain + c->price - 1) / c->price;
        o.buy = -(sell < c->acres - 1 ? sell : c->acres - 1);
    } else if (c->price <= knobs->cap) {
        o.buy = (-short_grain / c->price) / knobs->share;
    }

    o.feed = feed;

    int available = c->acres + o.buy;
    int after_feed = c->grain - o.buy * c->price - o.feed;
    o.plant = available < tend ? available : tend;
    if (o.plant > after_feed) o.plant = after_feed;
    if (o.plant < 0) o.plant = 0;

    return o;
}

static hamurabi_orders hamurabi_ruler_legal(hamurabi_city *c,
                                            const hamurabi_ruler_knobs *knobs) {
    hamurabi_orders o = hamurabi_ruler_orders(c, knobs);
    if (hamurabi_check(c, &o)) {
        o.buy = 0;
        o.feed = c->grain < c->people * HAMURABI_FOOD ? c->grain : c->people * HAMURABI_FOOD;
        o.plant = 0;
    }
    return o;
}

/* ---- The story. The words and numbers live in hamurabi_story.h, generated from art/hamurabi/story.json. ---- */

/* The omen for this year, or NULL. Years 2 to 9, about two years in three, never the same one twice.
   `seen` is a bit per omen. The dice order is the other ports' order: the 0.7 roll, then one draw to pick. */
static const hamurabi_omen *hamurabi_omen_for(const hamurabi_city *c, unsigned seen, hamurabi_rng *srng) {
    if (c->year < 2 || c->year > 9 || !hamurabi_rng_below(srng, HAMURABI_P70)) return 0;
    int open[HAMURABI_OMENS], n = 0;
    for (int i = 0; i < HAMURABI_OMENS; i++) {
        const hamurabi_omen *o = &hamurabi_omens[i];
        if (!(seen & (1u << i)) && c->grain >= o->needs_grain && c->acres >= o->needs_acres && c->people >= o->needs_people)
            open[n++] = i;
    }
    return n ? &hamurabi_omens[open[hamurabi_rng_int(srng, 0, n - 1)]] : 0;
}

/* Whether the city can pay for a choice. A choice that costs no land is never blocked by having none. */
static int hamurabi_can_afford(const hamurabi_choice *ch, const hamurabi_city *c) {
    return c->grain + ch->grain >= 0 && (ch->acres >= 0 || c->acres + ch->acres >= 1);
}

/* What came of a choice: the line to show, and for the star reader the harvest to expect (0 if none). */
typedef struct {
    const char *said;
    int peek_yield;
} hamurabi_outcome;

/* Applies a choice. `harvest` is the year's dice; the star reader looks at a copy, so the real ones do not move. */
static hamurabi_outcome hamurabi_apply_choice(const hamurabi_choice *ch, hamurabi_city *c, hamurabi_rng *srng,
                                              const hamurabi_rng *harvest) {
    hamurabi_outcome out = { ch->then, 0 };
    int pct = c->acres * ch->acres_pct / 100;
    c->grain += ch->grain; if (c->grain < 0) c->grain = 0;
    c->acres += ch->acres + pct; if (c->acres < 1) c->acres = 1;
    c->people += ch->people; if (c->people < 1) c->people = 1;
    if (ch->guard) c->guarded = 1;
    c->yield_bonus += ch->yield_bonus;
    if (ch->has_gamble && hamurabi_rng_below(srng, ch->gamble_thr)) {
        c->acres += ch->gamble_acres; if (c->acres < 1) c->acres = 1;
        out.said = ch->gamble_then;
    }
    if (ch->peek) {
        hamurabi_rng copy = *harvest;
        out.peek_yield = hamurabi_rng_int(&copy, 1, 5) + c->yield_bonus;
    }
    return out;
}

#endif
