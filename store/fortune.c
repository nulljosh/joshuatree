/* fortune: a line of desert wisdom, picked by the clock. */
/* The store's first app (6.55.0): `install fortune`, then `userrun fortune`. */
#include "jtsys.h"
#include "libjt/string.h"

static const char *const lines[] = {
    "The Joshua tree grows an inch a year and lives five hundred. Take your time.",
    "Granite was here first. Be kind to it.",
    "Carry more water than you think you need.",
    "Every yucca moth has exactly one job, and does it.",
    "The night is cold because the day was honest.",
    "A trail is just everyone who went before you, agreeing.",
    "Small code, like small plants, survives the dry years.",
};

__attribute__((section(".text.start"), used))
void _start(void) {
    unsigned t = 0;
    jt_time(&t);
    const char *l = lines[t % (sizeof lines / sizeof *lines)];
    jt_write(1, "fortune: ", 9);
    jt_write(1, l, (unsigned)strlen(l));
    jt_write(1, "\n", 1);
    jt_exit(0);
}
