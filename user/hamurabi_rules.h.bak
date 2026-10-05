/* Hamurabi: rule Sumeria for 10 years. Header-only C implementation.
 * Mirrors hamurabi.py, web/play/rules.js, and app/App/Game.swift.
 * No libc, no malloc, no floats, no 64-bit division/modulo.
 * SplitMix64, rules, check/step/grade, the robot Ruler, and story logic.
 */
#ifndef HAMURABI_RULES_H
#define HAMURABI_RULES_H

#include <limits.h>

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

/* 64-bit modulo using only 32-bit operations to avoid __umoddi3 */
static unsigned hamurabi_mod64_u32(unsigned long long n, unsigned divisor) {
    unsigned high = (unsigned)(n >> 32);
    unsigned low = (unsigned)(n & 0xFFFFFFFFUL);
    /* Compute (high * 2^32 + low) % divisor using modular arithmetic:
       (high % d * (2^32 % d) + low % d) % d  */
    unsigned mod_high = high % divisor;
    unsigned mod_low = low % divisor;
    /* 2^32 % divisor for the specific divisors used in hamurabi */
    unsigned pow2_mod;
    if (divisor == 2) pow2_mod = 0;       /* 2^32 % 2 = 0 */
    else if (divisor == 5) pow2_mod = 1;  /* 2^32 % 5 = 1 */
    else if (divisor == 10) pow2_mod = 6; /* 2^32 % 10 = 6 */
    else if (divisor == 1000) pow2_mod = 296; /* 2^32 % 1000 = 296 */
    else pow2_mod = 0;  /* shouldn't happen */
    /* Use only 32-bit arithmetic */
    return (mod_high * pow2_mod + mod_low) % divisor;
}

/* Draw int in [lo, hi] inclusive. Uses helper to compute 64-bit modulo without __umoddi3. */
static int hamurabi_rng_int(hamurabi_rng *r, int lo, int hi) {
    unsigned long long n = hamurabi_rng_next(r);
    unsigned range = (unsigned)(hi - lo + 1);
    return lo + (int)hamurabi_mod64_u32(n, range);
}

/* Chance: draw random [0,1) < num/den. Uses precomputed thresholds. */
static int hamurabi_rng_chance(hamurabi_rng *r, unsigned num, unsigned den) {
    unsigned long long n = hamurabi_rng_next(r) >> 11;
    /* Precomputed: num/100 * 9007199254740992, avoiding 64-bit division at runtime */
    if (den == 100) {
        if (num == 40) return n < 3602879701896396ULL;  /* 0.40 * 2^53 */
        if (num == 45) return n < 4053479564833447ULL;  /* 0.45 * 2^53 */
        if (num == 50) return n < 4503599627370496ULL;  /* 0.50 * 2^53 */
        if (num == 25) return n < 2251799813685248ULL;  /* 0.25 * 2^53 */
        if (num == 15) return n < 1351079812083897ULL;  /* 0.15 * 2^53 */
    }
    if (den == 2 && num == 1) return n < 4503599627370496ULL;  /* 0.50 * 2^53 */
    /* Shouldn't reach here for hamurabi game */
    return 0;
}

typedef struct {
    int year;
    int people;
    int grain;
    int acres;
    int price;
    int starved_total;
    int starved_pct;  /* sum of yearly starvation percent, in ten-thousandths of a percent (the reference keeps fractions) */
    int over;         /* 0 = ongoing, 1 = impeached, 2 = term ended */
} hamurabi_city;

typedef struct {
    int buy;
    int feed;
    int plant;
} hamurabi_orders;

typedef struct {
    int grain;
    int acres;
    int rat;      /* 0.4 = 40 (out of 100) */
    int mercy;    /* 0.45 = 45 (out of 100) */
} hamurabi_rules;

static const hamurabi_rules hamurabi_rules_normal = {
    .grain = 2800, .acres = 1000, .rat = 40, .mercy = 45
};

static const hamurabi_rules hamurabi_rules_easy = {
    .grain = 3600, .acres = 1200, .rat = 25, .mercy = 60
};

static const hamurabi_rules hamurabi_rules_hard = {
    .grain = 2400, .acres = 900, .rat = 50, .mercy = 35
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

    rep.yield = hamurabi_rng_int(rng, 1, 5);
    rep.harvest = o->plant * rep.yield;

    if (hamurabi_rng_chance(rng, r->rat, 100)) {
        int divisor = hamurabi_rng_int(rng, 0, 1) ? 4 : 2;
        rep.rats = c->grain / divisor;
    } else {
        rep.rats = 0;
    }

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

    rep.plague = hamurabi_rng_chance(rng, 15, 100);
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

/* Story omens and choices */
typedef struct {
    const char *id;
    int needs_grain;
    int needs_acres;
    int needs_people;
} hamurabi_omen_needs;

typedef struct {
    int grain;
    int acres;
    int acres_pct;
    int people;
    int guard;
    int yield_bonus;
    int gamble_chance;
    int gamble_acres;
    int gamble_yield_bonus;
} hamurabi_choice;

#endif
