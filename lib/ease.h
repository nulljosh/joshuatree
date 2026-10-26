/* One motion curve for every animation on both desktops and in the apps (6.4.0): an ease-out that starts fast and settles,
   1 - (1 - p)^5 (ease-out quint), close to the web's cubic-bezier(.23,1,.32,1). p and the result are thousandths. */
#ifndef JT_EASE_H
#define JT_EASE_H
static inline int ease_out(int p) {
    if (p <= 0) return 0;
    if (p >= 1000) return 1000;
    int q = 1000 - p, r = q;
    for (int i = 0; i < 4; i++) r = r * q / 1000;
    return 1000 - r;
}
#endif
