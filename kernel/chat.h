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

/* v1.1.0 ("Samantha's tools work from the Chat app"): before asking
   /api/chat at all, ask Turing's picker (POST /api/pick, the same worker,
   the same http_post/json_escape path chat_send already uses) whether the
   message names one of a fixed set of tools ("remind me to buy milk",
   "open notes", "what's the weather"). The picker is a small model plus
   strict server-side validation (worker.js's own PICKABLE list + S.sound);
   this kernel only ever trusts the {"tool","arg"} shape it hands back,
   the same "parse the untrusted network reply defensively" contract
   chat_send already keeps. A JSON `null` tool ({"tool":null,"arg":""})
   needs no special case: json_extract_string only ever matches a string
   value (see its own comment in drivers/json.c), so a null tool naturally
   yields 0 bytes copied, i.e. "no tool" -- the exact same outcome as the
   request failing outright. Never a blocker: any failure here (network
   down, a redirect, no reply) just means "no tool", and the caller falls
   through to chat_send exactly as it did before this pass. */
#define CHAT_TOOL_MAX 24
#define CHAT_ARG_MAX 128

static int chat_pick(const char *msg, char *tool, int toolsz, char *arg, int argsz) {
    if (toolsz > 0) tool[0] = 0;
    if (argsz > 0) arg[0] = 0;
    if (!net_init(0x0A00020F)) return 0;

    static char escaped[CHAT_CONTENT_MAX * 2];
    json_escape(msg, escaped, sizeof(escaped));

    static char req_body[CHAT_CONTENT_MAX * 2 + 32];
    unsigned int n = 0;
    const char *head = "{\"q\":\"";
    while (*head && n < sizeof(req_body)) req_body[n++] = *head++;
    { const char *s = escaped; while (*s && n < sizeof(req_body)) req_body[n++] = *s++; }
    const char *tail = "\",\"sections\":[]}";
    while (*tail && n < sizeof(req_body)) req_body[n++] = *tail++;

    static char resp[512];
    int respn = http_post(llm_host, "/api/pick", (unsigned short)llm_port, req_body, n, resp, sizeof(resp) - 1);
    if (respn == -1) return 0; /* resolve/connect failure: falls through to chat_send the same way chat_send itself would fail */

    { int st = http_last_status();
      if (st == 301 || st == 302 || st == 307 || st == 308) return 0; /* same HTTPS-upgrade case chat_send names explicitly; here it's silent, the pick is only an optimisation */
    }
    if (respn <= 0) return 0;
    resp[respn] = 0;

    unsigned int tn = json_extract_string(resp, "tool", tool, (unsigned int)toolsz);
    if (tn == 0) { serial_puts("chatpick=none\n"); return 0; } /* "tool":null, or missing/malformed */
    json_extract_string(resp, "arg", arg, (unsigned int)argsz);
    serial_puts("chatpick="); serial_puts(tool); serial_puts("\n"); /* discriminating marker for tools/checks/chattools-check.py */
    return 1;
}

static void chat_fmt_reply(char *reply, int replysz, const char *prefix, const char *text) {
    int p = 0;
    for (const char *s = prefix; *s && p < replysz - 1; s++) reply[p++] = *s;
    for (const char *s = text; *s && p < replysz - 1; s++) reply[p++] = *s;
    reply[p] = 0;
}

/* -1 while no app-open is pending; gui_launch_from_dock's own `again:`
   relaunch loop reads and clears this the instant Chat's own window
   closes (see chat_run_tool's open_app case below and that check's own
   comment in kernel.c), the same reopen-after-close shape it already has
   for a click that lands on another dock tile mid-app. */
static int chat_launch_after = -1;

/* Case-insensitive "does word start here" match: lbl is always one of
   this kernel's own known-good GUI_LABELS entries (short, ASCII, no
   punctuation), s is the untrusted, longer phrase the picker copied out
   of the user's message. Matches at s only when lbl's letters line up
   exactly and s either ends there or continues with a space, so "not"
   never matches inside "notes" the wrong way round. */
static int chat_word_prefix_ci(const char *lbl, const char *s) {
    while (*lbl) {
        char a = *lbl, b = *s;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
        lbl++; s++;
    }
    return *s == 0 || *s == ' ';
}

/* Matches arg against GUI_LABELS by whole word, case-insensitively, so
   "notes" or "the weather app" both find "Notes"/"Weather". A leading
   "the " and a trailing " app" are stripped first (both optional, neither
   required), then every word-start position in what's left is tried
   against every real app's label. Returns the matching icon index (into
   GUI_LABELS/gui_launch), or -1 for no match -- open_app then leaves the
   message unhandled rather than guessing, so chat_send/Samantha gets a
   chance to answer instead. */
static int chat_match_app(const char *arg) {
    char buf[80]; int n = 0;
    for (const char *s = arg; *s && n < (int)sizeof(buf) - 1; s++) buf[n++] = *s;
    buf[n] = 0;
    char *b = buf;
    if ((b[0] == 't' || b[0] == 'T') && (b[1] == 'h' || b[1] == 'H') && (b[2] == 'e' || b[2] == 'E') && b[3] == ' ') b += 4;
    int bl = (int)strlen(b);
    if (bl > 4 && b[bl - 4] == ' ' &&
        (b[bl - 3] == 'a' || b[bl - 3] == 'A') && (b[bl - 2] == 'p' || b[bl - 2] == 'P') && (b[bl - 1] == 'p' || b[bl - 1] == 'P'))
        bl -= 4;
    b[bl] = 0;
    for (int i = 0; i < GUI_APPS_FOLDER; i++) {
        for (const char *w = b; ; w++) {
            if ((w == b || *(w - 1) == ' ') && chat_word_prefix_ci(GUI_LABELS[i], w)) return i;
            if (!*w) break;
        }
    }
    return -1;
}

/* Tools chat_pick can name that this OS can actually do locally, no
   further network round trip. Returns 1 with `reply` filled (the caller
   shows it as the assistant turn and skips chat_send entirely) when
   handled; 0 for every other tool on Turing's list (open_url, web_search,
   current_tab, screenshot, clipboard, set_volume, battery, list_dir,
   read_file, make_logo, music, timer, set_heading, scroll_to, theme,
   reset_page, ...), which this kernel has no matching action for yet --
   the caller falls through to chat_send exactly as if chat_pick had
   returned nothing. Every branch that does handle its tool emits a
   `chattool=<tool>:<short result>` serial marker, a discriminating proof
   this actually ran (not just that chat_pick named a tool). */
static int chat_run_tool(const char *tool, const char *arg, char *reply, int replysz) {
    if (replysz > 0) reply[0] = 0;

    if (!strcmp(tool, "new_reminder")) {
        reminders_load();
        if (reminders_count >= REMINDERS_MAX) {
            chat_fmt_reply(reply, replysz, "", "Reminders is full, nothing added.");
            serial_puts("chattool=new_reminder:full\n");
            return 1;
        }
        int idx = reminders_count;
        int c = 0; for (; arg[c] && c < REMINDERS_TEXT_MAX - 1; c++) reminders_text[idx][c] = arg[c];
        reminders_text[idx][c] = 0;
        reminders_done[idx] = 0;
        reminders_count++;
        reminders_save();
        chat_fmt_reply(reply, replysz, "Reminder added: ", arg);
        serial_puts("chattool=new_reminder:"); serial_puts(reminders_text[idx]); serial_puts("\n");
        return 1;
    }

    if (!strcmp(tool, "new_note")) {
        static char buf[4096]; /* same bound editor.h's own editor_buffer keeps for NOTES.TXT */
        int n = vfs_read_file("NOTES.TXT", buf, sizeof(buf) - 1);
        if (n < 0) n = 0;
        if (n > 0 && buf[n - 1] != '\n' && n < (int)sizeof(buf) - 1) buf[n++] = '\n';
        for (const char *s = arg; *s && n < (int)sizeof(buf) - 2; s++) buf[n++] = *s;
        buf[n++] = '\n';
        vfs_replace_file("NOTES.TXT", buf, (unsigned int)n);
        editor_loaded = 0; /* forces Notes to re-read from disk next time it opens, instead of silently overwriting this with a stale in-memory buffer */
        chat_fmt_reply(reply, replysz, "Noted: ", arg);
        serial_puts("chattool=new_note:"); serial_puts(arg); serial_puts("\n");
        return 1;
    }

    if (!strcmp(tool, "weather")) {
        if (weather_have && weather_text[0]) chat_fmt_reply(reply, replysz, "", weather_text);
        else chat_fmt_reply(reply, replysz, "", "No weather reading yet, open Weather first.");
        serial_puts("chattool=weather:"); serial_puts(weather_have ? weather_text : "none"); serial_puts("\n");
        return 1;
    }

    if (!strcmp(tool, "calendar_today")) {
        cal_events_load();
        int y, m, d; cal_read_today(&y, &m, &d);
        char ds[CAL_DATE_LEN + 1]; cal_date_str(y, m, d, ds);
        int idx = cal_events_find(ds);
        if (idx >= 0) chat_fmt_reply(reply, replysz, "Today: ", cal_event_text[idx]);
        else chat_fmt_reply(reply, replysz, "", "Nothing on today's calendar");
        serial_puts("chattool=calendar_today:"); serial_puts(idx >= 0 ? cal_event_text[idx] : "none"); serial_puts("\n");
        return 1;
    }

    if (!strcmp(tool, "say")) {
        chat_fmt_reply(reply, replysz, "", arg);
        serial_puts("chattool=say:"); serial_puts(arg); serial_puts("\n");
        return 1;
    }

    if (!strcmp(tool, "open_app")) {
        int icon = chat_match_app(arg);
        if (icon < 0) return 0; /* unrecognized app name: let Samantha take a shot instead of guessing */
        chat_launch_after = icon;
        chat_fmt_reply(reply, replysz, "Opening ", GUI_LABELS[icon]);
        serial_puts("chattool=open_app:"); serial_puts(GUI_LABELS[icon]); serial_puts("\n");
        return 1;
    }

    return 0; /* open_url, web_search, current_tab, screenshot, clipboard, set_volume, battery, list_dir, read_file, make_logo, music, timer, set_heading, scroll_to, theme, reset_page: not handled */
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
        font_draw_string("n prompt   c clear   esc close   try: remind me to ..., note ..., open notes, weather", 20, (int)window_height() - 28, CHAT_DIM, -1);

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
            chat_draw_status("checking for a tool ...");
            font_draw_string(CHAT_PROMPT, x, T + 76, CHAT_DIM, -1);
            render_wrapped_text(msg, x + prompt_w, T + 76, body_w - prompt_w, 64, CHAT_INK);

            /* v1.1.0: chat_pick first (see chat.h's own comment above
               chat_pick/chat_run_tool) -- a tool this OS can honour
               skips /api/chat entirely; anything else falls straight
               through to chat_send below, unchanged. */
            int handled = 0;
            static char pick_tool[CHAT_TOOL_MAX], pick_arg[CHAT_ARG_MAX], tool_reply[256];
            if (chat_pick(msg, pick_tool, sizeof(pick_tool), pick_arg, sizeof(pick_arg))
                && chat_run_tool(pick_tool, pick_arg, tool_reply, sizeof(tool_reply))) {
                handled = 1;
                if (chat_launch_after >= 0) return; /* open_app: Chat closes, gui_launch_from_dock's again: label reopens the picked app next -- no reply to show */
                chat_push(CHAT_ROLE_USER, msg);
                chat_push(CHAT_ROLE_ASSISTANT, tool_reply);
                state = "ready";
            }

            if (!handled) {
                chat_draw_status("generating ...");
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
}
