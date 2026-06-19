/* ===== accsys client — login service ===== */
/* Authenticates locally against the account storage, then connects to a
 * presence server. The server only tracks that this client is active,
 * its uptime and IP; it does NOT know the account system details. */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <limits.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

#define GENERATOR_BIN   "./generator"
#define ACCOUNT_DIR     "account"
#define MAX_LINE        4096
#define MAX_NAME        32
#define DEFAULT_PORT    9000
#define SCAN_START      9000
#define SCAN_END        9010
#define SCAN_TIMEOUT_MS 200
#define HEARTBEAT_SEC   5

/* ---------- utils ---------- */

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

/* ---------- account resolve ---------- */

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

/* ---------- auth ---------- */

static int generator_verify_key(const char *stored, const char *key) {
    char *cmd;
    size_t need;
    int ret;

    need = strlen(GENERATOR_BIN) + strlen(" verify ''") + strlen(stored) + strlen(key) + 1;
    cmd = malloc(need);
    if (!cmd) return 1;

    snprintf(cmd, need, "%s verify '%s' '%s'", GENERATOR_BIN, stored, key);
    ret = system(cmd);
    free(cmd);
    if (ret == -1) return 1;
    return WEXITSTATUS(ret);
}

static int auth_secure(const char *identifier, const char *key) {
    char uuid_dir[PATH_MAX];
    char key_file[PATH_MAX];
    char stored_key[MAX_LINE];

    if (!resolve_uuid_dir(identifier, uuid_dir)) return 1;
    if (!join2(key_file, sizeof(key_file), uuid_dir, "key")) return 1;
    if (!read_file_line(key_file, stored_key, sizeof(stored_key))) return 1;

    return generator_verify_key(stored_key, key) != 0;
}

static int auth(const char *identifier, const char *key) {
    if (!identifier || !key) return 1;
    if (!is_valid_name(identifier)) return 1;
    if (!is_valid_key(key)) return 1;

    if (generator_available()) {
        return auth_secure(identifier, key);
    }

    fprintf(stderr, "auth failed: generator not available\n");
    return 1;
}

/* ---------- network ---------- */

static int connect_server(const char *host, int port) {
    int fd;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "invalid host: %s\n", host);
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }

    return fd;
}

static int send_all(int fd, const char *msg) {
    size_t len = strlen(msg);
    ssize_t r;
    size_t sent = 0;
    while (sent < len) {
        r = write(fd, msg + sent, len - sent);
        if (r <= 0) return -1;
        sent += (size_t)r;
    }
    return 0;
}

/* ---------- commands ---------- */

static int cmd_login(const char *host, const char *port_str, const char *name, const char *key) {
    int port = atoi(port_str);
    int fd;
    char buf[1024];
    fd_set rfds;
    struct timeval tv;
    ssize_t r;
    char login_msg[MAX_LINE];

    if (port <= 0 || port > 65535) port = DEFAULT_PORT;

    if (!is_valid_name(name)) {
        fprintf(stderr, "invalid name\n");
        return 1;
    }
    if (!is_valid_key(key)) {
        fprintf(stderr, "invalid key\n");
        return 1;
    }

    if (auth(name, key) != 0) {
        fprintf(stderr, "login failed: bad name or key\n");
        return 1;
    }

    fd = connect_server(host, port);
    if (fd < 0) return 1;

    printf("logged in as %s, connected to %s:%d\n", name, host, port);

    snprintf(login_msg, sizeof(login_msg), "login %s\n", name);
    if (send_all(fd, login_msg) < 0) {
        close(fd);
        return 1;
    }

    while (1) {
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = HEARTBEAT_SEC;
        tv.tv_usec = 0;

        r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        if (r == 0) {
            if (send_all(fd, "ping\n") < 0) break;
            continue;
        }

        if (FD_ISSET(fd, &rfds)) {
            r = read(fd, buf, sizeof(buf) - 1);
            if (r <= 0) {
                if (r < 0) perror("read");
                printf("disconnected from server\n");
                break;
            }
            buf[r] = '\0';
            printf("server: %s", buf);
            fflush(stdout);
        }
    }

    close(fd);
    return 0;
}

static int try_server(const char *host, int port) {
    int fd;
    struct sockaddr_in addr;
    fd_set rfds;
    struct timeval tv;
    char buf[64];
    ssize_t r;

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

    if (send_all(fd, "ping\n") < 0) {
        close(fd);
        return 0;
    }

    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    tv.tv_sec = 0;
    tv.tv_usec = SCAN_TIMEOUT_MS * 1000;

    r = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (r <= 0) {
        close(fd);
        return 0;
    }

    r = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (r <= 0) return 0;
    buf[r] = '\0';
    return strstr(buf, "pong") != NULL;
}

static int cmd_list(void) {
    int port, found = 0;
    printf("scanning for servers on 127.0.0.1:%d-%d...\n", SCAN_START, SCAN_END);
    for (port = SCAN_START; port <= SCAN_END; port++) {
        if (try_server("127.0.0.1", port)) {
            printf("  127.0.0.1:%d\n", port);
            found = 1;
        }
    }
    if (!found) printf("  no servers found\n");
    return 0;
}

static void print_usage(void) {
    fprintf(stderr, "usage:\n");
    fprintf(stderr, "  client login <host> <port> <name> <key>\n");
    fprintf(stderr, "  client list\n");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (strcmp(argv[1], "login") == 0) {
        if (argc != 6) { print_usage(); return 1; }
        return cmd_login(argv[2], argv[3], argv[4], argv[5]);
    }

    if (strcmp(argv[1], "list") == 0) {
        return cmd_list();
    }

    print_usage();
    return 1;
}
