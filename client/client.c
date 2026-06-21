/* ===== accsys client — login service ===== */
/* Authenticates remotely against the server. */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

#define MAX_LINE        4096
#define MAX_NAME        64
#define DEFAULT_PORT    9000
#define SCAN_START      9000
#define SCAN_END        9010
#define SCAN_TIMEOUT_MS 200
#define HEARTBEAT_SEC   5

/* ---------- utils ---------- */

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
    if (strcmp(name, "admin") != 0 && !is_valid_key(key)) {
        fprintf(stderr, "invalid key\n");
        return 1;
    }

    fd = connect_server(host, port);
    if (fd < 0) return 1;

    /* Send remote login command */
    snprintf(login_msg, sizeof(login_msg), "login %s %s\n", name, key);
    if (send_all(fd, login_msg) < 0) {
        close(fd);
        return 1;
    }

    /* Wait and read server auth response */
    r = read(fd, buf, sizeof(buf) - 1);
    if (r <= 0) {
        fprintf(stderr, "connection closed by server during login\n");
        close(fd);
        return 1;
    }
    buf[r] = '\0';

    if (strncmp(buf, "ERR:", 4) == 0) {
        fprintf(stderr, "login failed: %s", buf + 4);
        close(fd);
        return 1;
    }

    printf("logged in as %s, connected to %s:%d\n", name, host, port);
    printf("server: %s", buf); /* Show server's initial response */
    fflush(stdout);

    while (1) {
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
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
            if (strcmp(buf, "ping\n") == 0 || strcmp(buf, "ping") == 0) {
                send_all(fd, "pong\n");
            } else {
                printf("server: %s", buf);
                fflush(stdout);
            }
        }
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            if (fgets(buf, sizeof(buf) - 1, stdin)) {
                if (buf[0] == 20 || strncmp(buf, "hearts", 6) == 0) {
                    pid_t pid = fork();
                    if (pid == 0) {
                        char fd_str[32];
                        snprintf(fd_str, sizeof(fd_str), "%d", fd);
                        execl("./plugins/hearts", "hearts", fd_str, name, NULL);
                        execl("plugins/hearts", "hearts", fd_str, name, NULL);
                        printf("\n  [ERR] Executable hearts not found in ./plugins/\n");
                        exit(1);
                    } else if (pid > 0) {
                        int status;
                        waitpid(pid, &status, 0);
                    }
                } else if (strncmp(buf, "msg ", 4) == 0) {
                    send_all(fd, buf);
                } else if (strncmp(buf, "quit", 4) == 0) {
                    send_all(fd, "quit\n");
                    break;
                }
            }
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
    return strstr(buf, "pong") != NULL || strstr(buf, "uptime") != NULL;
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
    fprintf(stderr, "  client hearts <host> <port> <name> <key>\n");
    fprintf(stderr, "  client list\n");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (strcmp(*(argv + 1), "login") == 0) {
        if (argc != 6) {
            print_usage();
            return 1;
        }
        return cmd_login(*(argv + 2), *(argv + 3), *(argv + 4), *(argv + 5));
    }

    if (strcmp(*(argv + 1), "hearts") == 0) {
        if (argc != 6) {
            print_usage();
            return 1;
        }
        int port = atoi(argv[3]);
        if (port <= 0 || port > 65535) port = DEFAULT_PORT;
        int fd = connect_server(argv[2], port);
        if (fd < 0) return 1;
        char login_msg[256];
        snprintf(login_msg, sizeof(login_msg), "login %s %s\n", argv[4], argv[5]);
        write(fd, login_msg, strlen(login_msg));
        char dump[512];
        ssize_t r = read(fd, dump, sizeof(dump) - 1);
        if (r <= 0 || strncmp(dump, "ERR:", 4) == 0) {
            fprintf(stderr, "auth failed during hearts launch\n");
            if (fd >= 0) close(fd);
            return 1;
        }
        pid_t pid = fork();
        if (pid == 0) {
            char fd_str[32];
            snprintf(fd_str, sizeof(fd_str), "%d", fd);
            execl("./plugins/hearts", "hearts", fd_str, argv[4], NULL);
            execl("plugins/hearts", "hearts", fd_str, argv[4], NULL);
            printf("\n  [ERR] Executable hearts not found in ./plugins/\n");
            exit(1);
        } else if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
        }
        close(fd);
        return 0;
    }

    if (strcmp(argv[1], "list") == 0) {
        return cmd_list();
    }

    print_usage();
    return 1;
}
