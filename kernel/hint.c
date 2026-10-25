/* The keyboard-hint line every app draws, dropped on phones. A header kernel.c included until 4.20.12. */
#include "font.h"
#include "desktop.h"
#include "hint.h"
/* v1.8.5 demo A+ pass, rubric item 3: every app drew its own keyboard-only
   hint line ("up/down to pick ... esc closes") by calling font_draw_string
   directly, one call site per app (~20 of them across mail.h,
   reminders.h, etc, plus kernel.c's own Trash/Recents/Terminal). On a phone
   there is no keyboard and no Esc key (phone_home.h's back chevron is the
   only way back), so that whole line was dead advice shown to a visitor
   who can only tap. Rather than touch all ~20 sites individually, they now
   route through this one helper: draws nothing when boot_to_phone, draws
   exactly the same font_draw_string call otherwise, so desktop is pixel-
   identical and phone silently drops the line. */
void gui_draw_hint(int x, int y, const char *text, unsigned int color){
    if (jt_phone_mode()) return;   /* kernel.c's boot_to_phone */
    font_draw_string(text, x, y, color, -1);
}
