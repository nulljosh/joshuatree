/* 9.2.0, languages (lang.h). Twelve rows of the words the desktop draws: the dock, the Settings sections, the screen
   reader's line and the Language page. Each row is UTF-8; text.c draws Latin, Greek and Vietnamese from Tree Sans and
   the rest from Noto subsets (drivers/world_font.h, made by tools/gen/gen_world_font.py from the strings below). Hindi
   needs shaping, which the kernel does not do: the generator shapes its row with HarfBuzz into private-use codepoints,
   one per shaped glyph, and writes lang_hi.h, so the row here is its source only. JT/LANG.TXT holds `lang=es`. */
#include "lang.h"
#include "emmc.h"
#include "../../drivers/fat.h"
void uart_puts(const char *s);

static const char *const codes[LANG_COUNT] = { "en", "es", "fr", "de", "pt", "it", "vi", "ja", "zh", "ko", "hi", "el" };
static const char *const L[LANG_COUNT][S_COUNT] = {
    { "Apps", "Files", "Mail", "Calendar", "Notes", "Reminders", "Terminal", "Samantha", "Weather", "Stocks", "Settings", "Trash",
      "About", "Display", "Sound", "Dock", "Date and Time", "Users", "Wi-Fi", "Bluetooth", "Language",
      "Settings", "Screen reader on", "app", "of", "Press Return to open.", "Settings, the dock and Samantha follow it.", "English", "Screen reader off" },
    { "Apps", "Archivos", "Correo", "Calendario", "Notas", "Recordatorios", "Terminal", "Samantha", "Tiempo", "Bolsa", "Ajustes", "Papelera",
      "Información", "Pantalla", "Sonido", "Dock", "Fecha y hora", "Usuarios", "Wi-Fi", "Bluetooth", "Idioma",
      "Ajustes", "Lector de pantalla activado", "app", "de", "Pulsa Retorno para abrir.", "Ajustes, el Dock y Samantha lo siguen.", "Español", "Lector de pantalla desactivado" },
    { "Apps", "Fichiers", "Mail", "Calendrier", "Notes", "Rappels", "Terminal", "Samantha", "Météo", "Bourse", "Réglages", "Corbeille",
      "Informations", "Écran", "Son", "Dock", "Date et heure", "Utilisateurs", "Wi-Fi", "Bluetooth", "Langue",
      "Réglages", "Lecteur d'écran activé", "app", "sur", "Appuyez sur Retour pour ouvrir.", "Réglages, le Dock et Samantha la suivent.", "Français", "Lecteur d'écran désactivé" },
    { "Apps", "Dateien", "Mail", "Kalender", "Notizen", "Erinnerungen", "Terminal", "Samantha", "Wetter", "Aktien", "Einstellungen", "Papierkorb",
      "Info", "Anzeige", "Ton", "Dock", "Datum und Uhrzeit", "Benutzer", "WLAN", "Bluetooth", "Sprache",
      "Einstellungen", "Bildschirmleser an", "App", "von", "Zum Öffnen Return drücken.", "Einstellungen, Dock und Samantha folgen ihr.", "Deutsch", "Bildschirmleser aus" },
    { "Apps", "Arquivos", "Mail", "Calendário", "Notas", "Lembretes", "Terminal", "Samantha", "Tempo", "Bolsa", "Ajustes", "Lixo",
      "Sobre", "Tela", "Som", "Dock", "Data e Hora", "Usuários", "Wi-Fi", "Bluetooth", "Idioma",
      "Ajustes", "Leitor de tela ativado", "app", "de", "Pressione Return para abrir.", "Ajustes, o Dock e Samantha seguem o idioma.", "Português", "Leitor de tela desativado" },
    { "App", "File", "Mail", "Calendario", "Note", "Promemoria", "Terminale", "Samantha", "Meteo", "Borsa", "Impostazioni", "Cestino",
      "Info", "Schermo", "Suono", "Dock", "Data e ora", "Utenti", "Wi-Fi", "Bluetooth", "Lingua",
      "Impostazioni", "Lettore schermo attivo", "app", "di", "Premi Invio per aprire.", "Impostazioni, Dock e Samantha la seguono.", "Italiano", "Lettore schermo disattivato" },
    { "Ứng dụng", "Tệp", "Mail", "Lịch", "Ghi chú", "Lời nhắc", "Terminal", "Samantha", "Thời tiết", "Chứng khoán", "Cài đặt", "Thùng rác",
      "Giới thiệu", "Màn hình", "Âm thanh", "Dock", "Ngày và giờ", "Người dùng", "Wi-Fi", "Bluetooth", "Ngôn ngữ",
      "Cài đặt", "Trình đọc màn hình bật", "ứng dụng", "trên", "Nhấn Return để mở.", "Cài đặt, Dock và Samantha theo ngôn ngữ này.", "Tiếng Việt", "Trình đọc màn hình tắt" },
    { "アプリ", "ファイル", "メール", "カレンダー", "メモ", "リマインダー", "ターミナル", "Samantha", "天気", "株価", "設定", "ゴミ箱",
      "情報", "ディスプレイ", "サウンド", "Dock", "日付と時刻", "ユーザ", "Wi-Fi", "Bluetooth", "言語",
      "設定", "画面読み上げオン", "アプリ", "/", "Returnで開きます。", "設定、Dock、Samanthaがこの言語になります。", "日本語", "画面読み上げオフ" },
    { "应用", "文件", "邮件", "日历", "备忘录", "提醒事项", "终端", "Samantha", "天气", "股市", "设置", "废纸篓",
      "关于", "显示", "声音", "程序坞", "日期与时间", "用户", "无线局域网", "蓝牙", "语言",
      "设置", "屏幕阅读器已打开", "应用", "/", "按 Return 打开。", "设置、程序坞和 Samantha 使用此语言。", "中文", "屏幕阅读器已关闭" },
    { "앱", "파일", "메일", "캘린더", "메모", "미리 알림", "터미널", "Samantha", "날씨", "주식", "설정", "휴지통",
      "정보", "디스플레이", "사운드", "Dock", "날짜 및 시간", "사용자", "Wi-Fi", "Bluetooth", "언어",
      "설정", "화면 읽기 켜짐", "앱", "/", "Return 키를 눌러 여세요.", "설정, Dock, Samantha가 이 언어를 따릅니다.", "한국어", "화면 읽기 꺼짐" },
    { 0 },   /* Hindi: lang_hi.h, shaped from the source row below */
    { "Εφαρμογές", "Αρχεία", "Mail", "Ημερολόγιο", "Σημειώσεις", "Υπενθυμίσεις", "Τερματικό", "Samantha", "Καιρός", "Μετοχές", "Ρυθμίσεις", "Κάδος",
      "Πληροφορίες", "Οθόνη", "Ήχος", "Dock", "Ημερομηνία και ώρα", "Χρήστες", "Wi-Fi", "Bluetooth", "Γλώσσα",
      "Ρυθμίσεις", "Αναγνώστης οθόνης ενεργός", "εφαρμογή", "από", "Πατήστε Return για άνοιγμα.", "Οι Ρυθμίσεις, το Dock και η Samantha την ακολουθούν.", "Ελληνικά", "Αναγνώστης οθόνης ανενεργός" },
};
/* read by tools/gen/gen_world_font.py, and spoken by tr_say: the voice reads Devanagari, not the shaped glyph codes */
static const char *const L_hi_src[S_COUNT] = {
    "ऐप्स", "फ़ाइलें", "मेल", "कैलेंडर", "नोट्स", "रिमाइंडर", "टर्मिनल", "Samantha", "मौसम", "स्टॉक्स", "सेटिंग्स", "ट्रैश",
    "परिचय", "डिस्प्ले", "ध्वनि", "Dock", "दिनांक और समय", "उपयोगकर्ता", "Wi-Fi", "Bluetooth", "भाषा",
    "सेटिंग्स", "स्क्रीन रीडर चालू", "ऐप", "/", "खोलने के लिए Return दबाएँ।", "सेटिंग्स, Dock और Samantha इस भाषा में चलते हैं।", "हिन्दी", "स्क्रीन रीडर बंद" };
#include "lang_hi.h"   /* L_hi[S_COUNT] */

int lang_cur;
const char *tr(int id) {
    if (id < 0 || id >= S_COUNT) return "";
    const char *s = lang_cur == LANG_HI ? L_hi[id] : L[lang_cur][id];
    return s ? s : L[0][id];
}
const char *tr_say(int id) { return lang_cur == LANG_HI && id >= 0 && id < S_COUNT ? L_hi_src[id] : tr(id); }
const char *lang_name(int lang) { return lang == LANG_HI ? L_hi[S_SELF] : L[lang][S_SELF]; }
const char *lang_code(int lang) { return codes[lang]; }
const char *lang_english(int lang) {
    static const char *const en[LANG_COUNT] = { "English", "Spanish", "French", "German", "Portuguese", "Italian", "Vietnamese", "Japanese", "Chinese", "Korean", "Hindi", "Greek" };
    return en[lang];
}

#define LANG_FILE "LANG.TXT"
void lang_set(int lang) {
    if (lang < 0 || lang >= LANG_COUNT) return;
    lang_cur = lang;
    uart_puts("lang "); uart_puts(codes[lang]); uart_puts("\n");
    char b[9] = "lang=xx\n"; b[5] = codes[lang][0]; b[6] = codes[lang][1];
    if (sd_ready() && sd_write(LANG_FILE, b, 8) != 1) { void menubar_notify(const char *m); uart_puts("lang: not saved\n"); menubar_notify("The language could not be saved to the card"); }
}
void lang_selftest(void) {   /* text.c's text_init, once the faces are loaded */
    int text_covers(const char *s), text_chars(const char *s);
    int whole = 0, chars = 0;
    for (int i = 0; i < LANG_COUNT; i++) { int ok = 1; for (int k = 0; k < S_COUNT; k++) { const char *s = i == LANG_HI ? L_hi[k] : L[i][k]; ok &= text_covers(s); if (i != LANG_HI) chars += text_chars(s); } whole += ok; }
    char line[] = "lang rows drawable 00 of 12, 00000 characters\n"; line[19] = (char)('0' + whole / 10); line[20] = (char)('0' + whole % 10);
    for (int d = 0, v = chars; d < 5; d++, v /= 10) line[33 - d] = (char)('0' + v % 10);
    uart_puts(line);   /* every word of every row has a glyph, and UTF-8 is read as characters: the check counts them too */
}
void lang_load(void) {
    char b[16];
    if (!sd_ready() || !sd_enter_jt(0)) return;
    int got = fat_read_file(LANG_FILE, b, sizeof b);
    fat_chdir("..");
    if (got < 7 || b[0] != 'l' || b[4] != '=') return;
    for (int i = 0; i < LANG_COUNT; i++) if (b[5] == codes[i][0] && b[6] == codes[i][1]) { lang_cur = i; uart_puts("lang loaded "); uart_puts(codes[i]); uart_puts("\n"); }
}
