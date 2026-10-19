#ifndef SETTINGS_H
#define SETTINGS_H
/* kernel/settings.c: SETTINGS.TXT (load once at boot, save on every change) and the LLM, Mail and Claude relay
   settings it carries. Settings' window (settings_ui.h), the shell and kmain's command-line overrides read and write
   these directly; SYS_HTTP_POST reads them through the getters in syscall.h. */
#define LLM_MODEL_MAX 32
#define LLM_HOST_MAX 40
#define LLM_MODEL_COUNT 3
#define MAIL_TOKEN_MAX 64
#define CLAUDE_HOST_MAX 40
#define CLAUDE_TOKEN_MAX 64
extern const char *LLM_MODELS[LLM_MODEL_COUNT];   /* the chat models Settings cycles; index 0 is the default */
extern char llm_model[LLM_MODEL_MAX], llm_host[LLM_HOST_MAX], mail_token[MAIL_TOKEN_MAX];
extern char claude_host[CLAUDE_HOST_MAX], claude_token[CLAUDE_TOKEN_MAX];
extern int llm_port, claude_port;
int key_is(const char *k, int len, const char *want);   /* k[0..len) is exactly want */
void settings_load(void);
void settings_save(void);

/* from kernel.c */
extern int wind_enabled, dock_scale_pct;
#endif
