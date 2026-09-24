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
   pair.

   1.0.12 (direct owner request, "hook Chat up to our Samantha LLM"): the
   compiled-in defaults (kernel.c) point at the Turing project's own
   Cloudflare Worker (turing.heyitsmejosh.com:80, model "samantha"), not
   a local Ollama server on the host Mac -- Settings still lets anyone
   point this at a real local Ollama install instead, nothing here
   assumes the default is the only valid target. Turing's own `/api/chat`
   is deliberately Ollama-shaped (same request/response fields this file
   already builds/parses), so no wire-format change was needed, just the
   defaults. One real gap that default change exposed: a Cloudflare-fronted host
   like turing.heyitsmejosh.com can answer plain HTTP with a redirect to
   HTTPS (Cloudflare's "Always Use HTTPS" zone setting), which this kernel
   cannot speak (no TLS anywhere in this stack); whether the real host does
   is settled by check.yml's `network` job, not assumed here. chat_send
   below now recognizes a 3xx off `http_last_status()`
   and reports it as a clear, specific status instead of the old generic
   "no reply" (which read exactly like a dead host or a typo, not "you
   need a different port/host"), via chat_error() below. */

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

/* 1.0.12: the specific, actionable failure chat_send hit last time, empty
   string when the last failure (if any) was the old generic kind (no
   route, timeout, no reply) that already had a fine generic message.
   Callers (the shell `chat` command and the GUI app below) check this
   right after a chat_send() failure and prefer it over their own
   generic text when it's non-empty. */
#define CHAT_ERR_MAX 96
static char chat_last_error[CHAT_ERR_MAX] = "";
static const char *chat_error(void) { return chat_last_error; }

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
    /* v0.85.4: qwen3:8b, then the local-Ollama default, emits a
       <think>...</think> reasoning block ahead of its real answer by
       default; Ollama's own /api/chat takes a "think":false field to turn
       that off at the model level (supported since Ollama added
       reasoning-model support, confirmed against this host's ollama
       0.34.2), the smallest correct fix, no client-side tag stripping
       needed. Kept sending unconditionally in 1.0.12 now that the
       compiled-in default is "samantha" against Turing's own Worker:
       llama3.1:8b and Samantha both just ignore a field they don't
       understand, same as any Ollama-compatible server already does for
       any option it doesn't recognize. */
    const char *head2 = "\",\"stream\":false,\"think\":false,\"messages\":[";
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
    chat_last_error[0] = 0; /* clear any stale message from a previous send before this one runs */

    if (!net_init(0x0A00020F)) return 0;

    static char req_body[6144]; /* real growth from the old 768-byte cap; bounded to keep worst-case heap use (see http_post) sane */
    unsigned int rn = chat_build_request(req_body, sizeof(req_body));

    static char resp[8192]; /* real growth from the old 4096-byte cap */
    int respn = http_post(llm_host, "/api/chat", (unsigned short)llm_port, req_body, rn, resp, sizeof(resp) - 1);
    if (respn == -1) return 0; /* resolve/connect failure, no HTTP reply at all: http_last_status is stale, don't trust it */

    /* 1.0.12: a host that upgrades plain HTTP to HTTPS (Cloudflare's
       "Always Use HTTPS" default in front of turing.heyitsmejosh.com would)
       answers the plain-HTTP request above with a real redirect
       (301/302/307/308) instead of a JSON body, and this kernel cannot
       follow it (no TLS anywhere in this stack, see docs/THREAT-MODEL.md). The old code
       just fell through to json_extract_string finding nothing and
       reported the same generic "no reply" as a dead host or a typo'd
       port -- a real, specific, fixable cause deserves a real, specific
       message instead of that generic one. Checked before the body-length
       branch below since a redirect's own tiny body ("Moved
       Permanently") can still make respn > 0. */
    { int st = http_last_status();
      if (st == 301 || st == 302 || st == 307 || st == 308) {
          const char *msg = "host redirects to HTTPS; this kernel speaks HTTP only, set another host in Settings";
          int i = 0; while (msg[i] && i < CHAT_ERR_MAX - 1) { chat_last_error[i] = msg[i]; i++; } chat_last_error[i] = 0;
          serial_puts("chathttps=1\n"); /* discriminating marker for tools/checks/chat-samantha-check.py's redirect case */
          return 0;
      }
    }

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

    /* v0.85.4: mirror a bounded prefix of the real reply to serial, the
       same real-fetch-proof convention weather_fetch's wx=/geo= lines
       already establish (net.c v71): a headless QEMU boot can prove a
       real model reply actually arrived without a screen, the same gap
       chattest's own comment names as out of scope for a network-free
       regression test. Newlines/CRs flattened to spaces so the marker
       stays one line; capped well under a full reply, this is a proof
       marker, not a render path. */
    { char mirror[300]; unsigned int mi = 0;
      for (const char *s = answer; *s && mi < sizeof(mirror) - 1; s++) {
          char c = *s; if (c == '\n' || c == '\r') c = ' '; mirror[mi++] = c;
      }
      mirror[mi] = 0;
      serial_puts("chatreply="); serial_puts(mirror); serial_puts("\n");
    }

    chat_push(CHAT_ROLE_ASSISTANT, answer);
    return 1;
}

/* Uses the shared gui_prompt.h helper to avoid the per-keystroke full-redraw
   bug (v0.76.24). The helper already implements the correct pattern:
   draw chrome once before the loop, redraw content only per keystroke. */

/* An LLM console in the spirit of `ollama run`, not a messenger: one status
   line naming the real model and host every request goes to (llm_model /
   llm_host / llm_port, the same globals chat_send uses, nothing invented),
   each prompt echoed after a plain ">>> ", and the reply as plain wrapped
   text under it. No bubbles, no sender labels, no contact framing. Only
   the tail that fits the window is shown (this kernel has no scroll-offset
   input yet, same honest limit render_wrapped_text's own "out of room,
   stop drawing" already has for every other long-text view); n opens the
   prompt, c clears the context, esc closes. */
#define CHAT_PROMPT ">>> "
#define CHAT_DIM 0x0075726E
#define CHAT_INK 0x001C1C1E

/* Rows render_wrapped_text will use for this text at this width, same wrap
   rule, so a long reply pushes the next prompt down instead of being drawn
   over (the old per-'\n' count ignored word wrap entirely). */
static int chat_wrapped_rows(const char *p, int max_w) {
    int rows = 1, x = 0;
    int space_w = font_string_width(" ");
    if (space_w < 1) space_w = 1;
    while (*p) {
        if (*p == '\n') { rows++; x = 0; p++; continue; }
        if (*p == ' ') { if (x + space_w > max_w) { x = 0; rows++; } else x += space_w; p++; continue; }
        char word[256]; unsigned int n = 0;
        while (*p && *p != ' ' && *p != '\n') { if (n < sizeof(word) - 1) word[n++] = *p; p++; }
        word[n] = 0;
        int ww = font_string_width(word);
        if (x > 0 && x + ww > max_w) { x = 0; rows++; }
        x += ww;
    }
    return rows;
}

/* "<model>   <host>:<port>   <state>", the console's one status line. */
static void chat_draw_status(const char *state) {
    char line[LLM_MODEL_MAX + LLM_HOST_MAX + 48];
    int p = 0;
    const char *s = llm_model; while (*s && p < (int)sizeof(line) - 40) line[p++] = *s++;
    s = "   "; while (*s) line[p++] = *s++;
    s = llm_host; while (*s && p < (int)sizeof(line) - 32) line[p++] = *s++;
    line[p++] = ':';
    char digits[8]; int nd = 0; int v = llm_port;
    do { digits[nd++] = (char)('0' + v % 10); v /= 10; } while (v && nd < 8);
    while (nd) line[p++] = digits[--nd];
    s = "   "; while (*s) line[p++] = *s++;
    while (*state && p < (int)sizeof(line) - 1) line[p++] = *state++;
    line[p] = 0;
    int T = gui_app_dy();
    window_rect(0, T + 40, (int)window_width(), 32, GUI_BG);
    font_draw_string(line, 20, T + 52, CHAT_DIM, -1);
}

static void gui_launch_chat_app(void) {
    chat_load();
    serial_puts("chatchrome\n"); /* discriminating marker for tools/checks/termchatflash-check.sh, same convention editor.h's "editorchrome" already established */
    window_clear(GUI_BG);
    gui_draw_app_titlebar("Chat"); /* v0.76.11: drawn once, not every keystroke -- see chat_prompt_line's own comment */
    const char *state = "ready";
    int T = gui_app_dy();
    for (;;) {
        window_rect(0, T + 40, (int)window_width(), (int)window_height() - 40 - T, GUI_BG);
        chat_draw_status(state);
        serial_puts("chatconsole\n"); /* marker for tools/checks/chat-check.sh: the console view drew, status line included */

        int x = 20, y = T + 76;
        int bottom = (int)window_height() - 40;
        int prompt_w = font_string_width(CHAT_PROMPT);
        int body_w = (int)window_width() - 40;
        /* Walk back from the newest turn until the visible area is full,
           then draw what fit top-down: show the tail, never a silent
           overflow. */
        int start = chat_count, used = 0;
        for (int i = chat_count - 1; i >= 0; i--) {
            int user = chat_msgs[i].role != CHAT_ROLE_ASSISTANT;
            int h = chat_wrapped_rows(chat_msgs[i].content, user ? body_w - prompt_w : body_w) * 16 + (user ? 4 : 12);
            if (used + h > bottom - y && i != chat_count - 1) break;
            used += h;
            start = i;
        }
        int cy = y;
        for (int i = start; i < chat_count && cy + 16 <= bottom; i++) {
            int user = chat_msgs[i].role != CHAT_ROLE_ASSISTANT;
            int tx = user ? x + prompt_w : x;
            int tw = user ? body_w - prompt_w : body_w;
            if (user) font_draw_string(CHAT_PROMPT, x, cy, CHAT_DIM, -1);
            render_wrapped_text(chat_msgs[i].content, tx, cy, tw, bottom - cy, CHAT_INK);
            cy += chat_wrapped_rows(chat_msgs[i].content, tw) * 16 + (user ? 4 : 12);
        }
        if (cy + 16 <= bottom) font_draw_string(CHAT_PROMPT, x, cy, CHAT_DIM, -1); /* the idle prompt, waiting for n */
        font_draw_string("n prompt   c clear context   esc close", 20, (int)window_height() - 28, CHAT_DIM, -1);

        sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();
        if (k == KEY_ESC || k == KEY_CLICK) return;
        if (k == 'c') { chat_clear(); state = "ready"; continue; }
        if (k == 'n') {
            char msg[CHAT_CONTENT_MAX];
            if (!gui_prompt_line_input("Chat", CHAT_PROMPT "send a message (enter sends, esc cancels)", msg, sizeof(msg))) continue;
            if (msg[0] == 0) continue;

            window_rect(0, T + 40, (int)window_width(), (int)window_height() - 40 - T, GUI_BG);
            chat_draw_status("generating ...");
            font_draw_string(CHAT_PROMPT, x, T + 76, CHAT_DIM, -1);
            render_wrapped_text(msg, x + prompt_w, T + 76, body_w - prompt_w, 64, CHAT_INK);

            static char answer[4096]; /* real growth from the old 2048-byte cap */
            /* 1.0.12: chat_error() carries a specific message (currently
               just the HTTPS-redirect case) when chat_send knows exactly
               why it failed; the generic text stands for every other
               failure (DNS/connect failure, timeout, no reply, no "content"
               field in the reply). */
            if (chat_send(msg, answer, sizeof(answer))) state = "ready";
            else state = chat_error()[0] ? chat_error() : "error: couldn't reach the host, or no reply";
        }
    }
}
