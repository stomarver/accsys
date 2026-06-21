/* ===== client_cli — login client UI ===== */
/* 1. Select server from scanned list (highlighting). */
/* 2. Enter name + key. */
/* 3. Connect to server and authenticate remotely. */
/* 4. Show status shell. */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../shared/plugins/plugin_engine.c"

#define MAX_LINE       4096
#define INPUT_BUF_SZ   256
#define FPS            20
#define FRAME_USEC     (1000000 / FPS)
#define CURSOR_BLINK   10
#define DEFAULT_PORT   9000
#define SCAN_START     9000
#define SCAN_END       9010
#define SCAN_TIMEOUT_MS 200
#define HEARTBEAT_SEC  5
#define MAX_NAME       64
#define MAX_SERVERS    32

#define CTRL_D  4
#define CTRL_C  3
#define CTRL_L  12
#define CTRL_P  16
#define CTRL_T  20
#define ESC     27
#define ARROW_UP    'A'
#define ARROW_DOWN  'B'

#define ANSI_REV  "\033\x5B" "7m"
#define ANSI_RST  "\033\x5B" "0m"
#define ANSI_RREV "\033\x5B" "91;7m"
#define ANSI_RED  "\033\x5B" "91m"
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

/* connection state */
static int client_fd = -1;
static char client_host[INPUT_BUF_SZ];
static int client_port = 0;
static time_t client_connected = 0;
static char client_name[MAX_NAME + 1];

static volatile int running = 1;

/* scanned server list */
typedef struct {
    char host[64];
    int port;
} server_t;
static server_t servers[MAX_SERVERS];
static int server_count = 0;

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

/* ================ utils ================ */

static int is_valid_name(const char *s) {
    size_t n = strlen(s);
    size_t i;
    if (n == 0 || n > MAX_NAME) return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!(isalnum(c) || c == '_' || c == '-' || c == '.')) return 0;
    }
    return 1;
}

static int is_valid_key(const char *s) {
    size_t n = strlen(s);
    size_t i;
    if (n == 0) return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'a' && c <= 'z') continue;
        if (c >= 'A' && c <= 'Z') continue;
        if (c >= '0' && c <= '9') continue;
        if (c == '+' || c == '=' || c == '_' || c == '-' || c == '*') continue;
        return 0;
    }
    return 1;
}

/* ================ server discovery ================ */

static int try_server(const char *host, int port) {
    int fd;
    struct sockaddr_in addr;
    fd_set rfds;
    struct timeval tv;
    char buf[64];
    ssize_t r;
    size_t sent, total;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        close(fd);
        return 0;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return 0;
    }

    total = strlen("ping\n");
    sent = 0;
    while (sent < total) {
        r = write(fd, "ping\n" + sent, total - sent);
        if (r <= 0) { close(fd); return 0; }
        sent += (size_t)r;
    }

    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    tv.tv_sec = 0;
    tv.tv_usec = SCAN_TIMEOUT_MS * 1000;

    r = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (r <= 0) { close(fd); return 0; }

    r = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (r <= 0) return 0;
    buf[r] = '\0';
    return strstr(buf, "pong") != NULL || strstr(buf, "uptime") != NULL;
}

static void scan_servers(void) {
    int port;
    server_count = 0;
    for (port = SCAN_START; port <= SCAN_END && server_count < MAX_SERVERS; port++) {
        if (try_server("127.0.0.1", port)) {
            strncpy(servers[server_count].host, "127.0.0.1", sizeof(servers[server_count].host) - 1);
            servers[server_count].host[sizeof(servers[server_count].host) - 1] = '\0';
            servers[server_count].port = port;
            server_count++;
        }
    }
}

/* ================ network ================ */

static int connect_server(const char *host, int port) {
    int fd;
    struct sockaddr_in addr;

    if (client_fd != -1) {
        close(client_fd);
        client_fd = -1;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    client_fd = fd;
    client_port = port;
    if (client_host != host) {
        strncpy(client_host, host, sizeof(client_host) - 1);
        client_host[sizeof(client_host) - 1] = '\0';
    }
    client_connected = time(NULL);
    return 0;
}

static int send_server(const char *msg) {
    size_t len = strlen(msg);
    ssize_t r;
    size_t sent = 0;
    if (client_fd == -1) return -1;
    while (sent < len) {
        r = write(client_fd, msg + sent, len - sent);
        if (r <= 0) return -1;
        sent += (size_t)r;
    }
    return 0;
}

static int do_disconnect(void) {
    if (client_fd == -1) return 1;
    close(client_fd);
    client_fd = -1;
    client_port = 0;
    client_host[0] = '\0';
    client_connected = 0;
    client_name[0] = '\0';
    return 0;
}

/* ================ UI primitives ================ */

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

static void build_header(char *out, size_t outsz, const char *title, const char *step) {
    if (title && *title && step && *step)
        snprintf(out, outsz, " %s > %s", title, step);
    else if (step && *step)
        snprintf(out, outsz, " %s", step);
    else if (title && *title)
        snprintf(out, outsz, " %s", title);
    else
        snprintf(out, outsz, " ");
}

static void render_header(const char *header) {
    move_cursor(1, 1);
    print_inverted_full(header);
}

static void clear_line(int row) {
    move_cursor(row, 1);
    fputs(ANSI_CL, stdout);
}

static void render_status_footer(void) {
    move_cursor(term_rows, 1);
    print_empty_inverted();
}

/* ================ UI prompts ================ */

static int prompt_line(const char *title, const char *step,
                       char *buf, size_t bufsz, int numeric) {
    char header[MAX_LINE];
    size_t pos = 0;
    int ch, frame = 0, cursor_on = 1;

    build_header(header, sizeof(header), title, step);
    *buf = 0;
    clear_screen();

    while (running) {
        frame++;
        if (frame >= CURSOR_BLINK) { frame = 0; cursor_on = !cursor_on; }

        get_term_size();
        fputs(ANSI_HIDE, stdout);
        render_header(header);
        clear_line(2);
        move_cursor(3, 1);
        fputs(" > ", stdout);
        fputs(buf, stdout);
        if (cursor_on) {
            fputs(ANSI_REV, stdout); putchar(' '); fputs(ANSI_RST, stdout);
        } else {
            putchar(' ');
        }
        fputs(ANSI_CL, stdout);
        render_status_footer();
        fflush(stdout);

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
                if (numeric && !isdigit((unsigned char)ch)) continue;
                buf[pos++] = (char)ch;
                buf[pos] = 0;
                cursor_on = 1; frame = 0;
            }
        }
        sleep_us(FRAME_USEC);
    }
    return -1;
}

static void show_result(const char *title, const char *step, int res) {
    char header[MAX_LINE];
    const char *msg = (res == 0) ? "success" : "fail";
    int i;
    build_header(header, sizeof(header), title, step);
    for (i = 0; i < FPS; i++) {
        get_term_size();
        fputs(ANSI_HIDE, stdout);
        render_header(header);
        clear_line(2);
        move_cursor(3, 1);
        fputs(" > ", stdout);
        fputs(msg, stdout);
        fputs(ANSI_CL, stdout);
        render_status_footer();
        fflush(stdout);
        sleep_us(FRAME_USEC);
    }
}

/* ================ server list selection ================ */

static void render_server_list(int selected) {
    int i, row;
    char line[MAX_LINE];

    get_term_size();
    fputs(ANSI_HIDE, stdout);
    render_header(" client > select server");

    for (i = 0; i < MAX_SERVERS; i++) {
        row = 3 + i;
        if (row >= term_rows - 1) break;
        if (i >= server_count) {
            move_cursor(row, 1);
            fputs(ANSI_CL, stdout);
            continue;
        }
        snprintf(line, sizeof(line), "  %s:%d", servers[i].host, servers[i].port);
        move_cursor(row, 1);
        if (i == selected) {
            if (use_color) {
                fputs(ANSI_RREV, stdout);
            } else {
                fputs(ANSI_REV, stdout);
            }
            fputs(line, stdout);
            int pad = term_cols - (int)strlen(line);
            if (pad < 0) pad = 0;
            while (pad-- > 0) putchar(' ');
            fputs(ANSI_RST, stdout);
        } else {
            fputs(line, stdout);
            fputs(ANSI_CL, stdout);
        }
    }
    for (; row < term_rows - 1; row++) {
        move_cursor(row, 1);
        fputs(ANSI_CL, stdout);
    }

    move_cursor(term_rows, 1);
    fputs(ANSI_REV, stdout);
    fputs(" up/down = move, enter = select, esc = quit ", stdout);
    int pad = term_cols - 45;
    if (pad < 0) pad = 0;
    while (pad-- > 0) putchar(' ');
    fputs(ANSI_RST, stdout);
    fflush(stdout);
}

static int prompt_server_select(void) {
    int selected = 0;
    int ch, arrow;

    clear_screen();
    if (server_count == 0) {
        show_result("client", "no servers found", 1);
        return -1;
    }

    while (running) {
        render_server_list(selected);
        while (input_available()) {
            ch = read_char();
            if (ch < 0) continue;

            if (ch == '\n' || ch == '\r') return selected;
            if (ch == CTRL_C || ch == CTRL_D) return -1;

            if (ch == ESC) {
                if (!input_available()) return -1;
                arrow = read_char();
                if (arrow != '[') {
                    while (input_available()) read_char();
                    return -1;
                }
                if (!input_available()) continue;
                arrow = read_char();
                if (arrow == ARROW_UP) {
                    if (selected > 0) selected--;
                } else if (arrow == ARROW_DOWN) {
                    if (selected < server_count - 1) selected++;
                }
                continue;
            }
            if (ch == 'k' || ch == 'K') {
                if (selected > 0) selected--;
                continue;
            }
            if (ch == 'j' || ch == 'J') {
                if (selected < server_count - 1) selected++;
                continue;
            }
        }
        sleep_us(FRAME_USEC);
    }
    return -1;
}

/* ================ status shell ================ */

static long server_start_offset = -1;
static char status_chat_log[15][256];
static int status_chat_count = 0;

static void render_status_shell(const char *input, int cursor_on) {
    char right_str[128];
    long up = (long)(time(NULL) - client_connected);
    int h = up / 3600, m = (up % 3600) / 60, s = up % 60;
    snprintf(right_str, sizeof(right_str), "%s  %02d:%02d:%02d ", client_name, h, m, s);

    get_term_size();
    fputs(ANSI_HIDE, stdout);

    move_cursor(1, 1);
    if (use_color) {
        int fixed = 31; /* " ctrl + ping / logout / hearts " */
        int right_len = strlen(right_str);
        int pad = term_cols - fixed - right_len;
        if (pad < 0) pad = 0;

        fputs(ANSI_REV, stdout);
        fputs(" ctrl + ", stdout);
        fputs(ANSI_RREV, stdout); putchar('p'); fputs(ANSI_RST ANSI_REV, stdout);
        fputs("ing / ", stdout);
        fputs(ANSI_RREV, stdout); putchar('l'); fputs(ANSI_RST ANSI_REV, stdout);
        fputs("ogout / hear", stdout);
        fputs(ANSI_RREV, stdout); putchar('t'); fputs(ANSI_RST ANSI_REV, stdout);
        fputs("s ", stdout);

        for (int i = 0; i < pad; i++) putchar(' ');
        fputs(right_str, stdout);
        fputs(ANSI_RST, stdout);
    } else {
        char full[MAX_LINE];
        snprintf(full, sizeof(full), " ctrl + ping / logout / hearts ");
        int right_len = strlen(right_str);
        int pad = term_cols - 31 - right_len;
        if (pad < 0) pad = 0;
        fputs(ANSI_REV, stdout);
        fputs(full, stdout);
        for (int i = 0; i < pad; i++) putchar(' ');
        fputs(right_str, stdout);
        fputs(ANSI_RST, stdout);
    }

    clear_line(2);
    move_cursor(3, 1);
    fputs(" > ", stdout);
    fputs(input, stdout);
    if (cursor_on) {
        fputs(ANSI_REV, stdout); putchar(' '); fputs(ANSI_RST, stdout);
    } else {
        putchar(' ');
    }
    fputs(ANSI_CL, stdout);

    int start_i = status_chat_count > 10 ? status_chat_count - 10 : 0;
    for (int ci = 0; ci < 10; ci++) {
        int r_row = 5 + ci;
        if (r_row >= term_rows - 2) break;
        move_cursor(r_row, 3);
        if (start_i + ci < status_chat_count) {
            fputs(status_chat_log[(start_i + ci) % 15], stdout);
        }
        fputs(ANSI_CL, stdout);
    }

    move_cursor(term_rows, 1);
    char bot_str[512];
    if (server_start_offset != -1) {
        long sup = time(NULL) - server_start_offset;
        int sh = sup / 3600, sm = (sup % 3600) / 60, ss = sup % 60;
        snprintf(bot_str, sizeof(bot_str), " %s:%d | uptime: %02d:%02d:%02d ", client_host, client_port, sh, sm, ss);
    } else {
        snprintf(bot_str, sizeof(bot_str), " %s:%d | uptime: --:--:-- ", client_host, client_port);
    }
    int bot_len = strlen(bot_str);
    int bpad = (term_cols - bot_len) / 2;
    if (bpad < 0) bpad = 0;
    int bpad_right = term_cols - bot_len - bpad;
    if (bpad_right < 0) bpad_right = 0;

    fputs(ANSI_REV, stdout);
    for (int i = 0; i < bpad; i++) putchar(' ');
    fputs(bot_str, stdout);
    for (int i = 0; i < bpad_right; i++) putchar(' ');
    fputs(ANSI_RST, stdout);

    plugin_engine_client_render();

    fflush(stdout);
}

static int do_ping(void) {
    fd_set rfds;
    struct timeval tv;
    ssize_t r;
    char buf[256];

    if (client_fd == -1) return 1;
    if (send_server("ping\n") < 0) {
        do_disconnect();
        return 1;
    }
    FD_ZERO(&rfds);
    FD_SET(client_fd, &rfds);
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    r = select(client_fd + 1, &rfds, NULL, NULL, &tv);
    if (r <= 0) return 1;
    r = read(client_fd, buf, sizeof(buf) - 1);
    if (r <= 0) {
        do_disconnect();
        return 1;
    }
    buf[r] = '\0';
    long up;
    if (sscanf(buf, "pong %ld", &up) == 1) {
        server_start_offset = time(NULL) - up;
        return 0;
    }
    return strstr(buf, "pong") != NULL ? 0 : 1;
}

static void add_chat_log_helper(const char *text) {
    strncpy(status_chat_log[status_chat_count % 15], text, 255);
    status_chat_log[status_chat_count % 15][255] = '\0';
    status_chat_count++;
    clear_screen();
}

static int status_shell(void) {
    char input[INPUT_BUF_SZ];
    size_t pos = 0;
    int ch, frame = 0, cursor_on = 1;
    fd_set rfds;
    struct timeval tv;
    char buf[256];
    ssize_t r;

    plugin_client_ctx_t pctx;
    pctx.term_rows = term_rows;
    pctx.term_cols = term_cols;
    pctx.client_fd = client_fd;
    pctx.client_name = client_name;
    pctx.move_cursor = move_cursor;
    pctx.send_server = send_server;
    pctx.add_chat_log = add_chat_log_helper;
    plugin_engine_client_init(&pctx);

    *input = 0;
    clear_screen();

    while (running && client_fd != -1) {
        frame++;
        if (frame >= CURSOR_BLINK) { frame = 0; cursor_on = !cursor_on; }
        render_status_shell(input, cursor_on);

        /* heartbeat: send ping every HEARTBEAT_SEC if input idle */
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(client_fd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = FRAME_USEC;
        r = select(client_fd + 1, &rfds, NULL, NULL, &tv);

        if (r > 0 && FD_ISSET(client_fd, &rfds)) {
            r = read(client_fd, buf, sizeof(buf) - 1);
            if (r <= 0) {
                do_disconnect();
                break;
            }
            buf[r] = '\0';
            char *line_ptr = strtok(buf, "\n");
            while (line_ptr != NULL) {
                long up;
                if (strcmp(line_ptr, "ping") == 0) {
                    send_server("pong\n");
                } else if (sscanf(line_ptr, "uptime %ld", &up) == 1 || sscanf(line_ptr, "pong %ld", &up) == 1) {
                    server_start_offset = time(NULL) - up;
                } else if (strncmp(line_ptr, "OK:", 3) == 0 || strncmp(line_ptr, "ERR:", 4) == 0) {
                    /* control message */
                } else {
                    if (plugin_engine_client_handle_packet(line_ptr)) {
                        line_ptr = strtok(NULL, "\n");
                        continue;
                    }

                    strncpy(status_chat_log[status_chat_count % 15], line_ptr, 255);
                    status_chat_log[status_chat_count % 15][255] = '\0';
                    status_chat_count++;
                    clear_screen();
                }
                line_ptr = strtok(NULL, "\n");
            }
        }

        if (r > 0 && FD_ISSET(STDIN_FILENO, &rfds)) {
            ch = read_char();
            if (ch < 0) continue;

            if (plugin_engine_client_handle_keypress(ch, (int)pos, input)) {
                pos = 0; *input = 0;
                clear_screen();
                continue;
            }

            if (ch == '\n' || ch == '\r') {
                if (strcmp(input, "quit") == 0 || strcmp(input, "logout") == 0 || strcmp(input, "exit") == 0) {
                    do_disconnect();
                    break;
                }
                if (strcmp(input, "ping") == 0) {
                    show_result("client", do_ping() == 0 ? "pong" : "ping failed", do_ping() == 0 ? 0 : 1);
                    pos = 0; *input = 0;
                    clear_screen();
                    continue;
                }
                if (*input) {
                    show_result("client", "unknown command", 1);
                }
                pos = 0; *input = 0;
                clear_screen();
                continue;
            }
            if (ch == CTRL_P) {
                if (do_ping() != 0) show_result("client", "ping failed", 1);
                else show_result("client", "pong", 0);
                pos = 0; *input = 0;
                clear_screen();
                continue;
            }
            if (ch == CTRL_T) {
                if (client_fd != -1) {
                    term_restore();
                    fputs(ANSI_SHOW, stdout);
                    clear_screen();

                    pid_t pid = fork();
                    if (pid == 0) {
                        char fd_str[32];
                        snprintf(fd_str, sizeof(fd_str), "%d", client_fd);
                        execl("./plugins/hearts", "hearts", fd_str, client_name, NULL);
                        execl("plugins/hearts", "hearts", fd_str, client_name, NULL);
                        printf("\n  [ERR] Executable hearts not found in ./plugins/\n");
                        exit(1);
                    } else if (pid > 0) {
                        int status;
                        waitpid(pid, &status, 0);
                    }

                    term_raw_mode();
                    fputs(ANSI_HIDE, stdout);
                } else {
                    show_result("client", "not connected", 1);
                }
                pos = 0; *input = 0;
                clear_screen();
                continue;
            }
            if (ch == CTRL_L) {
                do_disconnect();
                break;
            }
            if (ch == CTRL_C || ch == CTRL_D) {
                do_disconnect();
                break;
            }
            if (ch == ESC) {
                if (input_available()) {
                    while (input_available()) read_char();
                    cursor_on = 1; frame = 0;
                    continue;
                } else {
                    do_disconnect();
                    break;
                }
            }
            if (ch == 127 || ch == 8) {
                if (pos > 0) input[--pos] = 0;
                cursor_on = 1; frame = 0;
                continue;
            }
            if (ch >= 32 && ch != 127 && pos < sizeof(input) - 1) {
                input[pos++] = (char)ch;
                input[pos] = 0;
                cursor_on = 1; frame = 0;
            }
        }
    }
    return 0;
}

static int prompt_start_page(char *buf, size_t bufsz) {
    size_t pos = 0;
    int ch, frame = 0, cursor_on = 1;

    *buf = 0;
    clear_screen();

    while (running) {
        frame++;
        if (frame >= CURSOR_BLINK) { frame = 0; cursor_on = !cursor_on; }

        get_term_size();
        fputs(ANSI_HIDE, stdout);
        render_header(" ACCSYS INTERACTIVE CLIENT LAUNCHER ");

        move_cursor(5, 5);
        fputs("Choose connection method:", stdout);
        move_cursor(7, 5);
        fputs("  [ 1 ]  list     - Scan available servers in local network (9000-9010)", stdout);
        move_cursor(9, 5);
        fputs("  [ 2 ]  direct   - Manually enter server IP and Port (e.g. ip:9000)", stdout);
        move_cursor(11, 5);
        fputs("  [ ESC / Q ] Exit Client Launcher", stdout);

        clear_line(14);
        move_cursor(15, 1);
        fputs(" > ", stdout);
        fputs(buf, stdout);
        if (cursor_on) {
            fputs(ANSI_REV, stdout); putchar(' '); fputs(ANSI_RST, stdout);
        } else {
            putchar(' ');
        }
        fputs(ANSI_CL, stdout);

        move_cursor(term_rows, 1);
        if (use_color) {
            fputs(ANSI_REV " 1 / l" ANSI_RREV "i" ANSI_RST ANSI_REV "st = scan | 2 / d" ANSI_RREV "i" ANSI_RST ANSI_REV "rect = manual | " ANSI_RREV "esc" ANSI_RST ANSI_REV " = quit ", stdout);
            int pad = term_cols - 52;
            while (pad-- > 0) putchar(' ');
            fputs(ANSI_RST, stdout);
        } else {
            print_inverted_full(" 1 / list = scan | 2 / direct = manual | esc = quit ");
        }
        fflush(stdout);

        while (input_available()) {
            ch = read_char();
            if (ch < 0) continue;

            if (ch == '\n' || ch == '\r') {
                if (strcmp(buf, "list") == 0 || strcmp(buf, "1") == 0) return 1;
                if (strcmp(buf, "direct") == 0 || strcmp(buf, "2") == 0) return 2;
                if (strcmp(buf, "quit") == 0 || strcmp(buf, "exit") == 0) return 0;
                if (strchr(buf, ':') || strchr(buf, '.')) return 2;
            }
            if (ch == CTRL_D) return 0;
            if (ch == CTRL_C) {
                while (input_available()) read_char();
                return 0;
            }
            if (ch == ESC) {
                if (input_available()) {
                    while (input_available()) read_char();
                    cursor_on = 1; frame = 0;
                    continue;
                } else {
                    return 0;
                }
            }

            if (ch == '1') { strcpy(buf, "list"); return 1; }
            if (ch == '2') { strcpy(buf, "direct"); return 2; }

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
    return 0;
}

/* ================ main flow ================ */

static int interactive_mode(void) {
    char name[INPUT_BUF_SZ];
    char key[INPUT_BUF_SZ];
    char launcher_buf[INPUT_BUF_SZ];
    int server_idx, r;

    use_color = detect_color_support();
    term_raw_mode();
    clear_screen();

    r = prompt_start_page(launcher_buf, sizeof(launcher_buf));
    if (r <= 0) goto done;

    if (r == 1 || strcmp(launcher_buf, "list") == 0) {
        scan_servers();
        server_idx = prompt_server_select();
        if (server_idx < 0) goto done;
        strncpy(client_host, servers[server_idx].host, sizeof(client_host) - 1);
        client_host[sizeof(client_host) - 1] = '\0';
        client_port = servers[server_idx].port;
    } else {
        if (strcmp(launcher_buf, "direct") == 0 || strcmp(launcher_buf, "2") == 0 || launcher_buf[0] == '\0') {
            r = prompt_line("direct", "server ip:port (default 127.0.0.1:9000)", launcher_buf, sizeof(launcher_buf), 0);
            if (r <= 0) goto done;
        }
        if (launcher_buf[0] == '\0') strcpy(launcher_buf, "127.0.0.1:9000");

        char host_part[256];
        int port_part = 9000;
        if (sscanf(launcher_buf, "%255[^:]:%d", host_part, &port_part) == 1) port_part = 9000;
        strncpy(client_host, host_part, sizeof(client_host));
        client_port = port_part;
    }

    r = prompt_line("client", "name", name, sizeof(name), 0);
    if (r <= 0) goto done;
    if (!is_valid_name(name)) {
        show_result("client", "invalid name", 1);
        goto done;
    }

    r = prompt_line("client", "key", key, sizeof(key), 0);
    if (r <= 0) goto done;
    if (strcmp(name, "admin") != 0 && !is_valid_key(key)) {
        show_result("client", "invalid key", 1);
        goto done;
    }

    if (connect_server(client_host, client_port) != 0) {
        show_result("client", "connect failed", 1);
        goto done;
    }

    {
        char login_msg[MAX_LINE];
        snprintf(login_msg, sizeof(login_msg), "login %s %s\n", name, key);
        if (send_server(login_msg) < 0) {
            do_disconnect();
            show_result("client", "connect failed", 1);
            goto done;
        }

        /* Wait and read server auth response */
        char resp[512];
        ssize_t r_len = read(client_fd, resp, sizeof(resp) - 1);
        if (r_len <= 0) {
            do_disconnect();
            show_result("client", "connect failed", 1);
            goto done;
        }
        resp[r_len] = '\0';
        if (strncmp(resp, "ERR:", 4) == 0) {
            do_disconnect();
            show_result("client", "auth failed", 1);
            goto done;
        }
    }

    strncpy(client_name, name, sizeof(client_name) - 1);
    client_name[sizeof(client_name) - 1] = '\0';

    show_result("client", "logged in", 0);
    status_shell();

done:
    do_disconnect();
    term_restore();
    fputs(ANSI_SHOW, stdout);
    clear_screen();
    return 0;
}

static void signal_handler(int sig) { (void)sig; running = 0; }

int main(int argc, char **argv) {
    int i;
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "-dumb") == 0) force_dumb = 1;

    setvbuf(stdout, NULL, _IONBF, 0);
    srand(time(NULL));
    return interactive_mode();
}
