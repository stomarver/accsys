/* ===== session_cli — unified (2026-06-18) ===== */
/* only logout, with check support */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <ctype.h>

#define SESSION_BIN    "./session"
#define CHECK_BIN      "./check"
#define MAX_LINE       4096
#define INPUT_BUF_SZ   256
#define FPS            20
#define FRAME_USEC     (1000000 / FPS)
#define CURSOR_BLINK   10

#define CTRL_D  4
#define CTRL_C  3
#define CTRL_L  12
#define ESC     27

#define ANSI_REV  "\033\x5B" "7m"
#define ANSI_RST  "\033\x5B" "0m"
#define ANSI_RREV "\033\x5B" "91;7m"
#define ANSI_HIDE "\033\x5B" "?25l"
#define ANSI_SHOW "\033\x5B" "?25h"
#define ANSI_CL   "\033\x5B" "K"
#define ANSI_CLR  "\033\x5B" "2J" "\033\x5B" "3J" "\033\x5B" "H"

static int term_rows   = 24;
static int term_cols   = 80;
static int use_color   = 0;
static int force_dumb  = 0;
static int raw_enabled = 0;
static struct termios orig_term;

/* ================ terminal ================ */

static void get_term_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_row > 0) term_rows = ws.ws_row;
        if (ws.ws_col > 0) term_cols = ws.ws_col;
    }
}

static void sleep_us(long us) {
    struct timespec ts;
    ts.tv_sec  = us / 1000000;
    ts.tv_nsec = (us % 1000000) * 1000;
    nanosleep(&ts, NULL);
}

static int detect_color_support(void) {
    const char *term, *colorterm;
    if (force_dumb) return 0;
    if (!isatty(STDOUT_FILENO)) return 0;
    colorterm = getenv("COLORTERM");
    if (colorterm && *colorterm) return 1;
    term = getenv("TERM");
    if (!term || !*term) return 0;
    if (strcmp(term, "dumb") == 0) return 0;
    if (strstr(term, "color") || strstr(term, "xterm")  ||
        strstr(term, "screen")|| strstr(term, "tmux")   ||
        strstr(term, "rxvt")  || strstr(term, "linux")  ||
        strstr(term, "vte")   || strstr(term, "konsole")||
        strstr(term, "gnome") || strstr(term, "alacritty") ||
        strstr(term, "kitty")) return 1;
    return 0;
}

static void term_raw_mode(void) {
    struct termios raw;
    if (raw_enabled) return;
    if (tcgetattr(STDIN_FILENO, &orig_term) < 0) return;
    raw = orig_term;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_enabled = 1;
}

static void term_restore(void) {
    if (!raw_enabled) return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_term);
    raw_enabled = 0;
}

static int input_available(void) {
    struct timeval tv;
    fd_set fds;
    tv.tv_sec  = 0;
    tv.tv_usec = 0;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

static int read_char(void) {
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) == 1) return (int)c;
    return -1;
}

/* ================ backend helpers ================ */

static void trim_nl(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
}

static int call_status(const char *cmd) {
    int ret = system(cmd);
    if (ret == -1) return 1;
    return WEXITSTATUS(ret);
}

static char *call_output(const char *cmd) {
    FILE *fp;
    char *buf = malloc(MAX_LINE);
    if (!buf) return NULL;
    fp = popen(cmd, "r");
    if (!fp) { free(buf); return NULL; }
    if (!fgets(buf, MAX_LINE, fp)) { pclose(fp); free(buf); return NULL; }
    pclose(fp);
    trim_nl(buf);
    return buf;
}

/* ================ backend ================ */

static char *backend_login(const char *name, const char *key) {
    char cmd[MAX_LINE];
    snprintf(cmd, sizeof(cmd), "%s login '%s' '%s'", SESSION_BIN, name, key);
    return call_output(cmd);
}

static int backend_logout(const char *sid) {
    char cmd[MAX_LINE];
    snprintf(cmd, sizeof(cmd), "%s logout '%s'", SESSION_BIN, sid);
    return call_status(cmd);
}

static char *backend_whoami(const char *sid) {
    char cmd[MAX_LINE];
    snprintf(cmd, sizeof(cmd), "%s whoami '%s'", SESSION_BIN, sid);
    return call_output(cmd);
}

/* ================ validation ================ */

static int valid_key_char(int ch) {
    if (ch >= 'a' && ch <= 'z') return 1;
    if (ch >= 'A' && ch <= 'Z') return 1;
    if (ch >= '0' && ch <= '9') return 1;
    if (ch == '+' || ch == '=' || ch == '_' || ch == '-' || ch == '*') return 1;
    return 0;
}

/* ================ UI — primitives ================ */

static void clear_screen(void) {
    fputs(ANSI_CLR, stdout);
    fflush(stdout);
}

static void move_cursor(int row, int col) {
    printf("\033\x5B%d;%dH", row, col);
}

static void print_inverted_full(const char *text) {
    int len = (int)strlen(text);
    int pad = term_cols - len;
    int i;
    if (pad < 0) pad = 0;
    fputs(ANSI_REV, stdout);
    fputs(text, stdout);
    for (i = 0; i < pad; i++) putchar(' ');
    fputs(ANSI_RST, stdout);
}

static void print_empty_inverted(void) {
    int i;
    fputs(ANSI_REV, stdout);
    for (i = 0; i < term_cols; i++) putchar(' ');
    fputs(ANSI_RST, stdout);
}

static void build_header(char *out, size_t outsz,
                         const char *title, const char *step) {
    if (title && *title && step && *step)
        snprintf(out, outsz, " %s > %s", title, step);
    else if (step && *step)
        snprintf(out, outsz, " %s", step);
    else if (title && *title)
        snprintf(out, outsz, " %s", title);
    else
        snprintf(out, outsz, " ");
}

/* ================ UI — frames ================ */

static void render_login_frame(const char *header, const char *input, int cursor_on) {
    get_term_size();
    fputs(ANSI_HIDE, stdout);
    move_cursor(1, 1);
    print_inverted_full(header);
    move_cursor(2, 1);
    fputs(" > ", stdout);
    fputs(input, stdout);
    if (cursor_on) {
        fputs(ANSI_REV, stdout); putchar(' '); fputs(ANSI_RST, stdout);
    } else {
        putchar(' ');
    }
    fputs(ANSI_CL, stdout);
    move_cursor(term_rows, 1);
    print_empty_inverted();
    move_cursor(term_rows, term_cols);
    fflush(stdout);
}

static void render_message_frame(const char *header, const char *message) {
    get_term_size();
    fputs(ANSI_HIDE, stdout);
    move_cursor(1, 1);
    print_inverted_full(header);
    move_cursor(2, 1);
    fputs(" > ", stdout);
    fputs(message, stdout);
    fputs(ANSI_CL, stdout);
    move_cursor(term_rows, 1);
    print_empty_inverted();
    move_cursor(term_rows, term_cols);
    fflush(stdout);
}

static void show_message(const char *header, const char *message, int frames) {
    int i;
    for (i = 0; i < frames; i++) {
        render_message_frame(header, message);
        sleep_us(FRAME_USEC);
    }
}

static void show_result(const char *title, const char *step, int res) {
    char header[MAX_LINE];
    const char *msg = (res == 0) ? "success" : "fail";
    int i;
    build_header(header, sizeof(header), title, step);
    for (i = 0; i < FPS; i++) {
        render_message_frame(header, msg);
        sleep_us(FRAME_USEC);
    }
}

/* ================ UI — prompt ================ */

static int prompt_line(const char *title, const char *step,
                       char *buf, size_t bufsz, int filter_mode) {
    char header[MAX_LINE];
    size_t pos = 0;
    int ch, frame = 0, cursor_on = 1;

    build_header(header, sizeof(header), title, step);
    *buf = 0;
    clear_screen();

    while (1) {
        frame++;
        if (frame >= CURSOR_BLINK) { frame = 0; cursor_on = !cursor_on; }
        render_login_frame(header, buf, cursor_on);

        while (input_available()) {
            ch = read_char();
            if (ch < 0) continue;

            if (ch == '\n' || ch == '\r') return 1;
            if (ch == CTRL_D) return 0;
            if (ch == CTRL_C || ch == ESC) {
                while (input_available()) read_char();
                return -1;
            }
            if (ch == 127 || ch == 8) {
                if (pos > 0) buf[--pos] = 0;
                cursor_on = 1; frame = 0;
                continue;
            }
            if (ch >= 32 && ch < 127 && pos < bufsz - 1) {
                if (filter_mode == 1 && !valid_key_char(ch)) continue;
                buf[pos++] = (char)ch;
                buf[pos] = 0;
                cursor_on = 1; frame = 0;
            }
        }
        sleep_us(FRAME_USEC);
    }
}

/* ================ session-shell header — status RIGHT ================ */

static void format_uptime(long secs, char *out, size_t outsz) {
    long h = secs / 3600;
    long m = (secs % 3600) / 60;
    long s = secs % 60;
    snprintf(out, outsz, "%02ld:%02ld:%02ld", h, m, s);
}

static void render_session_header_color(const char *name, const char *ustr) {
    int left  = 14;
    int right = 1 + (int)strlen(name) + 3 + (int)strlen(ustr) + 1;
    int pad = term_cols - left - right;
    int i;
    if (pad < 0) pad = 0;

    fputs(ANSI_REV, stdout);
    fputs(" ctrl + ", stdout);
    fputs(ANSI_RREV, stdout); putchar('l');
    fputs(ANSI_RST ANSI_REV, stdout); fputs("ogout", stdout);
    for (i = 0; i < pad; i++) putchar(' ');
    printf(" %s @ %s ", name, ustr);
    fputs(ANSI_RST, stdout);
}

static void render_session_header_plain(const char *name, const char *ustr) {
    char buf[MAX_LINE];
    int left  = 14;
    int right = 1 + (int)strlen(name) + 3 + (int)strlen(ustr) + 1;
    int pad = term_cols - left - right;
    if (pad < 0) pad = 0;
    snprintf(buf, sizeof(buf), "ctrl + logout%*s %s @ %s ",
             pad, "", name, ustr);
    print_inverted_full(buf);
}

static void render_session_header(const char *username, long uptime) {
    char ustr[32];
    format_uptime(uptime, ustr, sizeof(ustr));
    if (use_color) render_session_header_color(username, ustr);
    else           render_session_header_plain(username, ustr);
}

static void render_session_frame(const char *username, long uptime,
                                 const char *input, int cursor_on) {
    get_term_size();
    fputs(ANSI_HIDE, stdout);
    move_cursor(1, 1);
    render_session_header(username, uptime);
    move_cursor(2, 1);
    fputs(" > ", stdout);
    fputs(input, stdout);
    if (cursor_on) {
        fputs(ANSI_REV, stdout); putchar(' '); fputs(ANSI_RST, stdout);
    } else {
        putchar(' ');
    }
    fputs(ANSI_CL, stdout);
    move_cursor(term_rows, 1);
    print_empty_inverted();
    move_cursor(term_rows, term_cols);
    fflush(stdout);
}

/* ================ session shell ================ */

static void run_check(void) {
    term_restore();
    printf("\n");
    fflush(stdout);
    system(CHECK_BIN);
    printf("\n-- press any key to continue --\n");
    fflush(stdout);
    term_raw_mode();
    while (input_available()) read_char();
    while (!input_available()) sleep_us(FRAME_USEC);
    read_char();
    while (input_available()) read_char();
}

static int session_shell(const char *session_id) {
    char *username;
    char input[INPUT_BUF_SZ];
    size_t pos;
    int ch, fc, cur, run;
    long start, now, upt;

    username = backend_whoami(session_id);
    if (!username) return 1;

    start = time(NULL); run = 1; pos = 0; fc = 0; cur = 1; *input = 0;
    clear_screen();

    while (run) {
        now = time(NULL); upt = now - start;
        fc++; if (fc >= CURSOR_BLINK) { fc = 0; cur = !cur; }
        render_session_frame(username, upt, input, cur);

        while (input_available()) {
            ch = read_char(); if (ch < 0) continue;

            if (ch == CTRL_L) { run = 0; continue; }

            if (ch == '\n' || ch == '\r') {
                if (!strcmp(input, "logout") || !strcmp(input, "exit") || !strcmp(input, "quit"))
                    run = 0;
                else if (!strcmp(input, "check")) {
                    run_check();
                    pos = 0; *input = 0; clear_screen();
                    continue;
                } else if (*input) {
                    show_message(" error", "unknown command", FPS / 2);
                }
                pos = 0; *input = 0; clear_screen();
                continue;
            }

            if (ch == CTRL_D || ch == CTRL_C || ch == ESC) {
                while (input_available()) read_char();
                run = 0; continue;
            }

            if (ch == 127 || ch == 8) {
                if (pos > 0) { pos--; *(input + pos) = 0; }
                cur = 1; fc = 0; continue;
            }

            if (ch >= 32 && ch < 127 && pos < INPUT_BUF_SZ - 1) {
                *(input + pos) = (char)ch; pos++; *(input + pos) = 0;
                cur = 1; fc = 0;
            }
        }
        sleep_us(FRAME_USEC);
    }
    free(username);
    return 0;
}

/* ================ main flow ================ */

static int do_login(void) {
    char name[INPUT_BUF_SZ], key[INPUT_BUF_SZ], *sid;
    int r;

    r = prompt_line("session > login", "name", name, sizeof(name), 0);
    if (r <= 0) return r;
    r = prompt_line("session > login", "key",  key,  sizeof(key),  1);
    if (r <= 0) return r;

    sid = backend_login(name, key);
    if (!sid) { show_result("session > login", NULL, 1); return 1; }

    show_result("session > login", NULL, 0);
    session_shell(sid);
    backend_logout(sid);
    free(sid);
    return 1;
}

static int interactive_mode(void) {
    int run = 1, r;
    use_color = detect_color_support();
    term_raw_mode();
    clear_screen();
    while (run) { r = do_login(); if (r <= 0) run = 0; }
    term_restore();
    fputs(ANSI_SHOW, stdout);
    clear_screen();
    return 0;
}

int main(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "-dumb") == 0) force_dumb = 1;
    setvbuf(stdout, NULL, _IONBF, 0);
    return interactive_mode();
}
