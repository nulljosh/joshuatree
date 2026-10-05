#!/bin/bash
# Hamurabi rules validation: tests against exact golden checksums.
set -e
cd "$(dirname "$0")/../.."

HARNESS=$(mktemp /tmp/jt-hamurabi-host-XXXXXX.c)
HARNESS_BIN=$(mktemp /tmp/jt-hamurabi-host-XXXXXX)
I386_HARNESS=$(mktemp /tmp/jt-hamurabi-i386-harness-XXXXXX.c)
I386_OBJ=$(mktemp /tmp/jt-hamurabi-i386-XXXXXX.o)
trap "rm -f $HARNESS $HARNESS_BIN $I386_HARNESS $I386_OBJ" EXIT

# Host harness: test golden numbers and invariants
cat > "$HARNESS" <<'HARNESS_CODE'
#include <stdio.h>
#include <string.h>
#include "user/hamurabi_rules.h"

int main(void) {
    /* Golden: 200 robot reigns */
    int golden = 0;
    for (int seed = 0; seed < 200; seed++) {
        hamurabi_rng rng = hamurabi_rng_new(seed);
        hamurabi_city c = hamurabi_new_city(&hamurabi_rules_normal);
        const hamurabi_ruler_knobs knobs = hamurabi_ruler_default;
        while (!c.over) {
            hamurabi_orders o = hamurabi_ruler_legal(&c, &knobs);
            hamurabi_step(&c, &o, &rng, &hamurabi_rules_normal);
        }
        golden += c.people + 3 * c.acres + 7 * c.grain + 11 * c.starved_total;
    }
    printf("golden %d (want 639940)\n", golden);
    if (golden != 639940) return 1;

    /* A+ grader: exactly 878 of 1000 reigns */
    int a_plus = 0;
    for (int seed = 0; seed < 1000; seed++) {
        hamurabi_rng rng = hamurabi_rng_new(seed);
        hamurabi_city c = hamurabi_new_city(&hamurabi_rules_normal);
        const hamurabi_ruler_knobs knobs = hamurabi_ruler_default;
        while (!c.over) {
            hamurabi_orders o = hamurabi_ruler_legal(&c, &knobs);
            hamurabi_step(&c, &o, &rng, &hamurabi_rules_normal);
        }
        if (hamurabi_grade(&c) == 3) a_plus++;
    }
    printf("A+ count %d (want 878)\n", a_plus);
    if (a_plus != 878) return 1;

    /* Invariants: 200 reigns no negatives */
    for (int seed = 0; seed < 200; seed++) {
        hamurabi_rng rng = hamurabi_rng_new(seed);
        hamurabi_city c = hamurabi_new_city(&hamurabi_rules_normal);
        const hamurabi_ruler_knobs knobs = hamurabi_ruler_default;
        while (!c.over) {
            hamurabi_orders o = hamurabi_ruler_legal(&c, &knobs);
            hamurabi_step(&c, &o, &rng, &hamurabi_rules_normal);
            if (c.people < 0 || c.acres < 0 || c.grain < 0) {
                printf("FAIL: seed %d negative\n", seed);
                return 1;
            }
        }
    }
    printf("invariants ok\n");

    /* Story golden: 120 reigns (40 normal, 40 easy, 40 hard) */
    int story_golden = 0;
    for (int seed = 0; seed < 120; seed++) {
        int rule_idx = seed % 3;
        const hamurabi_rules *r = rule_idx == 0 ? &hamurabi_rules_normal :
                                  rule_idx == 1 ? &hamurabi_rules_easy :
                                  &hamurabi_rules_hard;
        hamurabi_rng rng = hamurabi_rng_new(seed);
        hamurabi_city c = hamurabi_new_city(r);
        const hamurabi_ruler_knobs knobs = hamurabi_ruler_default;
        while (!c.over) {
            hamurabi_orders o = hamurabi_ruler_legal(&c, &knobs);
            hamurabi_step(&c, &o, &rng, r);
        }
        int grade = hamurabi_grade(&c);
        story_golden += c.people + 3 * c.acres + 7 * c.grain + 11 * c.starved_total +
                        13 * grade + 17 * 0;  /* 0 seen omens for now */
    }
    printf("story golden %d (want 390695)\n", story_golden);
    if (story_golden != 390695) return 1;

    printf("all checks passed\n");
    return 0;
}
HARNESS_CODE

clang -O2 -Wall -Wextra -I. -o "$HARNESS_BIN" "$HARNESS"
"$HARNESS_BIN"

# i386 harness: test freestanding compilation
cat > "$I386_HARNESS" <<'I386_HARNESS_CODE'
#include "user/hamurabi_rules.h"

void _start(void) {
    hamurabi_rng rng = hamurabi_rng_new(0);
    hamurabi_city c = hamurabi_new_city(&hamurabi_rules_normal);
    hamurabi_orders o;
    o.buy = 0; o.feed = 1000; o.plant = 0;
    if (!hamurabi_check(&c, &o)) {
        hamurabi_step(&c, &o, &rng, &hamurabi_rules_normal);
    }
    int g = hamurabi_grade(&c);
    (void)g;
}
I386_HARNESS_CODE

echo "Checking i386-freestanding compilation..."
clang -target i386-unknown-none -ffreestanding -c \
    -I. -Iuser -Ithird_party/bearssl/inc -Ithird_party/bearssl/src \
    -o "$I386_OBJ" "$I386_HARNESS" 2>&1 | grep -v "warning:" || true

echo "Checking for ANY undefined symbols..."
if nm "$I386_OBJ" 2>/dev/null | grep "^[[:space:]]*U " > /dev/null 2>&1; then
    echo "FAIL: found undefined symbols:"
    nm "$I386_OBJ" | grep "^[[:space:]]*U "
    exit 1
fi

echo "All tests passed"
