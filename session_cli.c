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

#define SESSION_BIN    "./session"
#define MAX_LINE       4096
#define INPUT_BUF_SZ   256
#define SESSION_ID_LEN 32
#define FPS            20
#define FRAME_USEC     (1000000 / FPS)
#define CURSOR_BLINK   10

#define CTRL_L  12
#define CTRL_W  23

/* ---------- terminal ---------- */

static int term_rows = 24;
static int term_cols = 80;
static int term_colors = 0;
static int force_dumb = 0;
static struct termios orig_termios;
static int raw_mode_enabled = 0;

static void get_term_size(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_row > 0) term_rows = ws.ws_row;
        if (ws.ws_col > 0) term_cols = ws.ws_col;
    }
}

static int detect_color_support(void) {
    const char *term;
    const char *colorterm;

    if (force_dumb) return 0;
    if (!isatty(STDOUT_FILENO)) return 0;

    colorterm = getenv("COLORTERM");
    if (colorterm && *colorterm) return 1;

    term = getenv("TERM");
    if (!term || !*term) return 0;

    if (strcmp(term, "dumb") == 0) return 0;
    if (strstr(term, "color")) return 1;
    if (strstr(term, "xterm")) return 1;
    if (strstr(term, "screen")) return 1;
    if (strstr(term, "tmux")) return 1;
    if (strstr(term, "rxvt")) return 1;
    if (strstr(term, "linux")) return 1;
    if (strstr(term, "vte")) return 1;
    if (strstr(term, "konsole")) return 1;
    if (strstr(term, "gnome")) return 1;
    if (strstr(term, "alacritty")) return 1;
    if (strstr(term, "kitty")) return 1;

    return 0;
}

static void sleep_us(long us) {
    struct timespec ts;
    ts.tv_sec = us / 1000000;
    ts.tv_nsec = (us % 1000000) * 1000;
    nanosleep(&ts, NULL);
}

static void term_raw_mode(void) {
    struct termios raw;
    if (raw_mode_enabled) return;
    if (tcgetattr(STDIN_FILENO, &orig_termios) < 0) return;
    raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_mode_enabled = 1;
}

static void term_restore(void) {
    if (raw_mode_enabled) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        raw_mode_enabled = 0;
    }
}

static int input_available(void) {
    struct timeval tv;
    fd_set fds;
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

static int read_char(void) {
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) == 1) {
        return c;
    }
    return -1;
}

/* ---------- key validation ---------- */

static int is_valid_key_char(int ch) {
    if (ch >= 'a' && ch <= 'z') return 1;
    if (ch >= 'A' && ch <= 'Z') return 1;
    if (ch >= '0' && ch <= '9') return 1;
    if (ch == '+' || ch == '=' || ch == '_' || ch == '-' || ch == '*') return 1;
    return 0;
}

/* ---------- backend calls ---------- */

static void trim_nl(char *s) {
    size_t n = strlen(s);
    while (n && (*(s + n - 1) == '\n' || *(s + n - 1) == '\r')) {
        --n;
        *(s + n) = '\0';
    }
}

static int call_session_exit(const char *cmd) {
    int ret = system(cmd);
    if (ret == -1) return 1;
    return WEXITSTATUS(ret);
}

static char *call_session_output(const char *cmd) {
    FILE *fp;
    char *result;

    result = malloc(MAX_LINE);
    if (!result) return NULL;

    fp = popen(cmd, "r");
    if (!fp) {
        free(result);
        return NULL;
    }

    if (!fgets(result, MAX_LINE, fp)) {
        pclose(fp);
        free(result);
        return NULL;
    }

    pclose(fp);
    trim_nl(result);
    return result;
}

static char *backend_login(const char *name, const char *key) {
    char cmd[MAX_LINE];
    snprintf(cmd, MAX_LINE, "%s login '%s' '%s'", SESSION_BIN, name, key);
    return call_session_output(cmd);
}

static int backend_logout(const char *session_id) {
    char cmd[MAX_LINE];
    snprintf(cmd, MAX_LINE, "%s logout '%s'", SESSION_BIN, session_id);
    return call_session_exit(cmd);
}

static int backend_touch(const char *session_id) {
    char cmd[MAX_LINE];
    snprintf(cmd, MAX_LINE, "%s touch '%s'", SESSION_BIN, session_id);
    return call_session_exit(cmd);
}

static char *backend_whoami(const char *session_id) {
    char cmd[MAX_LINE];
    snprintf(cmd, MAX_LINE, "%s whoami '%s'", SESSION_BIN, session_id);
    return call_session_output(cmd);
}

/* ---------- ui ---------- */

#define ANSI_REVERSE    "\033\x5B" "7m"
#define ANSI_RESET      "\033\x5B" "0m"
#define ANSI_RED_REV    "\033\x5B" "91;7m"
#define ANSI_HIDE_CUR   "\033\x5B" "?25l"
#define ANSI_SHOW_CUR   "\033\x5B" "?25h"
#define ANSI_CLEAR_LINE "\033\x5B" "K"

static void clear_screen(void) {
    fputs("\033\x5B" "2J" "\033\x5B" "3J" "\033\x5B" "H", stdout);
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

    fputs(ANSI_REVERSE, stdout);
    fputs(text, stdout);
    for (i = 0; i < pad; i++) putchar(' ');
    fputs(ANSI_RESET, stdout);
}

static void print_empty_inverted(void) {
    int i;
    fputs(ANSI_REVERSE, stdout);
    for (i = 0; i < term_cols; i++) putchar(' ');
    fputs(ANSI_RESET, stdout);
}

static void format_uptime(long seconds, char *out, size_t outsz) {
    long h, m, s;
    h = seconds / 3600;
    m = (seconds % 3600) / 60;
    s = seconds % 60;
    snprintf(out, outsz, "%02ld:%02ld:%02ld", h, m, s);
}

static void render_login_frame(const char *header, const char *input, int cursor_on) {
    get_term_size();

    fputs(ANSI_HIDE_CUR, stdout);

    move_cursor(1, 1);
    print_inverted_full(header);

    move_cursor(2, 1);
    fputs(" > ", stdout);
    fputs(input, stdout);

    if (cursor_on) {
        fputs(ANSI_REVERSE, stdout);
        putchar(' ');
        fputs(ANSI_RESET, stdout);
    } else {
        putchar(' ');
    }

    fputs(ANSI_CLEAR_LINE, stdout);

    move_cursor(term_rows, 1);
    print_empty_inverted();

    move_cursor(term_rows, term_cols);
    fflush(stdout);
}

static void render_session_header_color(const char *username, const char *uptime_str) {
    int printed = 0;
    int pad;
    int i;

    /* " username @ HH:MM:SS  |  ctrl + logout / whoami" */
    /* with l and w in red */
    fputs(ANSI_REVERSE, stdout);
    printf(" %s @ %s  |  ctrl + ", username, uptime_str);
    printed = 1 + (int)strlen(username) + 3 + (int)strlen(uptime_str) + 12;

    fputs(ANSI_RED_REV, stdout);
    putchar('l');
    printed += 1;

    fputs(ANSI_RESET ANSI_REVERSE, stdout);
    fputs("ogout / ", stdout);
    printed += 8;

    fputs(ANSI_RED_REV, stdout);
    putchar('w');
    printed += 1;

    fputs(ANSI_RESET ANSI_REVERSE, stdout);
    fputs("hoami", stdout);
    printed += 5;

    pad = term_cols - printed;
    if (pad < 0) pad = 0;
    for (i = 0; i < pad; i++) putchar(' ');

    fputs(ANSI_RESET, stdout);
}

static void render_session_header_plain(const char *username, const char *uptime_str) {
    char header[MAX_LINE];
    snprintf(header, MAX_LINE, " %s @ %s  |  logout / whoami / help", username, uptime_str);
    print_inverted_full(header);
}

static void render_session_header(const char *username, long uptime) {
    char uptime_str[32];
    format_uptime(uptime, uptime_str, sizeof(uptime_str));

    if (term_colors) {
        render_session_header_color(username, uptime_str);
    } else {
        render_session_header_plain(username, uptime_str);
    }
}

static void render_session_frame(const char *username, long uptime, const char *input, int cursor_on) {
    get_term_size();

    fputs(ANSI_HIDE_CUR, stdout);

    move_cursor(1, 1);
    render_session_header(username, uptime);

    move_cursor(2, 1);
    fputs(" > ", stdout);
    fputs(input, stdout);

    if (cursor_on) {
        fputs(ANSI_REVERSE, stdout);
        putchar(' ');
        fputs(ANSI_RESET, stdout);
    } else {
        putchar(' ');
    }

    fputs(ANSI_CLEAR_LINE, stdout);

    move_cursor(term_rows, 1);
    print_empty_inverted();

    move_cursor(term_rows, term_cols);
    fflush(stdout);
}

static void render_message_frame(const char *header, const char *message) {
    get_term_size();

    fputs(ANSI_HIDE_CUR, stdout);

    move_cursor(1, 1);
    print_inverted_full(header);

    move_cursor(2, 1);
    fputs(" > ", stdout);
    fputs(message, stdout);
    fputs(ANSI_CLEAR_LINE, stdout);

    move_cursor(term_rows, 1);
    print_empty_inverted();

    move_cursor(term_rows, term_cols);
    fflush(stdout);
}

static int prompt_line_filtered(const char *header, char *buf, size_t bufsz, int filter_mode) {
    size_t pos = 0;
    int ch;
    int frame_count = 0;
    int cursor_on = 1;

    *buf = '\0';
    clear_screen();

    while (1) {
        frame_count++;
        if (frame_count >= CURSOR_BLINK) {
            frame_count = 0;
            cursor_on = !cursor_on;
        }

        render_login_frame(header, buf, cursor_on);

        while (input_available()) {
            ch = read_char();
            if (ch < 0) continue;

            if (ch == '\n' || ch == '\r') return 1;
            if (ch == 4) return 0;

            if (ch == 3 || ch == 27) {
                while (input_available()) read_char();
                return -1;
            }

            if (ch == 127 || ch == 8) {
                if (pos > 0) {
                    pos--;
                    *(buf + pos) = '\0';
                }
                cursor_on = 1;
                frame_count = 0;
                continue;
            }

            if (ch >= 32 && ch < 127 && pos < bufsz - 1) {
                if (filter_mode == 1 && !is_valid_key_char(ch)) {
                    continue;
                }
                *(buf + pos) = (char)ch;
                pos++;
                *(buf + pos) = '\0';
                cursor_on = 1;
                frame_count = 0;
            }
        }

        sleep_us(FRAME_USEC);
    }
}

static void show_message(const char *header, const char *message, int frames) {
    int i;
    for (i = 0; i < frames; i++) {
        render_message_frame(header, message);
        sleep_us(FRAME_USEC);
    }
}

/* ---------- session shell ---------- */

static int session_shell(const char *session_id) {
    char *username;
    char input[INPUT_BUF_SZ];
    size_t pos;
    int ch;
    int frame_count;
    int cursor_on;
    int running;
    long session_start;
    long now;
    long uptime;
    char *whoami_result;

    username = backend_whoami(session_id);
    if (!username) {
        return 1;
    }

    session_start = time(NULL);
    running = 1;
    pos = 0;
    frame_count = 0;
    cursor_on = 1;
    *input = '\0';

    clear_screen();

    while (running) {
        now = time(NULL);
        uptime = now - session_start;

        frame_count++;
        if (frame_count >= CURSOR_BLINK) {
            frame_count = 0;
            cursor_on = !cursor_on;
        }

        render_session_frame(username, uptime, input, cursor_on);

        backend_touch(session_id);

        while (input_available()) {
            ch = read_char();
            if (ch < 0) continue;

            /* ctrl+l -> logout */
            if (ch == CTRL_L) {
                running = 0;
                continue;
            }

            /* ctrl+w -> whoami */
            if (ch == CTRL_W) {
                whoami_result = backend_whoami(session_id);
                if (whoami_result) {
                    show_message(" whoami", whoami_result, FPS);
                    free(whoami_result);
                }
                pos = 0;
                *input = '\0';
                clear_screen();
                continue;
            }

            /* enter -> execute command */
            if (ch == '\n' || ch == '\r') {
                if (strcmp(input, "logout") == 0 || strcmp(input, "exit") == 0 || strcmp(input, "quit") == 0) {
                    running = 0;
                } else if (strcmp(input, "whoami") == 0) {
                    whoami_result = backend_whoami(session_id);
                    if (whoami_result) {
                        show_message(" whoami", whoami_result, FPS);
                        free(whoami_result);
                    }
                } else if (strcmp(input, "uptime") == 0) {
                    char uptime_msg[64];
                    format_uptime(uptime, uptime_msg, sizeof(uptime_msg));
                    show_message(" uptime", uptime_msg, FPS);
                } else if (strcmp(input, "help") == 0) {
                    show_message(" help", "whoami / uptime / logout", FPS);
                } else if (strlen(input) > 0) {
                    show_message(" error", "unknown command", FPS / 2);
                }

                pos = 0;
                *input = '\0';
                clear_screen();
                continue;
            }

            /* ctrl+d, ctrl+c, esc -> logout */
            if (ch == 4 || ch == 3 || ch == 27) {
                while (input_available()) read_char();
                running = 0;
                continue;
            }

            /* backspace */
            if (ch == 127 || ch == 8) {
                if (pos > 0) {
                    pos--;
                    *(input + pos) = '\0';
                }
                cursor_on = 1;
                frame_count = 0;
                continue;
            }

            /* printable */
            if (ch >= 32 && ch < 127 && pos < INPUT_BUF_SZ - 1) {
                *(input + pos) = (char)ch;
                pos++;
                *(input + pos) = '\0';
                cursor_on = 1;
                frame_count = 0;
            }
        }

        sleep_us(FRAME_USEC);
    }

    free(username);
    return 0;
}

/* ---------- main flow ---------- */

static int do_login(void) {
    char name[INPUT_BUF_SZ];
    char key[INPUT_BUF_SZ];
    char *session_id;
    int r;

    r = prompt_line_filtered(" session > login > name", name, INPUT_BUF_SZ, 0);
    if (r <= 0) return r;

    r = prompt_line_filtered(" session > login > key", key, INPUT_BUF_SZ, 1);
    if (r <= 0) return r;

    session_id = backend_login(name, key);
    if (!session_id) {
        show_message("
