/* v85 (0.70.0): a real Chat app, not the v10 one-shot shell command it
   grew out of. Three real gaps in the old `chat` handler, checked against
   the actual code before touching anything (kernel/kernel.c's `chat`
   command and the old gui_launch_chat, both now gone):

   (1) `/api/generate` is single-shot, no conversation memory; Ollama's
       `/api/chat` takes a real messages array and the server keeps
       context across turns. This file keeps that history VFS-backed
       (CHAT.TXT), same write-through `vfs_replace_file`/`vfs_read_file`
       pattern mail.h/contacts.h/calendar.h/reminders.h already
       established, so a conversation survives a reboot the same way a
       Mail inbox does.
   (2) The request/response buffers were undersized (768/2048/4096-byte
       caps, a 512-byte escaped-message cap). Grown here, but bounded
       against two real, load-bearing kernel limits found by tracing the
       actual send path rather than assuming a bigger buffer alone would
       help: http.c's http_post used to hard-cap the whole HTTP request
       at a fixed 1024-byte stack buffer regardless of what this file
       passed it, and net.c's tcp_get used to reject any request over one
       536-byte TCP segment outright. Both fixed for real this pass
       (http_post now heap-sizes its request buffer to the caller's real
       body_len; tcp_get now sends multiple back-to-back TCP_MAX_PAYLOAD
       segments instead of refusing anything bigger than one), so the
       larger buffers here are real capacity, not a number that silently
       gets truncated three layers down.
   (3) No GUI window, own dock icon aside (the old gui_launch_chat was a
       single-question popup with no scrollback and its own bespoke
       titlebar draw instead of gui_draw_app_titlebar, an inconsistency
       fixed here too). This is a real scrollback window, following the
       same shape Contacts/Calculator/Settings already established:
       list/detail view, get_key_or_click's touch-first contract, its own
       CHAT.TXT persistence.

   Model/host/port are no longer hardcoded here or in the shell `chat`
   command (kernel.c): both read the same three settings-persisted
   globals (llm_model/llm_host/llm_port, kernel.c's settings_load/save,
   editable from Settings), one source of truth instead of two copies
   that could silently drift apart, the same class of bug the mail/
   contacts/calendar apps already avoid by sharing their own load/save
   pair. */

#define CHAT_MAX 8            /* messages kept (4 user/assistant exchanges); oldest drop first once full */
#define CHAT_CONTENT_MAX 640  /* raw stored content per message; real growth from the old 512-byte input cap */
#define CHAT_ROLE_USER 0
#define CHAT_ROLE_ASSISTANT 1

typedef struct {
    unsigned char role;
    char content[CHAT_CONTENT_MAX];
} chat_msg_t;

static chat_msg_t chat_msgs[CHAT_MAX];
static int chat_count = 0;
static int chat_loaded = 0;

/* Same field-boundary contract contacts.h/mail.h already use: stored
   content can't contain '|' or '\n', so a plain scan for either is a
   real, unambiguous boundary, no escaping needed on disk (JSON escaping
   only happens transiently when building a request, see
   chat_build_request below). User input already can't produce '|'/'\n'
   (the prompt only accepts printable ASCII 32-126, same guard every
   other app's own prompt keeps); a model's reply comes over the network
   and could contain either, so it's sanitized on the way into history. */
static void chat_sanitize_copy(char *dst, const char *src, int max) {
    int i = 0;
    for (const char *s = src; *s && i < max - 1; s++) {
        char c = *s;
        if (c == '|' || c == '\n' || c == '\r') c = ' ';
        dst[i++] = c;
    }
    dst[i] = 0;
}

static void chat_load(void) {
    if (chat_loaded) return;
    chat_loaded = 1;
    static char buf[CHAT_MAX * (CHAT_CONTENT_MAX + 8)];
    int n = vfs_read_file("CHAT.TXT", buf, sizeof(buf) - 1);
    if (n < 0) return; /* no file yet: empty history, a real fresh start */
    buf[n] = 0;
    chat_count = 0;
    int i = 0;
    while (i < n && chat_count < CHAT_MAX) {
        int role = (buf[i] == 'a') ? CHAT_ROLE_ASSISTANT : CHAT_ROLE_USER;
        i += 2; /* role char + the '|' right after it */
        chat_msg_t *m = &chat_msgs[chat_count];
        m->role = (unsigned char)role;
        int j = 0;
        while (i < n && buf[i] != '\n' && j < CHAT_CONTENT_MAX - 1) m->content[j++] = buf[i++];
        m->content[j] = 0;
        while (i < n && buf[i] != '\n') i++; /* truncated field: eat the rest */
        chat_count++;
        if (i < n && buf[i] == '\n') i++;
    }
}

static void chat_save(void) {
    static char buf[CHAT_MAX * (CHAT_CONTENT_MAX + 8)];
    int n = 0;
    for (int idx = 0; idx < chat_count; idx++) {
        chat_msg_t *m = &chat_msgs[idx];
        buf[n++] = (m->role == CHAT_ROLE_ASSISTANT) ? 'a' : 'u';
        buf[n++] = '|';
        const char *s = m->content;
        while (*s && n < (int)sizeof(buf) - 2) buf[n++] = *s++;
        buf[n++] = '\n';
    }
    vfs_replace_file("CHAT.TXT", buf, (unsigned int)n);
}

/* Appends one message, dropping the oldest if the history is already
   full (a real bounded ring, not an unbounded VFS file that could grow
   forever), then writes through immediately, same "no separate Save
   step" contract every other persisted app in this kernel already
   keeps. */
static void chat_push(int role, const char *content) {
    if (chat_count == CHAT_MAX) {
        for (int j = 0; j < CHAT_MAX - 1; j++) chat_msgs[j] = chat_msgs[j + 1];
        chat_count--;
    }
    chat_msg_t *m = &chat_msgs[chat_count];
    m->role = (unsigned char)role;
    chat_sanitize_copy(m->content, content, CHAT_CONTENT_MAX);
    chat_count++;
    chat_save();
}

static void chat_clear(void) {
    chat_count = 0;
    chat_save();
}

/* Builds the real /api/chat request body: {"model":"...","stream":false,
   "messages":[{"role":"user","content":"..."},...]} from the current
   history (chat_msgs, which already includes the just-sent user turn by
   the time this is called). Escapes each message's content through
   json_escape (drivers/json.c, already bounds-safe against maxlen, this
   file just gives it a bigger maxlen than the old code did) into a
   shared scratch buffer, one message at a time, so this needs no
   dynamic allocation of its own beyond the one static scratch buffer,
   matching how every other network caller in this kernel builds a
   request. Returns the byte length written into out, capped at out_cap,
   the same "never write past what the caller gave" contract http_post
   itself now also honestly keeps end to end (see http.c). */
static unsigned int chat_build_request(char *out, unsigned int out_cap) {
    unsigned int n = 0;
    const char *head1 = "{\"model\":\"";
    while (*head1 && n < out_cap) out[n++] = *head1++;
    { const char *s = llm_model; while (*s && n < out_cap) out[n++] = *s++; }
    const char *head2 = "\",\"stream\":false,\"messages\":[";
    while (*head2 && n < out_cap) out[n++] = *head2++;

    static char escaped[CHAT_CONTENT_MAX * 2];
    for (int i = 0; i < chat_count; i++) {
        if (i > 0 && n < out_cap) out[n++] = ',';
        const char *m1 = "{\"role\":\"";
        while (*m1 && n < out_cap) out[n++] = *m1++;
        const char *role_str = (chat_msgs[i].role == CHAT_ROLE_ASSISTANT) ? "assistant" : "user";
        while (*role_str && n < out_cap) out[n++] = *role_str++;
        const char *m2 = "\",\"content\":\"";
        while (*m2 && n < out_cap) out[n++] = *m2++;
        json_escape(chat_msgs[i].content, escaped, sizeof(escaped));
        const char *es = escaped;
        while (*es && n < out_cap) out[n++] = *es++;
        const char *m3 = "\"}";
        while (*m3 && n < out_cap) out[n++] = *m3++;
    }
    const char *tail = "]}";
    while (*tail && n < out_cap) out[n++] = *tail++;
    return n;
}

/* Shared by the shell `chat` command (kernel.c) and the GUI app below:
   pushes the user turn, sends /api/chat with full history, pushes the
   assistant reply on success. Returns 1 with `answer` filled on success,
   0 on any failure (network down, no response field), matching the old
   handlers' own return-shape so callers keep the same branching they
   already had. answer_cap should be sized to what the caller can render/
   print; this function still asks Ollama for the whole reply (no
   streaming, same real gap the roadmap entry named as lower priority,
   needs chunked-transfer-encoding support http.c doesn't have yet). */
static int chat_send(const char *user_msg, char *answer, unsigned int answer_cap) {
    chat_load();
    chat_push(CHAT_ROLE_USER, user_msg);

    if (!net_init(0x0A00020F)) return 0;

    static char req_body[6144]; /* real growth from the old 768-byte cap; bounded to keep worst-case heap use (see http_post) sane */
    unsigned int rn = chat_build_request(req_body, sizeof(req_body));

    static char resp[8192]; /* real growth from the old 4096-byte cap */
    int respn = http_post(llm_host, "/api/chat", (unsigned short)llm_port, req_body, rn, resp, sizeof(resp) - 1);
    if (respn <= 0) return 0;
    resp[respn] = 0;

    /* /api/chat's reply shape is {"message":{"role":"assistant","content":"..."},...},
       not /api/generate's flat "response" field; content is still a
       plain string field either way, so json_extract_string's own
       "search anywhere in the JSON for this key" behaviour finds it
       without needing to parse the nested object first. */
    unsigned int an = json_extract_string(resp, "content", answer, answer_cap);
    if (an == 0) return 0;
    answer[an] = 0;
    chat_push(CHAT_ROLE_ASSISTANT, answer);
    return 1;
}

/* Same lightweight get_key_or_click prompt loop contacts.h/mail.h/
   settings_prompt_line already established (live render, backspace,
   enter confirms, esc or a click cancels). Real growth from the old
   200-byte shell-command cap / GUI popup's own 200-byte cap: this one
   accepts up to CHAT_CONTENT_MAX-1 characters, matching the history
   buffer it feeds. */
/* v0.76.11: direct report, "every keystroke causes page to re-render"
   still reproducing after v0.76.10's Notes-only fix. This loop had the
   identical full-window_clear-plus-titlebar-on-every-keystroke shape;
   the caller (gui_launch_chat_app) already draws the "Chat" titlebar
   before entering here and it never changes while this prompt is open,
   so this now only clears/redraws its own content band (the prompt text
   and input box), matching term_render's own fix in kernel.c. */
static int chat_prompt_line(const char *prompt, char *out, int max) {
    unsigned int n = 0;
    out[0] = 0;
    mouse_click_edge_sync();
    for (;;) {
        window_rect(0, 40, (int)window_width(), (int)window_height() - 40, GUI_BG);
        font_draw_string(prompt, 20, 52, 0x0075726E, -1);
        window_rect(20, 76, (int)window_width() - 40, 20, 0x00FFFFFF);
        out[n] = 0;
        font_draw_string(out, 24, 78, 0x001C1C1E, -1);
        int k = get_key_or_click();
        if (k == KEY_ESC || k == KEY_CLICK) return 0;
        if (k == KEY_ENTER) break;
        if (k == '\b') { if (n > 0) n--; }
        else if ((int)n < max - 1 && k >= 32 && k < 127) out[n++] = (char)k;
    }
    out[n] = 0;
    return 1;
}

/* Real scrollback: the last few turns rendered top-to-bottom, wrapped,
   user/assistant told apart by color the same way Mail tells read/
   unread apart by weight. Only the tail that fits the window is shown
   (this kernel has no scroll-offset input yet, same honest limit
   render_wrapped_text's own "out of room, stop drawing" already has for
   every other long-text view); n adds a new message, c clears history,
   esc closes. */
static void gui_launch_chat_app(void) {
    chat_load();
    serial_puts("chatchrome\n"); /* discriminating marker for tools/checks/termchatflash-check.sh, same convention editor.h's "editorchrome" already established */
    window_clear(GUI_BG);
    gui_draw_app_titlebar("Chat"); /* v0.76.11: drawn once, not every keystroke -- see chat_prompt_line's own comment */
    for (;;) {
        window_rect(0, 40, (int)window_width(), (int)window_height() - 40, GUI_BG);
        font_draw_string("n sends a message   c clears history   esc closes", 20, 52, 0x00807468, -1);

        int y = 76;
        int bottom = (int)window_height() - 20;
        if (chat_count == 0) {
            font_draw_string("No messages yet. Press n to start.", 20, y, 0x00807468, -1);
        } else {
            /* Render from the newest message backward, stopping once we've
               filled the visible area, then draw what fit top-down: the
               same "show the tail, not a silent overflow" contract every
               other unbounded-content view here already keeps. */
            int start = 0;
            int used = 0;
            for (int i = chat_count - 1; i >= 0; i--) {
                int lines = 1;
                for (const char *p = chat_msgs[i].content; *p; p++) if (*p == '\n') lines++;
                int block_h = 16 + lines * 16 + 6; /* label line + wrapped body + gap */
                if (used + block_h > bottom - y && i != chat_count - 1) { start = i + 1; break; }
                used += block_h;
                start = i;
            }
            int cy = y;
            for (int i = start; i < chat_count && cy < bottom; i++) {
                const char *label = (chat_msgs[i].role == CHAT_ROLE_ASSISTANT) ? llm_model : "you";
                unsigned int label_color = (chat_msgs[i].role == CHAT_ROLE_ASSISTANT) ? 0x0085144B : 0x007A2048;
                font_draw_string(label, 20, cy, label_color, -1);
                cy += 16;
                int avail_h = bottom - cy;
                if (avail_h < 16) break;
                render_wrapped_text(chat_msgs[i].content, 20, cy, (int)window_width() - 40, avail_h, 0x001C1C1E);
                int lines = 1;
                for (const char *p = chat_msgs[i].content; *p; p++) if (*p == '\n') lines++;
                cy += lines * 16 + 6;
            }
        }

        sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();
        if (k == KEY_ESC || k == KEY_CLICK) return;
        if (k == 'c') { chat_clear(); continue; }
        if (k == 'n') {
            char msg[CHAT_CONTENT_MAX];
            if (!chat_prompt_line("type a message (enter to send, esc to cancel):", msg, sizeof(msg))) continue;
            if (msg[0] == 0) continue;

            window_rect(0, 40, (int)window_width(), (int)window_height() - 40, GUI_BG);
            char asking[LLM_MODEL_MAX + LLM_HOST_MAX + 32];
            { int p = 0; const char *a1 = "asking "; while (*a1) asking[p++] = *a1++;
              const char *m = llm_model; while (*m && p < (int)sizeof(asking) - 2) asking[p++] = *m++;
              const char *a2 = " ..."; while (*a2 && p < (int)sizeof(asking) - 1) asking[p++] = *a2++;
              asking[p] = 0; }
            font_draw_string(asking, 20, 76, 0x0075726E, -1);

            static char answer[4096]; /* real growth from the old 2048-byte cap */
            if (!chat_send(msg, answer, sizeof(answer))) {
                window_rect(0, 40, (int)window_width(), (int)window_height() - 40, GUI_BG);
                font_draw_string("FAIL (couldn't reach the LLM host, or no reply)", 20, 76, 0x001C1C1E, -1);
                gui_wait_close();
            }
        }
    }
}
