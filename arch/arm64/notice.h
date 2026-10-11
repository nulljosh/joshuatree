/* One short menu-bar notice. A newer message replaces the old one. */
struct menu_notice { char text[48]; unsigned since; };
static void notice_set(struct menu_notice *n, const char *s, unsigned now) {
    unsigned i = 0;
    if (s) for (; i < sizeof n->text - 1 && s[i]; i++)
        n->text[i] = s[i] >= 32 && s[i] <= 126 ? s[i] : '?';
    n->text[i] = 0; n->since = now;
}
static int notice_expire(struct menu_notice *n, unsigned now) {
    if (!n->text[0] || now - n->since < 400u) return 0;
    n->text[0] = 0; return 1;
}
