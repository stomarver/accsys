/* ===== manager_cli — unified (2026-06-18) ===== */
/* register / rename / rekey (no login, no exit) */

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

#define MANAGER_BIN    "./manager"
#define MAX_LINE       4096
#define INPUT_BUF_SZ   256
#define FPS            20
#define FRAME_USEC     (1000000 / FPS)
#define CURSOR_BLINK   10

#define CTRL_D  4
#define CTRL_C  3
#define CTRL_G  7
#define CTRL_K  11
#define CTRL_N  14
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



/* ================ manager backend ================ */





static int call_manager(char *const args[]) {
    pid_t pid;
    int status;
    if ((pid = fork()) == -1) return 1;
    if (pid == 0) {
        execv(args[0], args);
        exit(1);
    }
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int backend_register(const char *name, const char *key) {
    char *args[] = { MANAGER_BIN, "register", (char *)name, (char *)key, NULL };
    return call_manager(args);
}

static int backend_rename(const char *name, const char *key, const char *new_name) {
    char *args[] = { MANAGER_BIN, "rename", (char *)name, (char *)key, (char *)new_name, NULL };
    return call_manager(args);
}

static int backend_rekey(const char *name, const char *old_key, const char *new_key) {
    char *args[] = { MANAGER_BIN, "rekey", (char *)name, (char *)old_key, (char *)new_key, NULL };
    return call_manager(args);
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
    fputs(ANSI_CL, stdout);
    move_cursor(3, 1);
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
    fputs(ANSI_CL, stdout);
    move_cursor(3, 1);
    fputs(" > ", stdout);
    fputs(message, stdout);
    fputs(ANSI_CL, stdout);
    move_cursor(term_rows, 1);
    print_empty_inverted();
    move_cursor(term_rows, term_cols);
    fflush(stdout);
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

/* ================ UI — menu frame ================ */

static void render_menu_frame(const char *input, int cursor_on) {
    int pad, i;

    get_term_size();
    fputs(ANSI_HIDE, stdout);

    move_cursor(1, 1);

    if (use_color) {
        int fixed = 33;  /* visible length of " ctrl + register / rename / rekey" */
        pad = term_cols - fixed;
        if (pad < 0) pad = 0;

        fputs(ANSI_REV, stdout);
        fputs(" ctrl + re", stdout);

        fputs(ANSI_RREV, stdout); putchar('g');
        fputs(ANSI_RST ANSI_REV, stdout);
        fputs("ister / re", stdout);

        fputs(ANSI_RREV, stdout); putchar('n');
        fputs(ANSI_RST ANSI_REV, stdout);
        fputs("ame / re", stdout);

        fputs(ANSI_RREV, stdout); putchar('k');
        fputs(ANSI_RST ANSI_REV, stdout);
        fputs("ey", stdout);

        for (i = 0; i < pad; i++) putchar(' ');
        fputs(ANSI_RST, stdout);
    } else {
        print_inverted_full(" ctrl + register / rename / rekey");
    }

    move_cursor(2, 1);
    fputs(ANSI_CL, stdout);

    move_cursor(3, 1);
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

static int prompt_menu_choice(char *buf, size_t bufsz) {
    size_t pos = 0;
    int ch, frame = 0, cursor_on = 1;

    *buf = 0;
    clear_screen();

    while (1) {
        frame++;
        if (frame >= CURSOR_BLINK) { frame = 0; cursor_on = !cursor_on; }
        render_menu_frame(buf, cursor_on);

        while (input_available()) {
            ch = read_char();
            if (ch < 0) continue;

            if (ch == '\n' || ch == '\r') return 1;
            if (ch == CTRL_D) return 0;
            if (ch == CTRL_C) {
                while (input_available()) read_char();
                return -1;
            }
            if (ch == ESC) {
                if (input_available()) {
                    while (input_available()) read_char();
                    cursor_on = 1; frame = 0;
                    continue;
                } else {
                    return -1;
                }
            }

            if (ch == CTRL_G) { strcpy(buf, "register"); return 1; }
            if (ch == CTRL_N) { strcpy(buf, "rename");   return 1; }
            if (ch == CTRL_K) { strcpy(buf, "rekey");    return 1; }

            if (ch == 127 || ch == 8) {
                if (pos > 0) buf[--pos] = 0;
                cursor_on = 1; frame = 0;
                continue;
            }
            if (ch >= 32 && ch != 127 && pos < bufsz - 1) {
                buf[pos++] = (char)ch;
                buf[pos] = 0;
                cursor_on = 1; frame = 0;
            }
        }
        sleep_us(FRAME_USEC);
    }
}

/* ================ UI — prompt ================ */

static int prompt_line(const char *title, const char *step,
                       char *buf, size_t bufsz) {
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
            if (ch == CTRL_C) {
                while (input_available()) read_char();
                return -1;
            }
            if (ch == ESC) {
                if (input_available()) {
                    while (input_available()) read_char();
                    cursor_on = 1; frame = 0;
                    continue;
                } else {
                    return -1;
                }
            }
            if (ch == 127 || ch == 8) {
                if (pos > 0) buf[--pos] = 0;
                cursor_on = 1; frame = 0;
                continue;
            }
            if (ch >= 32 && ch != 127 && pos < bufsz - 1) {
                buf[pos++] = (char)ch;
                buf[pos] = 0;
                cursor_on = 1; frame = 0;
            }
        }
        sleep_us(FRAME_USEC);
    }
}

/* ================ interactive mode ================ */

static int interactive_mode(void) {
    char choice[INPUT_BUF_SZ];
    char name[INPUT_BUF_SZ];
    char key[INPUT_BUF_SZ];
    char new_val[INPUT_BUF_SZ];
    int run = 1, r;

    use_color = detect_color_support();
    term_raw_mode();
    clear_screen();

    while (run) {
        r = prompt_menu_choice(choice, sizeof(choice));
        if (r <= 0) { run = 0; continue; }

        if (strcmp(choice, "register") == 0 || strcmp(choice, "1") == 0) {
            r = prompt_line("register", "name", name, sizeof(name));
            if (r <= 0) { run = 0; continue; }
            r = prompt_line("register", "key", key, sizeof(key));
            if (r <= 0) { run = 0; continue; }
            show_result("register", NULL, backend_register(name, key));
            continue;
        }

        if (strcmp(choice, "rename") == 0 || strcmp(choice, "2") == 0) {
            r = prompt_line("rename", "name", name, sizeof(name));
            if (r <= 0) { run = 0; continue; }
            r = prompt_line("rename", "key", key, sizeof(key));
            if (r <= 0) { run = 0; continue; }
            r = prompt_line("rename", "new name", new_val, sizeof(new_val));
            if (r <= 0) { run = 0; continue; }
            show_result("rename", NULL, backend_rename(name, key, new_val));
            continue;
        }

        if (strcmp(choice, "rekey") == 0 || strcmp(choice, "3") == 0) {
            r = prompt_line("rekey", "name", name, sizeof(name));
            if (r <= 0) { run = 0; continue; }
            r = prompt_line("rekey", "current key", key, sizeof(key));
            if (r <= 0) { run = 0; continue; }
            r = prompt_line("rekey", "new key", new_val, sizeof(new_val));
            if (r <= 0) { run = 0; continue; }
            show_result("rekey", NULL, backend_rekey(name, key, new_val));
            continue;
        }
    }

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
