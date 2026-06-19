/* ===== accsys server — minimal client tracker ===== */
/* The server knows NOTHING about the application logic.
 * It only tracks: is a client connected, for how long, and from which IP. */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <limits.h>
#include <ctype.h>

#define SERVER_PORT     9000
#define MAX_CLIENTS     128
#define BUF_SIZE        4096

#define ACCOUNT_DIR     "account"
#define GENERATOR_BIN   "./generator"
#define MAX_NAME        32
#define MAX_LINE        4096

/* ---------- client state ---------- */

typedef struct {
    int fd;
    int active;
    time_t connected;
    struct sockaddr_in addr;
    char ip[INET_ADDRSTRLEN];
    char name[33];             /* username (MAX_NAME=32 + 1) */
    int id;                    /* user ID: 0 for admin, 1-127 for others */
    long long last_ping_sent;  /* Timestamp in ms */
    int ping_ms;               /* latency in ms */
} client_t;

static volatile int running = 1;
static time_t server_start_time = 0;
static volatile sig_atomic_t print_list_flag = 0;
static client_t clients[MAX_CLIENTS];

/* ---------- utils ---------- */

static void stop_handler(int sig) {
    (void)sig;
    running = 0;
}

static void list_handler(int sig) {
    (void)sig;
    print_list_flag = 1;
}

static int find_free_client(void) {
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) return i;
    }
    return -1;
}

static int find_free_user_id(void) {
    int id;
    for (id = 1; id <= 127; id++) {
        int used = 0;
        int i;
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active && clients[i].id == id) {
                used = 1;
                break;
            }
        }
        if (!used) return id;
    }
    return -1;
}

static long long time_in_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void trim_nl(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

/* ---------- local authentication database engine ---------- */

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
    pid_t pid;
    int status;
    char *args[] = { GENERATOR_BIN, "verify", (char *)stored, (char *)key, NULL };
    if ((pid = fork()) == -1) return 1;
    if (pid == 0) {
        execv(args[0], args);
        exit(1);
    }
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int is_secure_non_uuid(const char *name) {
    char user_dir[PATH_MAX];
    char key_file[PATH_MAX];
    if (!join2(user_dir, sizeof(user_dir), ACCOUNT_DIR, name)) return 0;
    if (!is_directory(user_dir)) return 0;
    if (!join2(key_file, sizeof(key_file), user_dir, "key")) return 0;
    return path_exists(key_file);
}

static int auth_non_uuid_secure(const char *identifier, const char *key) {
    char user_dir[PATH_MAX];
    char key_file[PATH_MAX];
    char stored_key[MAX_LINE];

    if (!generator_available()) return 1;
    if (!join2(user_dir, sizeof(user_dir), ACCOUNT_DIR, identifier)) return 1;
    if (!is_directory(user_dir)) return 1;
    if (!join2(key_file, sizeof(key_file), user_dir, "key")) return 1;
    if (!read_file_line(key_file, stored_key, sizeof(stored_key))) return 1;

    return generator_verify_key(stored_key, key);
}

static int auth_plain(const char *identifier, const char *key) {
    char user_dir[PATH_MAX];
    char key_file[PATH_MAX];

    if (!join2(user_dir, sizeof(user_dir), ACCOUNT_DIR, identifier)) return 1;
    if (!is_directory(user_dir)) return 1;
    if (!join2(key_file, sizeof(key_file), user_dir, key)) return 1;
    if (path_exists(key_file)) return 0;
    return 1;
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

    if (auth_non_uuid_secure(identifier, key) == 0) return 0;

    if (!is_secure_non_uuid(identifier) && !is_valid_key(key)) return 1;

    if (auth_plain(identifier, key) == 0) return 0;

    if (generator_available()) {
        return auth_secure(identifier, key);
    }

    return 1;
}

/* ---------- connection cleanup ---------- */

static void remove_client(int idx) {
    int fd;
    if (idx < 0 || idx >= MAX_CLIENTS) return;
    if (!clients[idx].active) return;

    fd = clients[idx].fd;
    close(fd);

    if (clients[idx].name[0] != '\0') {
        printf("[%d/%s] disconnected (uptime %lds)\n",
               clients[idx].id, clients[idx].name,
               (long)(time(NULL) - clients[idx].connected));
    } else {
        printf("[-/%s] disconnected (uptime %lds)\n",
               clients[idx].ip,
               (long)(time(NULL) - clients[idx].connected));
    }
    fflush(stdout);

    clients[idx].active = 0;
    clients[idx].fd = -1;
}

static void print_client_list(void) {
    int i;
    int count = 0;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active) {
            long uptime = (long)(time(NULL) - clients[i].connected);
            char ping_str[32];
            if (clients[i].ping_ms >= 0) {
                snprintf(ping_str, sizeof(ping_str), "%dms", clients[i].ping_ms);
            } else {
                strcpy(ping_str, "unknown");
            }

            if (clients[i].name[0] != '\0') {
                printf("[%d/%s] | [%s / %lds]\n", clients[i].id, clients[i].name, ping_str, uptime);
            } else {
                printf("[-/%s] | [%s / %lds]\n", clients[i].ip, ping_str, uptime);
            }
            count++;
        }
    }
    if (count == 0) {
        printf("(no active clients)\n");
    }
    fflush(stdout);
}

/* ---------- server console commands ---------- */

static void handle_server_command(char *cmd) {
    if (strncmp(cmd, "info ", 5) == 0) {
        char *username = cmd + 5;
        while (*username == ' ') username++;
        int i, found = 0;
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active && strcmp(clients[i].name, username) == 0) {
                printf("User '%s' -> ID: %d, IP: %s\n", clients[i].name, clients[i].id, clients[i].ip);
                found = 1;
                break;
            }
        }
        if (!found) {
            printf("User '%s' not found\n", username);
        }
        fflush(stdout);
    } else if (strncmp(cmd, "check ", 6) == 0) {
        char *username = cmd + 6;
        while (*username == ' ') username++;
        int i, found = 0;
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active && strcmp(clients[i].name, username) == 0) {
                long uptime = (long)(time(NULL) - clients[i].connected);
                if (clients[i].ping_ms >= 0) {
                    printf("User '%s' -> Ping: %d ms, Uptime: %lds\n", clients[i].name, clients[i].ping_ms, uptime);
                } else {
                    printf("User '%s' -> Ping: unknown, Uptime: %lds\n", clients[i].name, uptime);
                }
                found = 1;
                break;
            }
        }
        if (!found) {
            printf("User '%s' not found\n", username);
        }
        fflush(stdout);
    } else if (strcmp(cmd, "list") == 0) {
        print_client_list();
    } else if (strcmp(cmd, "stop") == 0) {
        printf("Stopping server...\n");
        fflush(stdout);
        running = 0;
    } else if (strcmp(cmd, "hide") == 0) {
        printf("Hiding server and detaching from terminal...\n");
        fflush(stdout);
        if (daemon(1, 0) < 0) {
            perror("daemon");
        }
    } else if (strcmp(cmd, "help") == 0) {
        printf("Server commands:\n");
        printf("  info <username>  - Check IP and ID of a user\n");
        printf("  check <username> - Check ping and uptime of a user\n");
        printf("  list             - List all active clients in [id/name] | [ping/uptime] format\n");
        printf("  stop             - Stop and shut down the server\n");
        printf("  hide             - Hide console and detach server to background\n");
        fflush(stdout);
    } else if (*cmd != '\0') {
        printf("Unknown command: %s. Type 'help' for available commands.\n", cmd);
        fflush(stdout);
    }
}

/* ---------- I/O ---------- */

static void handle_client_data(int idx) {
    char buf[BUF_SIZE];
    ssize_t r;

    r = read(clients[idx].fd, buf, sizeof(buf) - 1);
    if (r <= 0) {
        if (r < 0 && errno != ECONNRESET) perror("read");
        remove_client(idx);
        return;
    }
    buf[r] = '\0';

    /* Parse line by line to handle concatenated inputs/packets properly */
    char *saveptr;
    char *line = strtok_r(buf, "\n", &saveptr);
    while (line != NULL) {
        /* Trim trailing \r */
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        if (strncmp(line, "login ", 6) == 0) {
            char *args_str = line + 6;
            while (*args_str == ' ') args_str++;

            char name[33] = {0};
            char password[256] = {0};

            if (sscanf(args_str, "%32s %255s", name, password) == 2) {
                if (auth(name, password) == 0) {
                    memcpy(clients[idx].name, name, sizeof(clients[idx].name) - 1);
                    clients[idx].name[sizeof(clients[idx].name) - 1] = '\0';

                    if (strcmp(clients[idx].name, "admin") == 0) {
                        clients[idx].id = 0;
                    } else {
                        clients[idx].id = find_free_user_id();
                    }

                    printf("[%d/%s] logged in from %s\n", clients[idx].id, clients[idx].name, clients[idx].ip);
                    fflush(stdout);

                    /* Reply with success */
                    char out[128];
                    snprintf(out, sizeof(out), "OK: logged in\nuptime %ld\n", (long)(time(NULL) - server_start_time));
                    write(clients[idx].fd, out, strlen(out));
                } else {
                    printf("[-/%s] login failed for user '%s'\n", clients[idx].ip, name);
                    fflush(stdout);
                    write(clients[idx].fd, "ERR: auth failed\n", 17);
                    remove_client(idx);
                    return;
                }
            } else {
                write(clients[idx].fd, "ERR: invalid login args\n", 24);
                remove_client(idx);
                return;
            }

        } else if (strcmp(line, "ping") == 0) {
            if (clients[idx].name[0] != '\0') {
                printf("[%d/%s] ping\n", clients[idx].id, clients[idx].name);
            } else {
                printf("[-/%s] ping\n", clients[idx].ip);
            }
            fflush(stdout);
            char out[64];
            snprintf(out, sizeof(out), "pong %ld\n", (long)(time(NULL) - server_start_time));
            if (write(clients[idx].fd, out, strlen(out)) < 0) {
                remove_client(idx);
                return;
            }

        } else if (strcmp(line, "pong") == 0) {
            if (clients[idx].last_ping_sent > 0) {
                clients[idx].ping_ms = (int)(time_in_ms() - clients[idx].last_ping_sent);
                clients[idx].last_ping_sent = 0;
            }

        } else if (strcmp(line, "quit") == 0) {
            if (write(clients[idx].fd, "OK: bye\n", 8) < 0) { /* ignore */ }
            remove_client(idx);
            return;

        } else if (strncmp(line, "msg ", 4) == 0) {
            char *msg_text = line + 4;
            printf("[%s/%s] %s\n", clients[idx].ip, clients[idx].name[0] ? clients[idx].name : "-", msg_text);
            fflush(stdout);
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }
}

/* ---------- main ---------- */

int main(int argc, char **argv) {
    int srv_fd, i, max_fd, client_idx, new_fd, opt;
    struct sockaddr_in srv_addr, cli_addr;
    fd_set rfds;
    socklen_t addr_len;
    int ret;
    int start_hidden = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-hide") == 0) {
            start_hidden = 1;
        }
    }

    if (start_hidden) {
        if (daemon(1, 0) < 0) {
            perror("daemon");
            return 1;
        }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = stop_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = list_handler;
    sigaction(SIGUSR1, &sa, NULL); /* print client list */

    server_start_time = time(NULL);
    for (i = 0; i < MAX_CLIENTS; i++) {
        clients[i].active = 0;
        clients[i].fd = -1;
        memset(clients[i].name, 0, sizeof(clients[i].name));
        clients[i].id = -1;
        clients[i].last_ping_sent = 0;
        clients[i].ping_ms = -1;
    }

    srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv_fd < 0) { perror("socket"); return 1; }

    opt = 1;
    setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_addr.s_addr = INADDR_ANY;
    srv_addr.sin_port = htons(SERVER_PORT);

    if (bind(srv_fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) < 0) {
        perror("bind");
        close(srv_fd);
        return 1;
    }

    if (listen(srv_fd, 16) < 0) {
        perror("listen");
        close(srv_fd);
        return 1;
    }

    printf("accsys server listening on 0.0.0.0:%d\n", SERVER_PORT);
    fflush(stdout);

    while (running) {
        FD_ZERO(&rfds);
        FD_SET(srv_fd, &rfds);
        FD_SET(STDIN_FILENO, &rfds);
        max_fd = srv_fd > STDIN_FILENO ? srv_fd : STDIN_FILENO;

        for (i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active) {
                FD_SET(clients[i].fd, &rfds);
                if (clients[i].fd > max_fd) max_fd = clients[i].fd;
            }
        }

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        ret = select(max_fd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) {
                if (print_list_flag) {
                    print_list_flag = 0;
                    print_client_list();
                }
                if (!running) break;
                continue;
            }
            perror("select");
            break;
        }

        if (print_list_flag) {
            print_list_flag = 0;
            print_client_list();
        }

        /* Periodic server-initiated ping every 5 seconds */
        static time_t last_ping_time = 0;
        time_t now = time(NULL);
        if (now - last_ping_time >= 5) {
            last_ping_time = now;
            long long now_ms = time_in_ms();
            for (i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].active && clients[i].name[0] != '\0') {
                    clients[i].last_ping_sent = now_ms;
                    /* Send raw ping line */
                    if (write(clients[i].fd, "ping\n", 5) < 0) { /* ignore */ }
                }
            }
        }

        /* Handle stdin console commands */
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            char cmd_buf[512];
            if (fgets(cmd_buf, sizeof(cmd_buf), stdin)) {
                trim_nl(cmd_buf);
                handle_server_command(cmd_buf);
            }
        }

        /* Handle new incoming connection */
        if (FD_ISSET(srv_fd, &rfds)) {
            addr_len = sizeof(cli_addr);
            new_fd = accept(srv_fd, (struct sockaddr *)&cli_addr, &addr_len);
            if (new_fd < 0) {
                perror("accept");
                continue;
            }
            client_idx = find_free_client();
            if (client_idx < 0) {
                printf("no free client slots for fd=%d\n", new_fd);
                close(new_fd);
            } else {
                clients[client_idx].fd = new_fd;
                clients[client_idx].active = 1;
                clients[client_idx].connected = time(NULL);
                memset(clients[client_idx].name, 0, sizeof(clients[client_idx].name));
                clients[client_idx].id = -1;
                clients[client_idx].last_ping_sent = 0;
                clients[client_idx].ping_ms = -1;
                memcpy(&clients[client_idx].addr, &cli_addr, sizeof(cli_addr));
                inet_ntop(AF_INET, &cli_addr.sin_addr,
                          clients[client_idx].ip,
                          sizeof(clients[client_idx].ip));
                printf("[-/%s] connected\n", clients[client_idx].ip);
                fflush(stdout);
            }
        }

        /* Handle client data */
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active && FD_ISSET(clients[i].fd, &rfds)) {
                handle_client_data(i);
            }
        }
    }

    printf("shutting down...\n");
    for (i = 0; i < MAX_CLIENTS; i++) remove_client(i);
    close(srv_fd);
    return 0;
}
