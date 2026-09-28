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

#include "chat_face.h"

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

/* CodeRabbit review of the 1.1.0 Chat work: http_post used to hand
   chat_send/chat_pick net.c's SLOW_REPLY_TIMEOUT_TICKS default (15000
   ticks at irq.c's 100Hz PIT, ~150s), sized for a slow local LLM
   generating under load. A connected-but-silent host -- TCP handshake
   completes, then nothing ever arrives -- held that full default before
   returning, which holds the whole GUI (this app blocks its own input
   loop waiting on chat_send/chat_pick, see gui_app_mouse_tick's own
   comment in kernel.c) for minutes over one bad reply.
   Bounded instead of host-conditional: this repo's only headless way to
   test a "connected but silent" host (tools/checks/chat-samantha-check.py)
   is a Python fake server reached through SLIRP's private 10.0.2.2
   gateway, the exact same address a real local Ollama would also use --
   so a "shorter timeout unless the host looks private (10.x/192.168.x)"
   rule would make the one behaviour this fix exists for untestable here,
   and would do nothing for the compiled-in default host besides (Turing,
   a public host, real replies land in a few seconds either way). Applied
   everywhere instead: 45s for /api/chat, comfortably above every real
   Turing reply observed and still enough for a short local-Ollama answer;
   10s for /api/pick, a small classifier call that should never legitimately
   take that long. Worse case for a genuinely slow local model (net.c's own
   comment cites up to 6 minutes under load) now surfaces the existing
   "error: couldn't reach the host, or no reply" status instead of hanging
   -- a bounded, clearly-reported failure beats an indefinite GUI freeze;
   a per-host or Settings-configurable timeout is a fair follow-up if a
   real local box needs longer. */
#define CHAT_SEND_TIMEOUT_TICKS 4500  /* ~45s at 100Hz: /api/chat */
#define CHAT_PICK_TIMEOUT_TICKS 1000  /* ~10s at 100Hz: /api/pick, a small classifier call */
#define CHAT_SPEAK_TIMEOUT_TICKS 1500 /* ~15s at 100Hz: /api/speak audio download, well under /api/chat's own bound */
#define CHAT_LISTEN_TIMEOUT_TICKS 3000 /* ~30s at 100Hz: worker.js's /api/listen runs Workers AI Whisper cold, plus uploading up to PTT_MAX_SAMPLES over this kernel's own TCP stack */

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
    int respn = http_post_timeout(llm_host, "/api/chat", (unsigned short)llm_port, req_body, rn, resp, sizeof(resp) - 1, CHAT_SEND_TIMEOUT_TICKS);
    if (respn == -1) { serial_puts("chatfail=connect\n"); return 0; } /* resolve/connect failure, no HTTP reply at all: http_last_status is stale, don't trust it */

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

    if (respn <= 0) { serial_puts("chatfail=noreply\n"); return 0; } /* discriminating marker for tools/checks/chat-samantha-check.py's silent-host case: CHAT_SEND_TIMEOUT_TICKS ran out with no data */
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
    const char *tail = "\",\"sections\":[],\"os\":\"jt\"}"; /* os: "jt" unlocks Joshua Tree's own tools (mail) in the picker */
    while (*tail && n < sizeof(req_body)) req_body[n++] = *tail++;

    static char resp[512];
    int respn = http_post_timeout(llm_host, "/api/pick", (unsigned short)llm_port, req_body, n, resp, sizeof(resp) - 1, CHAT_PICK_TIMEOUT_TICKS);
    if (respn == -1) { serial_puts("chatpickfail=connect\n"); return 0; } /* resolve/connect failure: falls through to chat_send the same way chat_send itself would fail */

    { int st = http_last_status();
      if (st == 301 || st == 302 || st == 307 || st == 308) return 0; /* same HTTPS-upgrade case chat_send names explicitly; here it's silent, the pick is only an optimisation */
    }
    if (respn <= 0) { serial_puts("chatpickfail=noreply\n"); return 0; } /* CHAT_PICK_TIMEOUT_TICKS ran out with no data; falls through to chat_send, same as any other pick failure */
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
   this kernel's own known-good APPS[] names (short, ASCII, no
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

/* Matches arg against APPS[].name by whole word, case-insensitively, so
   "notes" or "the weather app" both find "Notes"/"Weather". A leading
   "the " and a trailing " app" are stripped first (both optional, neither
   required), then every word-start position in what's left is tried
   against every real app's label. Returns the matching icon index (into
   APPS/gui_launch), or -1 for no match -- open_app then leaves the
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
    if ((b[0] | 32) == 'c' && (b[1] | 32) == 'h' && (b[2] | 32) == 'a' && (b[3] | 32) == 't' && !b[4]) b = "samantha"; /* Chat was renamed Samantha; "open chat" still works */
    for (int i = 0; i < GUI_APPS_FOLDER; i++) {
        for (const char *w = b; ; w++) {
            if ((w == b || *(w - 1) == ' ') && chat_word_prefix_ci(APPS[i].name, w)) return i;
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
static const char *chat_last_user_msg = "";
static int chat_starts(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; } /* what was actually said, for tools whose arg is only part of it (send_mail) */

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
        chat_fmt_reply(reply, replysz, "Added reminder: ", arg);
        serial_puts("chattool=new_reminder:"); serial_puts(reminders_text[idx]); serial_puts("\n");
        return 1;
    }

    if (!strcmp(tool, "list_reminders")) {
        reminders_load();
        if (!reminders_count) {
            chat_fmt_reply(reply, replysz, "", "No reminders.");
            serial_puts("chattool=list_reminders:none\n");
            return 1;
        }
        int p = 0; int shown = 0;
        for (int i = 0; i < reminders_count && p < replysz - 1; i++) {
            if (reminders_done[i]) continue;
            if (shown) { reply[p++] = ','; reply[p++] = ' '; }
            for (const char *c = reminders_text[i]; *c && p < replysz - 1; c++) reply[p++] = *c;
            shown++;
        }
        reply[p] = 0;
        if (!shown) chat_fmt_reply(reply, replysz, "", "No reminders.");
        serial_puts("chattool=list_reminders:"); serial_puts(shown ? reply : "none"); serial_puts("\n");
        return 1;
    }

    if (!strcmp(tool, "read_notes")) {
        static char buf[4096]; /* same NOTES.TXT bound new_note/editor.h already keep */
        int n = vfs_read_file("NOTES.TXT", buf, sizeof(buf) - 1);
        if (n <= 0) {
            chat_fmt_reply(reply, replysz, "", "No notes yet.");
            serial_puts("chattool=read_notes:none\n");
            return 1;
        }
        buf[n] = 0;
        chat_fmt_reply(reply, replysz, "", buf);
        serial_puts("chattool=read_notes:"); serial_puts(buf); serial_puts("\n");
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
        chat_fmt_reply(reply, replysz, "Opening ", APPS[icon].name);
        serial_puts("chattool=open_app:"); serial_puts(APPS[icon].name); serial_puts("\n");
        return 1;
    }

    if (!strcmp(tool, "read_mail")) {
        /* The newest message, or the newest from whoever she was asked about. */
        mail_load();
        int idx = -1;
        for (int i = mail_count - 1; i >= 0 && idx < 0; i--) {
            if (!arg[0]) { idx = i; break; }
            for (int k = 0; mail_msgs[i].from[k] && idx < 0; k++) {
                int m = 0;
                while (arg[m] && mail_msgs[i].from[k + m] && ((arg[m] | 32) == (mail_msgs[i].from[k + m] | 32))) m++;
                if (!arg[m]) idx = i;
            }
        }
        if (idx < 0) {
            chat_fmt_reply(reply, replysz, arg[0] ? "No mail from " : "", arg[0] ? arg : "Your inbox is empty.");
            serial_puts("chattool=read_mail:none\n");
            return 1;
        }
        mail_msg_t *m = &mail_msgs[idx];
        int p = 0;
        const char *parts[] = { "From ", m->from, ": ", m->subject, ". ", m->body };
        for (int k = 0; k < 6; k++) for (const char *c = parts[k]; *c && p < replysz - 1; c++) reply[p++] = *c;
        reply[p] = 0;
        if (!m->read) { m->read = 1; mail_save(); }
        serial_puts("chattool=read_mail:"); serial_puts(m->subject); serial_puts("\n");
        return 1;
    }

    if (!strcmp(tool, "send_mail")) {
        /* Local until Mail has accounts: the message lands in Mail, addressed. The words
           come from what was said after "that", "saying" or a colon, else all of it. */
        const char *body = chat_last_user_msg;
        for (const char *c = chat_last_user_msg; *c; c++) {
            if (*c == ':') { body = c + 1; break; }
            if (chat_starts(c, " that ")) { body = c + 6; break; }
            if (chat_starts(c, " saying ")) { body = c + 8; break; }
        }
        while (*body == ' ') body++;
        mail_load();
        if (mail_count >= MAIL_MAX) {
            chat_fmt_reply(reply, replysz, "", "Mail is full, nothing sent.");
            serial_puts("chattool=send_mail:full\n");
            return 1;
        }
        mail_msg_t *m = &mail_msgs[mail_count];
        char to[MAIL_FROM_MAX]; int t = 0;
        for (const char *c = "To "; *c; c++) to[t++] = *c;
        for (const char *c = arg; *c && *c != '|' && t < MAIL_FROM_MAX - 1; c++) to[t++] = *c;
        to[t] = 0;
        mail_str_copy(m->from, to, MAIL_FROM_MAX);
        mail_str_copy(m->subject, "From Samantha", MAIL_SUBJECT_MAX);
        int b = 0;
        for (const char *c = body; *c && b < MAIL_BODY_MAX - 1; c++) if (*c != '|') m->body[b++] = *c;
        m->body[b] = 0;
        m->read = 1;
        mail_count++;
        mail_save();
        chat_fmt_reply(reply, replysz, "Wrote it to ", arg);
        serial_puts("chattool=send_mail:"); serial_puts(arg); serial_puts("\n");
        return 1;
    }

    return 0; /* open_url, web_search, current_tab, screenshot, clipboard, set_volume, battery, list_dir, read_file, make_logo, music, timer, set_heading, scroll_to, theme, reset_page: not handled */
}

/* Local keyword fallback for the notes/reminders tools this pass adds
   (read_notes, list_reminders): mirrors mail's own "os":"jt" gate rather
   than widening it, since Turing's picker may not know these two tool
   names yet. Checked only after chat_pick has already failed to name a
   tool (see chat_process_message below), so a picker that does know them
   is always trusted first; this is strictly a fallback, not a bypass.
   Deliberately narrow substring matches, same shape chat_match_app's
   "the "/" app" trimming already uses elsewhere in this file: a false
   positive here just answers a tool question with a tool's own honest
   answer, never a wrong write. */
static int chat_has_word(const char *hay, int n, const char *needle) {
    int nl = 0; while (needle[nl]) nl++;
    for (int i = 0; i + nl <= n; i++) {
        int k = 0; while (k < nl && hay[i + k] == needle[k]) k++;
        if (k == nl) return 1;
    }
    return 0;
}

static int chat_keyword_fallback(const char *msg, char *tool, int toolsz) {
    char lower[256]; int n = 0;
    for (const char *s = msg; *s && n < (int)sizeof(lower) - 1; s++) {
        char c = *s; if (c >= 'A' && c <= 'Z') c += 32; lower[n++] = c;
    }
    lower[n] = 0;
    int asking = chat_has_word(lower, n, "what") || chat_has_word(lower, n, "read") || chat_has_word(lower, n, "list");
    if (asking && chat_has_word(lower, n, "note")) {
        int p = 0; const char *t = "read_notes"; while (*t && p < toolsz - 1) tool[p++] = *t++; tool[p] = 0; return 1;
    }
    if (asking && chat_has_word(lower, n, "reminders")) {
        int p = 0; const char *t = "list_reminders"; while (*t && p < toolsz - 1) tool[p++] = *t++; tool[p] = 0; return 1;
    }
    return 0;
}

/* Uses the shared gui_prompt.h helper to avoid the per-keystroke full-redraw
   bug (v0.76.24). The helper already implements the correct pattern:
   draw chrome once before the loop, redraw content only per keystroke. */

/* An LLM console in the spirit of `ollama run`, not a messenger: one status
   line naming the real model and host every request goes to (llm_model /
   llm_host / llm_port, the same globals chat_send uses, nothing invented),
   each prompt echoed after a plain ">>> ", and the reply as plain wrapped
   text under it. No bubbles drawn as boxes, but a plain "You:"/"Samantha:"
   label (CHAT_DIM) in front of each line tells the two apart at a glance,
   replacing the old bare ">>> " REPL prompt (1.2.0, direct request: "doesn't
   show much capability" -- a debug-looking prompt read like a shell, not a
   product). Only the tail that fits the window is shown (this kernel has
   no scroll-offset input yet, same honest limit render_wrapped_text's own
   "out of room, stop drawing" already has for every other long-text view);
   n opens the prompt, c clears the context, esc closes. */
#define CHAT_YOU "You: "
#define CHAT_SAM "Samantha: "
#define CHAT_DIM 0x0075726E
#define CHAT_INK 0x001C1C1E
#define CHAT_ACCENT 0x00B7862A   /* mustard, like her cardigan */

/* 1.3.0 ("Chat's empty state"): a blank console plus "n prompt" told a
   first-time visitor nothing about what Samantha can actually do here, and
   made them press n before they could even try. This table is every tool
   chat_run_tool above handles that a plain typed sentence can trigger
   (new_reminder, new_note, list_reminders, read_notes, weather,
   calendar_today, open_app -- "say" is
   the picker's own internal echo tool, not something a visitor asks for
   by name, so it has no row here), each phrased as the exact sentence
   that names it. Shown as a selectable list when chat_count is 0; picking
   one sends that exact text through the same chat_process_message path a
   typed message takes. */
typedef struct { const char *text; } chat_suggestion_t;
static const chat_suggestion_t CHAT_SUGGESTIONS[] = {
    { "Remind me to call mom at 5" },
    { "Note: pick up dry cleaning" },
    { "What's the weather like" },
    { "What's on my calendar today" },
    { "What are my reminders" },
    { "What's in my notes" },
    { "Open calculator" },
};
#define CHAT_SUGGEST_COUNT ((int)(sizeof(CHAT_SUGGESTIONS) / sizeof(CHAT_SUGGESTIONS[0])))
static int chat_suggest_sel = 0;

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

/* "Samantha    <state>", the console's one status line. 1.2.0 (direct
   request, "Chat demos on the landing page and doesn't show much
   capability"): dropped the old "<model>   <host>:<port>   <state>" debug
   header -- a visitor never asked which model or port answers them, and
   showing host:port read like an unfinished dev tool, not a product.
   llm_model/llm_host/llm_port are still the real values chat_send/chat_pick
   use underneath (Settings still edits them); only this status line stopped
   printing them. */
static void chat_draw_status(const char *state) {
    char line[LLM_MODEL_MAX + LLM_HOST_MAX + 48];
    int p = 0;
    const char *s = "Samantha"; while (*s) line[p++] = *s++;
    s = "    "; while (*s) line[p++] = *s++;
    while (*state && p < (int)sizeof(line) - 1) line[p++] = *state++;
    line[p] = 0;
    int T = gui_app_dy();
    window_rect(0, T + 40, (int)window_width(), 32, GUI_BG);
    font_draw_string(line, 20, T + 52, CHAT_DIM, -1);
    font_draw_string("Samantha", 20, T + 52, CHAT_ACCENT, -1); /* her name in her colour, over the dim copy */
    if (chat_count == 0) chat_face_draw(T);
}

/* Shared by every way a message can be sent now (n's prompt, a suggestion
   row, or just typing) so all three run chat_pick/chat_run_tool/chat_send
   exactly the same way. Returns the new status line text, or NULL when
   chat_run_tool picked open_app -- the caller must return immediately, the
   same chat_launch_after contract gui_launch_chat_app's caller relied on
   before this was pulled out into its own function. */
/* The conversation: with her face loaded, a big face above her latest
   reply; without it, the transcript tail. Drawn before she speaks too, so
   visitors see who is talking. */
static void chat_draw_conversation(int T, int x, int you_w, int sam_w, int body_w) {
    int y = T + 76, bottom = (int)window_height() - 40;
    window_rect(0, T + 72, (int)window_width(), bottom - (T + 72), GUI_BG);
    if (face_idle_n) {
        int last = -1;
        for (int i = chat_count - 1; i >= 0; i--) if (chat_msgs[i].role == CHAT_ROLE_ASSISTANT) { last = i; break; }
        int cw = (int)window_width() - 40;      /* the small face's reserve doesn't apply up here */
        int cap_rows = last >= 0 ? chat_wrapped_rows(chat_msgs[last].content, cw) : 0;
        if (cap_rows > 2) cap_rows = 2;
        int face_bottom = bottom - cap_rows * 16 - 10;
        int cy = chat_face_draw_big(T + 44, face_bottom) + 10; /* up into the status band's empty middle: the status text sits at the left */
        if (last >= 0) render_wrapped_text(chat_msgs[last].content, x, cy, cw, bottom - cy, CHAT_INK);
        return;
    }
    /* Walk back from the newest turn until the visible area is full,
       then draw what fit top-down: show the tail, never a silent
       overflow. */
    int start = chat_count, used = 0;
    for (int i = chat_count - 1; i >= 0; i--) {
        int user = chat_msgs[i].role != CHAT_ROLE_ASSISTANT;
        int label_w = user ? you_w : sam_w;
        int h = chat_wrapped_rows(chat_msgs[i].content, body_w - label_w) * 16 + (user ? 4 : 12);
        if (used + h > bottom - y && i != chat_count - 1) break;
        used += h;
        start = i;
    }
    int cy = y;
    for (int i = start; i < chat_count && cy + 16 <= bottom; i++) {
        int user = chat_msgs[i].role != CHAT_ROLE_ASSISTANT;
        int label_w = user ? you_w : sam_w;
        int tx = x + label_w;
        int tw = body_w - label_w;
        font_draw_string(user ? CHAT_YOU : CHAT_SAM, x, cy, CHAT_DIM, -1);
        render_wrapped_text(chat_msgs[i].content, tx, cy, tw, bottom - cy, CHAT_INK);
        cy += chat_wrapped_rows(chat_msgs[i].content, tw) * 16 + (user ? 4 : 12);
    }
}

static const char *chat_process_message(char *msg, int T, int x, int you_w, int body_w) {
    window_rect(0, T + 40, (int)window_width(), (int)window_height() - 40 - T, GUI_BG);
    chat_draw_status("checking for a tool ...");
    font_draw_string(CHAT_YOU, x, T + 76, CHAT_DIM, -1);
    render_wrapped_text(msg, x + you_w, T + 76, body_w - you_w, 64, CHAT_INK);

    chat_last_user_msg = msg;

    /* Local command handling for easter eggs */
    extern void window_set_drunk(int mode);
    extern int window_get_drunk(void);
    if (msg[0] && ((msg[0] | 32) == 'd' && (msg[1] | 32) == 'r' && (msg[2] | 32) == 'u' &&
                   (msg[3] | 32) == 'n' && (msg[4] | 32) == 'k' && !msg[5])) {
        window_set_drunk(1);
        chat_push(CHAT_ROLE_USER, msg);
        chat_push(CHAT_ROLE_ASSISTANT, "Whoa. Everything's a little wavy now.");
        return "ready";
    }
    if (msg[0] && ((msg[0] | 32) == 's' && (msg[1] | 32) == 'o' && (msg[2] | 32) == 'b' &&
                   (msg[3] | 32) == 'e' && (msg[4] | 32) == 'r' && (msg[5] | 32) == ' ' &&
                   (msg[6] | 32) == 'u' && (msg[7] | 32) == 'p' && !msg[8])) {
        window_set_drunk(0);
        chat_push(CHAT_ROLE_USER, msg);
        chat_push(CHAT_ROLE_ASSISTANT, "Okay, back to normal.");
        return "ready";
    }

    int handled = 0;
    static char pick_tool[CHAT_TOOL_MAX], pick_arg[CHAT_ARG_MAX], tool_reply[256];
    int picked = chat_pick(msg, pick_tool, sizeof(pick_tool), pick_arg, sizeof(pick_arg));
    if (!picked) picked = chat_keyword_fallback(msg, pick_tool, sizeof(pick_tool));
    if (picked
        && chat_run_tool(pick_tool, pick_arg, tool_reply, sizeof(tool_reply))) {
        handled = 1;
        if (chat_launch_after >= 0) return 0; /* open_app: caller returns, again: reopens the picked app */
        chat_push(CHAT_ROLE_USER, msg);
        chat_push(CHAT_ROLE_ASSISTANT, tool_reply);
        /* A tool's reply ("Reminder set: call mom") is spoken like any answer. */
        if (sb16_present() && tool_reply[0]) {
            chat_draw_status("speaking ...");
            chat_draw_conversation(T, x, you_w, font_string_width(CHAT_SAM), body_w);
            window_present();
            chat_face_speak(llm_host, (unsigned short)llm_port, tool_reply, CHAT_SPEAK_TIMEOUT_TICKS);
        }
    }

    if (!handled) {
        chat_draw_status("generating ...");
        static char answer[4096];
        if (chat_send(msg, answer, sizeof(answer))) {
            /* Speak the reply when a sound card is there; speak_text is a
               silent no-op without one or when /api/speak fails. */
            if (sb16_present()) {
                chat_draw_status("speaking ...");
                chat_draw_conversation(T, x, you_w, font_string_width(CHAT_SAM), body_w);
                window_present();
                chat_face_speak(llm_host, (unsigned short)llm_port, answer, CHAT_SPEAK_TIMEOUT_TICKS);
            }
            return "ready";
        }
        const char *e = chat_error();
        return e[0] ? e : "error: couldn't reach the host, or no reply";
    }
    return "ready";
}

/* Push-to-talk (v1.6.23, direct owner request: "voice-and-video first").
   Held F2 records, released F2 sends -- see kernel.c's KEY_PTT for why F2:
   it is a real scancode (0x3C make, 0xBC break) that kbd_map never turns
   into a character, so it was reaching here as a silently-dropped key
   already, the same unclaimed slot the synthetic clipboard keys used.

   sb16_record's DMA transfer blocks for a whole chunk at a time (about 2s
   at 16kHz -- see sb16.c's DMA_CHUNK), so a held key's release can only be
   noticed between chunks, not mid-word; PTT_MAX_SECONDS bounds the total
   at 8s of 8-bit 16kHz mono, matching the task's own cap. The level meter
   updates once per chunk (peak deviation from the 128 midpoint), not live
   -- the same 2s granularity.

   IMPORTANT, found reading QEMU's own hw/audio/sb16.c before writing this:
   QEMU's `-device sb16` logs "ADC not yet supported" for every recording
   DSP command (0x24/0x2C/0xB0-0xCF with the ADC bit set) and never drives
   an audio-input backend at all -- 0x42 (set input rate) is dead code that
   silently sets the *output* rate instead. So `make talk` under QEMU's
   sb16 model will never see IRQ 5 fire for a real recording: sb16_record
   times out and returns 0, cleanly, exactly like the no-card case. This
   driver is written to the real, DSP-2.xx-compatible hardware command set
   (what a real SB16 and, per the task, a from-scratch reimplementation
   would both honor) -- it is QEMU's own sb16 emulation, not this code,
   that has no record path today. */
#define PTT_CHUNK_SAMPLES (16000u * 2u)   /* one sb16 DMA chunk at 16kHz, ~2s */
#define PTT_MAX_SECONDS   8u
#define PTT_MAX_SAMPLES   (16000u * PTT_MAX_SECONDS)

static void chat_draw_listening(int T, int x, int body_w, unsigned int level_pct) {
    window_rect(0, T + 40, (int)window_width(), (int)window_height() - 40 - T, GUI_BG);
    font_draw_string("Listening... (release F2 to send)", x, T + 76, CHAT_ACCENT, -1);
    if (level_pct > 100) level_pct = 100;
    int meter_w = body_w > 220 ? 220 : body_w;
    window_rect(x, T + 104, meter_w, 14, 0x00EDE6DC);
    window_rect(x, T + 104, (int)((unsigned int)meter_w * level_pct / 100u), 14, CHAT_ACCENT);
    window_present();
}

/* Records, ships the clip to worker.js's /api/listen (Workers AI Whisper,
   same joshuatree.heyitsmejosh.com host kernel/stocks.h and kernel/
   curbfind.h already reach directly, not through llm_host/the Turing
   proxy), and runs whatever text comes back through the exact same
   chat_process_message path a typed message takes. Returns the new status
   line, or NULL when chat_process_message returned NULL (open_app: the
   caller must return immediately, same contract every other call site
   here already follows). */
static const char *chat_ptt_record(int T, int x, int you_w, int body_w) {
    if (!sb16_present()) return "no sound card";
    if (!net_init(0x0A00020F)) return "no network";
    unsigned char *pcm = kmalloc(PTT_MAX_SAMPLES);
    if (!pcm) return "out of memory";
    unsigned int captured = 0;
    for (;;) {
        unsigned int want = PTT_MAX_SAMPLES - captured;
        if (want > PTT_CHUNK_SAMPLES) want = PTT_CHUNK_SAMPLES;
        if (!want) break;
        int got = sb16_record(pcm + captured, want, 16000u);
        if (got <= 0) break;
        unsigned int peak = 0;
        for (int i = 0; i < got; i++) {
            int d = (int)pcm[captured + i] - 128; if (d < 0) d = -d;
            if ((unsigned int)d > peak) peak = (unsigned int)d;
        }
        captured += (unsigned int)got;
        chat_draw_listening(T, x, body_w, peak * 100u / 128u);
        int released = 0, sc;
        while ((sc = kbd_pop()) >= 0) if (sc == 0xBC) released = 1; /* F2 break code */
        if (released || (unsigned int)got < want) break;
    }
    if (!captured) { kfree(pcm); return "nothing recorded"; }
    chat_draw_status("listening to you ...");
    char resp[1024]; /* stack, not .bss -- tools/checks/bss-margin-check.py keeps the ring-3
                         window's margin real, and this is only ever live for this one call */
    int respn = http_post_timeout("joshuatree.heyitsmejosh.com", "/api/listen", 80,
                                   (const char *)pcm, captured, resp, sizeof(resp) - 1, CHAT_LISTEN_TIMEOUT_TICKS);
    kfree(pcm);
    if (respn <= 0 || http_last_status() != 200) return "couldn't hear that";
    resp[respn] = 0;
    char text[CHAT_CONTENT_MAX]; /* stack, like every other chat_process_message caller's msg[] */
    if (!json_extract_string(resp, "text", text, sizeof(text)) || !text[0]) return "didn't catch that";
    return chat_process_message(text, T, x, you_w, body_w);
}

static void gui_launch_chat_app(void) {
    chat_load();
    serial_puts("chatchrome\n"); /* discriminating marker for tools/checks/termchatflash-check.sh, same convention editor.h's "editorchrome" already established */
    window_clear(GUI_BG);
    gui_draw_app_titlebar("Samantha"); /* v0.76.11: drawn once, not every keystroke -- see chat_prompt_line's own comment */
    const char *state = "ready";
    int T = gui_app_dy();
    chat_face_load();
    for (;;) {
        window_rect(0, T + 40, (int)window_width(), (int)window_height() - 40 - T, GUI_BG);
        chat_draw_status(state);
        serial_puts("chatconsole\n"); /* marker for tools/checks/chat-check.sh: the console view drew, status line included */

        int x = 20, y = T + 76;
        int bottom = (int)window_height() - 40;
        int you_w = font_string_width(CHAT_YOU);
        int sam_w = font_string_width(CHAT_SAM);
        int body_w = (int)window_width() - 40 - chat_face_reserve();

        if (chat_count == 0) {
            /* 1.3.0: empty state -- a short line from Samantha plus every
               tool chat_run_tool can honour, phrased as the exact sentence
               that names it. Up/down or a click picks a row, enter or a
               click sends it -- no need to press n first. */
            render_wrapped_text("Ask me to remind you, jot a note, check the weather, look at today's calendar, or open an app. Or just type your own message below.",
                                 x, y, body_w, 48, CHAT_DIM);
            for (int i = 0; i < CHAT_SUGGEST_COUNT; i++) {
                int ry = y + 60 + i * 24;
                if (i == chat_suggest_sel) window_rect(x - 4, ry - 4, body_w, 20, 0x00EDE6DC);
                font_draw_string(CHAT_SUGGESTIONS[i].text, x, ry, CHAT_INK, -1);
            }
            font_draw_string("up/down select   enter sends   hold F2 to talk   or just type   esc close", 20, (int)window_height() - 28, CHAT_DIM, -1);
        } else {
            chat_draw_conversation(T, x, you_w, sam_w, body_w);
            font_draw_string("type to send   hold F2 to talk   n prompt   c clear   esc close", 20, (int)window_height() - 28, CHAT_DIM, -1);
        }

        sleep_ticks(5);
        mouse_click_edge_sync();
        /* While waiting, her idle loop plays (12fps); no face, plain wait. */
        int k;
        while (!(k = get_key_or_click_until(face_idle_n ? ticks() + 8 : 0))) chat_face_idle_tick();
        if (k == KEY_ESC) return;
        if (k == KEY_PTT) {
            const char *ns = chat_ptt_record(T, x, you_w, body_w);
            if (!ns) return;
            state = ns;
            continue;
        }
        if (k == KEY_CLICK) {
            if (chat_count == 0) {
                int click_vx = app_cursor_x - app_view_x, click_vy = app_cursor_y - app_view_y;
                int hit = -1;
                for (int i = 0; i < CHAT_SUGGEST_COUNT; i++) {
                    int ry = y + 60 + i * 24;
                    if (click_vx >= x - 4 && click_vx < x - 4 + body_w && click_vy >= ry - 4 && click_vy < ry + 16) { hit = i; break; }
                }
                if (hit >= 0) {
                    chat_suggest_sel = hit;
                    char msg[CHAT_CONTENT_MAX];
                    int n = 0; const char *p = CHAT_SUGGESTIONS[hit].text;
                    while (*p && n < (int)sizeof(msg) - 1) msg[n++] = *p++;
                    msg[n] = 0;
                    const char *ns = chat_process_message(msg, T, x, you_w, body_w);
                    if (!ns) return;
                    state = ns;
                    continue;
                }
            }
            return;
        }
        if (chat_count == 0 && k == KEY_UP) { if (chat_suggest_sel > 0) chat_suggest_sel--; continue; }
        if (chat_count == 0 && k == KEY_DOWN) { if (chat_suggest_sel < CHAT_SUGGEST_COUNT - 1) chat_suggest_sel++; continue; }
        if (chat_count == 0 && k == KEY_ENTER) {
            char msg[CHAT_CONTENT_MAX];
            int n = 0; const char *p = CHAT_SUGGESTIONS[chat_suggest_sel].text;
            while (*p && n < (int)sizeof(msg) - 1) msg[n++] = *p++;
            msg[n] = 0;
            const char *ns = chat_process_message(msg, T, x, you_w, body_w);
            if (!ns) return;
            state = ns;
            continue;
        }
        if (k == 'c') { chat_clear(); chat_suggest_sel = 0; state = "ready"; continue; }
        if (k == 'n') {
            char msg[CHAT_CONTENT_MAX];
            if (!gui_prompt_line_input("Samantha", CHAT_YOU "send a message (enter sends, esc cancels)", msg, sizeof(msg))) continue;
            if (msg[0] == 0) continue;
            const char *ns = chat_process_message(msg, T, x, you_w, body_w);
            if (!ns) return;
            state = ns;
            continue;
        }
        /* 1.3.0: typing any printable key starts a prompt directly, seeded
           with that character -- no need to press n first (n still works,
           handled above; the two shortcut letters that would otherwise be
           swallowed as a message's first character, n and c, are handled
           above this branch so they keep their own meaning). */
        if (k >= 32 && k < 127) {
            char msg[CHAT_CONTENT_MAX];
            if (!gui_prompt_line_input_seeded("Samantha", CHAT_YOU "send a message (enter sends, esc cancels)", msg, sizeof(msg), k)) continue;
            if (msg[0] == 0) continue;
            const char *ns = chat_process_message(msg, T, x, you_w, body_w);
            if (!ns) return;
            state = ns;
            continue;
        }
    }
}

/* "samantha" on the boot command line (kmain -> gui_run, kernel.c's
   boot_to_samantha): the very first frame after the splash is her, full
   screen -- big face, a caption, and an already-drawn, already-live input
   box, not the icon desktop. No conversation exists yet, so this is its
   own draw (chat_draw_conversation only runs once chat_count > 0), but it
   ends by handing off into the exact same gui_launch_chat_app console
   this same face and input live in the rest of the time: esc here goes
   straight to Chat's normal empty state, and a real first message is
   processed then dropped into that same console too, so opening an app
   afterward (chat_run_tool's "open_app") works exactly as it does from
   the dock icon. */
static void chat_boot_samantha_open(void) {
    chat_load();
    chat_face_load();
    window_clear(GUI_BG);
    gui_draw_app_titlebar("Samantha");
    int T = gui_app_dy();
    int bottom = (int)window_height() - 40;
    /* phone: portrait layout -- face centered in the top half sized to the
       screen width, caption right under it, input box stays pinned to the
       bottom same as the desktop layout below. Desktop/samantha-only keeps
       its original bottom-anchored face (unchanged from PR #246). */
    if (boot_to_phone) {
        int half = T + 20 + ((int)window_height() - (T + 20)) / 2;
        int cy = chat_face_draw_big(T + 20, half);
        render_wrapped_text("Tell me what to do.", 20, cy + 16, (int)window_width() - 40, 20, CHAT_DIM);
    } else {
        chat_face_draw_big(T + 20, bottom - 76);
        render_wrapped_text("Tell me what to do.", 20, bottom - 60, (int)window_width() - 40, 20, CHAT_DIM);
    }
    serial_puts("samopen\n"); /* discriminating marker for tools/checks/samantha-boot-check.py: full-screen avatar is up */

    unsigned int n = 0;
    char msg[CHAT_CONTENT_MAX];
    msg[0] = 0;
    mouse_click_edge_sync();
    for (;;) {
        window_rect(20, bottom - 30, (int)window_width() - 40, 20, 0x00FFFFFF);
        msg[n] = 0;
        font_draw_string(msg, 24, bottom - 28, 0x001C1C1E, -1);
        serial_puts("samfocus\n"); /* discriminating marker: the input box is drawn and reading keys every frame, i.e. focused */
        int k = get_key_or_click();
        if (k == KEY_ESC || k == KEY_CLICK) { gui_launch_chat_app(); return; }
        if (k == KEY_ENTER) break;
        if (k == '\b') { if (n > 0) n--; continue; }
        if (k >= 32 && k < 127 && n < sizeof(msg) - 1) msg[n++] = (char)k;
    }
    if (msg[0] != 0) {
        int x = 20, you_w = font_string_width(CHAT_YOU);
        int body_w = (int)window_width() - 40 - chat_face_reserve();
        chat_process_message(msg, T, x, you_w, body_w);
    }
    gui_launch_chat_app();
}
