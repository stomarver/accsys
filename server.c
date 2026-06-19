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

#define SERVER_PORT     9000
#define MAX_CLIENTS     128
#define BUF_SIZE        4096

/* ---------- client state ---------- */

typedef struct {
    int fd;
    int active;
    time_t connected;
    struct sockaddr_in addr;
    char ip[INET_ADDRSTRLEN];
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
    server_start_time = time(NULL);
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) return i;
    }
    return -1;
}

static void remove_client(int idx) {
    int fd;
    if (idx < 0 || idx >= MAX_CLIENTS) return;
    if (!clients[idx].active) return;

    fd = clients[idx].fd;
    close(fd);

    printf("[fd=%d ip=%s] disconnected (uptime %lds)\n",
           fd, clients[idx].ip,
           (long)(time(NULL) - clients[idx].connected));
    fflush(stdout);

    clients[idx].active = 0;
    clients[idx].fd = -1;
}

static void print_client_list(void) {
    int i;
    time_t now = time(NULL);
    printf("\n--- active clients ---\n");
    server_start_time = time(NULL);
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].active) {
            printf("  fd=%d  ip=%s  uptime=%lds\n",
                   clients[i].fd, clients[i].ip,
                   (long)(now - clients[i].connected));
        }
    }
    printf("---\n");
    fflush(stdout);
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

    /* The server knows NOTHING about the data it receives.
     * The only two wire-level commands it understands are ping/pong and quit.
     * Everything else is silently ignored. */
    if (strcmp(buf, "ping\n") == 0 || strcmp(buf, "ping") == 0) {
        printf("[fd=%d ip=%s] ping\n", clients[idx].fd, clients[idx].ip);
        fflush(stdout);
        char out[64];
        snprintf(out, sizeof(out), "pong %ld\n", (long)(time(NULL) - server_start_time));
        if (write(clients[idx].fd, out, strlen(out)) < 0) remove_client(idx);

    } else if (strcmp(buf, "quit\n") == 0 || strcmp(buf, "quit") == 0) {
        if (write(clients[idx].fd, "OK: bye\n", 8) < 0) { /* ignore */ }
        remove_client(idx);
    }
}

/* ---------- main ---------- */

int main(void) {
    int srv_fd, i, max_fd, client_idx, new_fd, opt;
    struct sockaddr_in srv_addr, cli_addr;
    fd_set rfds;
    socklen_t addr_len;
    int ret;

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
        max_fd = srv_fd;

    for (i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active) {
                FD_SET(clients[i].fd, &rfds);
                if (clients[i].fd > max_fd) max_fd = clients[i].fd;
            }
        }

        ret = select(max_fd + 1, &rfds, NULL, NULL, NULL);
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
                memcpy(&clients[client_idx].addr, &cli_addr, sizeof(cli_addr));
                inet_ntop(AF_INET, &cli_addr.sin_addr,
                          clients[client_idx].ip,
                          sizeof(clients[client_idx].ip));
                printf("[fd=%d ip=%s] connected\n", new_fd, clients[client_idx].ip);
                char msg[64];
                snprintf(msg, sizeof(msg), "uptime %ld\n", (long)(time(NULL) - server_start_time));
                write(new_fd, msg, strlen(msg));
                fflush(stdout);
            }
        }

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
