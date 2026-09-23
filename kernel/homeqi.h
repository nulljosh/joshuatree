/* Homeqi: Feng shui home assessment, running natively instead of as an
   HTML card. Eight yes/no questions about your home, one at a time.
   Answer each, see the reasoning, and get a final score out of 8.
   Condensed from Eva Wong's "Good Fengshui". Included by kernel.c. */

typedef struct {
    const char *question;
    const char *reasoning;
    int yes_is_good; /* 1: "yes" is the healthy answer, 0: "no" is */
} HqQuestion;

static const HqQuestion HQ_QUESTIONS[] = {
    {"Does your front door face a busy road head-on?",
     "A direct road creates rushing energy. Ideally, the entrance protects you.", 0},
    {"Is there natural light in your main living room?",
     "Light brings clarity, warmth, and good chi. Rooms without it feel heavy.", 1},
    {"Is your bed against a solid wall?",
     "A solid wall behind supports rest. Open sides make sleep restless.", 1},
    {"Is there clutter at your entry or main pathway?",
     "Blocked pathways block opportunities. Clear spaces welcome good energy.", 0},
    {"Can you see water (stream, pond, or fountain) from your home?",
     "Water attracts wealth and calm. Still water near home is auspicious.", 1},
    {"Are there high-voltage power lines or towers nearby?",
     "Electric fields disrupt chi. Distance and barriers help protect health.", 0},
    {"Does your neighborhood feel safe and well-maintained?",
     "Neglected areas have stagnant chi. Care in public spaces lifts the whole block.", 1},
    {"Can you open windows for fresh air and views?",
     "Open windows bring life and connection. Blocked views trap you in a box.", 1},
};
#define HQ_COUNT ((int)(sizeof(HQ_QUESTIONS) / sizeof(HQ_QUESTIONS[0])))

static int hq_q = 0;       /* current question index */
static int hq_score = 0;   /* score accumulator */
static int hq_show_reasoning = 0; /* 1 if showing reasoning, 0 if showing question */

static void hq_draw(void){
    window_clear(GUI_BG);
    gui_draw_app_titlebar("Homeqi");

    int ctr_y = (int)window_height() / 2 - 60;
    int ctr_x = 40;
    int max_w = (int)window_width() - 80;

    if (hq_q < HQ_COUNT) {
        if (!hq_show_reasoning) {
            /* Show question, score, and choices */
            char q_num[16];
            int qn = hq_q + 1;
            int qlen = 0;
            if (qn >= 10) q_num[qlen++] = (char)('0' + qn / 10);
            q_num[qlen++] = (char)('0' + qn % 10);
            q_num[qlen++] = '.'; q_num[qlen] = 0;
            font_draw_string(q_num, ctr_x, ctr_y, 0x0075726E, -1);

            render_wrapped_text(HQ_QUESTIONS[hq_q].question, ctr_x + 20, ctr_y,
                               max_w, 80, 0x001C1C1E);

            char score_txt[32];
            int slen = 0;
            if (hq_score >= 10) score_txt[slen++] = (char)('0' + hq_score / 10);
            score_txt[slen++] = (char)('0' + hq_score % 10);
            score_txt[slen++] = '/';
            score_txt[slen++] = '8'; score_txt[slen] = 0;
            font_draw_string(score_txt, ctr_x, ctr_y + 100, 0x0075726E, -1);

            font_draw_string("1 yes   2 no", ctr_x, ctr_y + 130, 0x001C1C1E, -1);
        } else {
            /* Show reasoning and prompt for next */
            render_wrapped_text(HQ_QUESTIONS[hq_q].reasoning, ctr_x, ctr_y,
                               max_w, 100, 0x001C1C1E);

            font_draw_string("any key for next", ctr_x, ctr_y + 140, 0x0075726E, -1);
        }
    } else {
        /* Results screen */
        const char *msg = "";
        if (hq_score <= 3) msg = "Your home has work to do.";
        else if (hq_score <= 6) msg = "Your home is pleasant and balanced.";
        else msg = "Your home is excellent. Well done.";

        font_draw_string("Your home assessment:", ctr_x, ctr_y, 0x001C1C1E, -1);

        char score_out[32];
        int slen = 0;
        if (hq_score >= 10) score_out[slen++] = (char)('0' + hq_score / 10);
        score_out[slen++] = (char)('0' + hq_score % 10);
        score_out[slen++] = '/';
        score_out[slen++] = '8'; score_out[slen] = 0;
        font_draw_string(score_out, ctr_x, ctr_y + 30, 0x001C1C1E, -1);

        font_draw_string(msg, ctr_x, ctr_y + 70, 0x0075726E, -1);

        font_draw_string("any key to restart", ctr_x, ctr_y + 140, 0x0075726E, -1);
    }
}

static void gui_launch_homeqi(void){
    hq_q = 0;
    hq_score = 0;
    hq_show_reasoning = 0;
    mouse_click_edge_sync();

    for (;;) {
        hq_draw();
        sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();

        if (k == KEY_ESC) return;

        if (hq_q < HQ_COUNT) {
            if (!hq_show_reasoning) {
                if (k == '1' || k == '2') {
                    if ((k == '1') == HQ_QUESTIONS[hq_q].yes_is_good) hq_score++;
                    hq_show_reasoning = 1;
                }
            } else {
                hq_show_reasoning = 0;
                hq_q++;
            }
        } else {
            /* On results screen, any key restarts */
            hq_q = 0;
            hq_score = 0;
            hq_show_reasoning = 0;
        }
    }
}
