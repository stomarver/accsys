/* ===== client_cli — login client UI ===== */
/* 1. Select server from scanned list (highlighting). */
/* 2. Enter name + key. */
/* 3. Authenticate locally against account storage. */
/* 4. Connect to server and show status shell. */
/* The server knows NOTHING about accounts; client_cli is the login service. */

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
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

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
#define GENERATOR_BIN  "./generator"
#define ACCOUNT_DIR    "account"
#define MAX_NAME       32
#define MAX_SERVERS    32

#define CTRL_D  4
#define CTRL_C  3
#define CTRL_L  12
#define CTRL_P  16
#define ESC     27
#define ARROW_UP    'A'
#define ARROW_DOWN  'B'

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

static void trim_nl(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
}

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

static int path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int is_directory(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

static int generator_available(void) {
    return path_exists(GENERATOR_BIN) && access(GENERATOR_BIN, X_OK) == 0;
}

static int join2(char *out, size_t outsz, const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    if (na + 1 + nb + 1 > outsz) return 0;
    memcpy(out, a, na);
    out[na] = '/';
    memcpy(out + na + 1, b, nb + 1);
    return 1;
}

static int read_file_line(const char *path, char *buf, size_t bufsz) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(buf, (int)bufsz, f)) { fclose(f); return 0; }
    fclose(f);
    trim_nl(buf);
    return 1;
}

/* ================ local auth (client is the login service) ================ */

static int resolve_uuid_dir(const char *identifier, char *uuid_dir_out) {
    DIR *d;
    struct dirent *ent;
    char check_dir[PATH_MAX];
    char name_file[PATH_MAX];
    char stored_name[MAX_LINE];
    int found = 0;

    if (!identifier) return 0;

    d = opendir(ACCOUNT_DIR);
    if (!d) return 0;

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (!join2(check_dir, sizeof(check_dir), ACCOUNT_DIR, ent->d_name)) continue;
        if (!is_directory(check_dir)) continue;
        if (!join2(name_file, sizeof(name_file), check_dir, "name")) continue;
        if (!read_file_line(name_file, stored_name, sizeof(stored_name))) continue;

        if (strcmp(stored_name, identifier) == 0 || strcmp(ent->d_name, identifier) == 0) {
            if (uuid_dir_out) {
                size_t len = strlen(check_dir);
                memcpy(uuid_dir_out, check_dir, len + 1);
            }
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

static int generator_verify_key(const char *stored, const char *key) {
    char *cmd;
    int ret;
    size_t need;

    need = strlen(GENERATOR_BIN) + 10 + strlen(stored) + strlen(key) + 10;
    cmd = malloc(need);
    if (!cmd) return 1;

    snprintf(cmd, need, "%s verify '%s' '%s'", GENERATOR_BIN, stored, key);
    ret = system(cmd);
    free(cmd);
    if (ret == -1) return 1;
    return WEXITSTATUS(ret);
}

static int auth_local(const char *name, const char *key) {
    char uuid_dir[PATH_MAX];
    char key_file[PATH_MAX];
    char stored_key[MAX_LINE];

    if (!is_valid_name(name) || !is_valid_key(key)) return 1;

    if (!generator_available()) {
        fprintf(stderr, "auth: generator not available\n");
        return 1;
    }

    if (!resolve_uuid_dir(name, uuid_dir)) return 1;
    if (!join2(key_file, sizeof(key_file), uuid_dir, "key")) return 1;
    if (!read_file_line(key_file, stored_key, sizeof(stored_key))) return 1;

    return generator_verify_key(stored_key, key) != 0;
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
    return strstr(buf, "pong") != NULL;
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
    strncpy(client_host, host, sizeof(client_host) - 1);
    client_host[sizeof(client_host) - 1] = '\0';
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
            if (ch >= 32 && ch < 127 && pos < bufsz - 1) {
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

static void render_status_shell(const char *input, int cursor_on) {
    char right_str[128];
    long up = (long)(time(NULL) - client_connected);
    int h = up / 3600, m = (up % 3600) / 60, s = up % 60;
    snprintf(right_str, sizeof(right_str), "%s  %02d:%02d:%02d ", client_name, h, m, s);

    get_term_size();
    fputs(ANSI_HIDE, stdout);

    move_cursor(1, 1);
    if (use_color) {
        int fixed = 22; /* " ctrl + ping / logout " */
        int right_len = strlen(right_str);
        int pad = term_cols - fixed - right_len;
        if (pad < 0) pad = 0;

        fputs(ANSI_REV, stdout);
        fputs(" ctrl + ", stdout);
        fputs(ANSI_RREV, stdout); putchar('p'); fputs(ANSI_RST ANSI_REV, stdout);
        fputs("ing / ", stdout);
        fputs(ANSI_RREV, stdout); putchar('l'); fputs(ANSI_RST ANSI_REV, stdout);
        fputs("ogout ", stdout);

        for (int i = 0; i < pad; i++) putchar(' ');
        fputs(right_str, stdout);
        fputs(ANSI_RST, stdout);
    } else {
        char full[MAX_LINE];
        snprintf(full, sizeof(full), " ctrl + ping / logout ");
        int right_len = strlen(right_str);
        int pad = term_cols - 22 - right_len;
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

static int status_shell(void) {
    char input[INPUT_BUF_SZ];
    size_t pos = 0;
    int ch, frame = 0, cursor_on = 1;
    fd_set rfds;
    struct timeval tv;
    char buf[256];
    ssize_t r;

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
                if (sscanf(line_ptr, "uptime %ld", &up) == 1 || sscanf(line_ptr, "pong %ld", &up) == 1) {
                    server_start_offset = time(NULL) - up;
                }
                if (strncmp(line_ptr, "msg ", 4) == 0) {
                    show_result("chat", line_ptr + 4, 0);
                    clear_screen();
                }
                line_ptr = strtok(NULL, "\n");
            }
        }

        if (r > 0 && FD_ISSET(STDIN_FILENO, &rfds)) {
            ch = read_char();
            if (ch < 0) continue;

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
            if (ch >= 32 && ch < 127 && pos < sizeof(input) - 1) {
                input[pos++] = (char)ch;
                input[pos] = 0;
                cursor_on = 1; frame = 0;
            }
        }
    }
    return 0;
}

/* ================ main flow ================ */

static int interactive_mode(void) {
    char name[INPUT_BUF_SZ];
    char key[INPUT_BUF_SZ];
    int server_idx, r;

    use_color = detect_color_support();
    term_raw_mode();
    clear_screen();

    scan_servers();
    server_idx = prompt_server_select();
    if (server_idx < 0) goto done;

    r = prompt_line("client", "name", name, sizeof(name), 0);
    if (r <= 0) goto done;
    if (!is_valid_name(name)) {
        show_result("client", "invalid name", 1);
        goto done;
    }

    r = prompt_line("client", "key", key, sizeof(key), 0);
    if (r <= 0) goto done;
    if (!is_valid_key(key)) {
        show_result("client", "invalid key", 1);
        goto done;
    }

    if (auth_local(name, key) != 0) {
        show_result("client", "auth failed", 1);
        goto done;
    }

    strncpy(client_name, name, sizeof(client_name) - 1);
    client_name[sizeof(client_name) - 1] = '\0';

    if (connect_server(servers[server_idx].host, servers[server_idx].port) != 0) {
        show_result("client", "connect failed", 1);
        goto done;
    }

    {
        char login_msg[MAX_LINE];
        snprintf(login_msg, sizeof(login_msg), "login %s\n", name);
        if (send_server(login_msg) < 0) {
            do_disconnect();
            show_result("client", "connect failed", 1);
            goto done;
        }
    }

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
    return interactive_mode();
}
