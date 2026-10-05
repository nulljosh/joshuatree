#!/bin/sh
# Hamurabi's rules in C against the other three ports (Python, JavaScript, Swift), headless, seconds.
#   - classic: 200 robot reigns add up to 639940, and the robot earns A+ in exactly 878 of 1000
#   - story: 120 scripted reigns across the three levels add up to 390695 (the same script rules.js runs)
#   - random legal orders, classic and story, never push people, acres or grain below zero
#   - the header builds freestanding for i386 with NO undefined symbols (no libgcc, no libc)
# Discriminating: change one number in HAMURABI rules (say .grain = 2800 to 2799) and the classic golden fails;
# change one effect in art/hamurabi/story.json, rerun tools/gen/gen_hamurabi_story.py, and the story golden fails.
set -e
cd "$(dirname "$0")/../.."
python3 tools/gen/gen_hamurabi_story.py >/dev/null
if ! git diff --quiet -- user/hamurabi_story.h 2>/dev/null && [ -n "$CI" ]; then
    echo "FAIL: user/hamurabi_story.h is stale, run tools/gen/gen_hamurabi_story.py and commit it"; exit 1
fi
T=$(mktemp -d /tmp/jt-hamurabi-check-XXXXXX); trap 'rm -rf "$T"' EXIT

cat > "$T/host.c" <<'EOF'
#include <stdio.h>
#include "user/hamurabi_rules.h"

static hamurabi_city reign_classic(unsigned long long seed) {
    hamurabi_rng rng = hamurabi_rng_new(seed);
    hamurabi_city c = hamurabi_new_city(&hamurabi_rules_normal);
    while (!c.over) { hamurabi_orders o = hamurabi_ruler_legal(&c, &hamurabi_ruler_default); hamurabi_step(&c, &o, &rng, &hamurabi_rules_normal); }
    return c;
}

/* Orders that are legal by construction, drawn from a separate seeded stream. */
static hamurabi_orders random_orders(const hamurabi_city *c, hamurabi_rng *d) {
    hamurabi_orders o;
    int maxbuy = c->grain / c->price;
    o.buy = hamurabi_rng_int(d, -c->acres, maxbuy);
    int left = c->grain - o.buy * c->price;
    o.feed = hamurabi_rng_int(d, 0, left);
    int cap = c->acres + o.buy; if (cap > c->people * HAMURABI_TEND) cap = c->people * HAMURABI_TEND;
    if (cap > left - o.feed) cap = left - o.feed;
    o.plant = hamurabi_rng_int(d, 0, cap < 0 ? 0 : cap);
    return o;
}

static int bad(const char *what, unsigned long long seed, const hamurabi_city *c) {
    if (c->people >= 0 && c->acres >= 0 && c->grain >= 0) return 0;
    printf("FAIL %s seed %llu: people=%d acres=%d grain=%d\n", what, seed, c->people, c->acres, c->grain);
    return 1;
}

int main(void) {
    int fail = 0, total = 0;
    for (int s = 0; s < 200; s++) { hamurabi_city c = reign_classic(s); total += c.people + 3 * c.acres + 7 * c.grain + 11 * c.starved_total; }
    printf("classic golden %d (want 639940)\n", total);
    if (total != 639940) fail = 1;

    int aplus = 0;
    for (int s = 0; s < 1000; s++) { hamurabi_city c = reign_classic(s); if (hamurabi_grade(&c) == 3) aplus++; }
    printf("A+ in %d of 1000 (want 878)\n", aplus);
    if (aplus != 878) fail = 1;

    /* the story script from storyGolden() in web/play/rules.js */
    total = 0;
    const hamurabi_rules *levels[3] = { &hamurabi_rules_normal, &hamurabi_rules_easy, &hamurabi_rules_hard };
    for (int s = 0; s < 120; s++) {
        const hamurabi_rules *rules = levels[s % 3];
        hamurabi_rng rng = hamurabi_rng_new(s), srng = hamurabi_rng_new((unsigned long long)s ^ 0x5707ULL);
        hamurabi_city c = hamurabi_new_city(rules);
        unsigned seen = 0; int seen_n = 0;
        while (!c.over) {
            int interlude = 0;
            for (int i = 0; i < HAMURABI_INTERLUDES; i++) if (hamurabi_interludes[i].year == c.year) interlude = 1;
            if (c.year != 1 && !interlude) {
                const hamurabi_omen *o = hamurabi_omen_for(&c, seen, &srng);
                if (o) {
                    seen |= 1u << (int)(o - hamurabi_omens); seen_n++;
                    int order[2] = { s % 2, 1 - s % 2 };
                    for (int k = 0; k < 2; k++) if (hamurabi_can_afford(&o->choice[order[k]], &c)) {
                        hamurabi_apply_choice(&o->choice[order[k]], &c, &srng, &rng);
                        break;
                    }
                }
            }
            hamurabi_orders o = hamurabi_ruler_legal(&c, &hamurabi_ruler_default);
            hamurabi_step(&c, &o, &rng, rules);
        }
        total += c.people + 3 * c.acres + 7 * c.grain + 11 * c.starved_total + 13 * hamurabi_grade(&c) + 17 * seen_n;
    }
    printf("story golden %d (want 390695)\n", total);
    if (total != 390695) fail = 1;

    /* random legal orders, classic and story: nothing goes below zero */
    for (int s = 0; s < 200; s++) {
        hamurabi_rng rng = hamurabi_rng_new(5000 + s), dice = hamurabi_rng_new(9000 + s);
        hamurabi_city c = hamurabi_new_city(&hamurabi_rules_normal);
        while (!c.over) { hamurabi_orders o = random_orders(&c, &dice); if (hamurabi_check(&c, &o)) { printf("FAIL random orders illegal, seed %d\n", s); fail = 1; break; }
                          hamurabi_step(&c, &o, &rng, &hamurabi_rules_normal); fail |= bad("classic", s, &c); }
    }
    for (int s = 0; s < 120; s++) {
        const hamurabi_rules *rules = levels[s % 3];
        hamurabi_rng rng = hamurabi_rng_new(7000 + s), srng = hamurabi_rng_new(8000 + s), dice = hamurabi_rng_new(9500 + s);
        hamurabi_city c = hamurabi_new_city(rules); unsigned seen = 0;
        while (!c.over) {
            const hamurabi_omen *o = hamurabi_omen_for(&c, seen, &srng);
            if (o) {
                seen |= 1u << (int)(o - hamurabi_omens);
                int pick = hamurabi_rng_int(&dice, 0, 1);
                if (hamurabi_can_afford(&o->choice[pick], &c)) hamurabi_apply_choice(&o->choice[pick], &c, &srng, &rng);
                fail |= bad("story choice", s, &c);
            }
            hamurabi_orders ord = random_orders(&c, &dice);
            if (hamurabi_check(&c, &ord)) { printf("FAIL random orders illegal (story), seed %d\n", s); fail = 1; break; }
            hamurabi_step(&c, &ord, &rng, rules); fail |= bad("story", s, &c);
        }
    }
    printf("random legal orders: %s\n", fail ? "see FAIL lines" : "nothing went below zero");
    printf("%s\n", fail ? "FAILED" : "all checks passed");
    return fail;
}
EOF
clang -O2 -Wall -Wextra -Wno-unused-function -I. -o "$T/host" "$T/host.c"
"$T/host"

# The same header, freestanding for i386, touching every function the app will use.
cat > "$T/free.c" <<'EOF'
#include "user/hamurabi_rules.h"
int sink;
void _start(void) {
    hamurabi_rng rng = hamurabi_rng_new(7), srng = hamurabi_rng_new(8);
    hamurabi_city c = hamurabi_new_city(&hamurabi_rules_hard);
    unsigned seen = 0;
    for (int i = 0; i < 12 && !c.over; i++) {
        const hamurabi_omen *om = hamurabi_omen_for(&c, seen, &srng);
        if (om) { seen |= 1u << (om - hamurabi_omens); if (hamurabi_can_afford(&om->choice[0], &c)) sink += hamurabi_apply_choice(&om->choice[0], &c, &srng, &rng).peek_yield; }
        hamurabi_orders o = hamurabi_ruler_legal(&c, &hamurabi_ruler_default);
        if (!hamurabi_check(&c, &o)) sink += hamurabi_step(&c, &o, &rng, &hamurabi_rules_normal).born;
    }
    sink += hamurabi_grade(&c) + (int)hamurabi_rng_int(&rng, 1, 7) + hamurabi_rng_below(&rng, HAMURABI_P45);
    for (;;) { }
}
EOF
clang -target i386-unknown-none -ffreestanding -fno-stack-protector -fno-pic -mno-sse -mno-mmx -Os -Wall -Wextra -Wno-unused-function -I. -c "$T/free.c" -o "$T/free.o"
UND=$(nm -u "$T/free.o" || true)
if [ -n "$UND" ]; then echo "FAIL: the freestanding i386 build needs symbols nobody provides:"; echo "$UND"; exit 1; fi
echo "i386 freestanding build: no undefined symbols"
