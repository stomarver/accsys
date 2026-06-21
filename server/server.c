/* ===== accsys server — minimal client tracker ===== */
/* The server knows NOTHING about the application logic.
 * It only tracks: is a client connected, for how long, and from which IP. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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
#include "../shared/plugins/plugin_engine.c"

#define SERVER_PORT     9000
#define MAX_CLIENTS     128
#define BUF_SIZE        4096

static char ACCOUNT_DIR[PATH_MAX] = "account";
#define GENERATOR_BIN   "./generator"
#define MAX_NAME        64
#define MAX_LINE        4096

/* ---------- client state ---------- */

typedef struct {
    int fd;
    int active;
    time_t connected;
    struct sockaddr_in addr;
    char ip[INET_ADDRSTRLEN];
    char name[65];             /* username (MAX_NAME=64 + 1) */
    char uuid[65];             /* UUID if applicable */
    int id;                    /* user ID: 0 for admin, 1-127 for others */
    long long last_ping_sent;  /* Timestamp in ms */
    int ping_ms;               /* latency in ms */
} client_t;

static volatile int running = 1;
static plugin_server_ctx_t plugin_sctx;
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
    if (path_exists("./shared/generator") && access("./shared/generator", X_OK) == 0) return 1;
    if (path_exists("../shared/generator") && access("../shared/generator", X_OK) == 0) return 1;
    if (path_exists("./generator") && access("./generator", X_OK) == 0) return 1;
    return 0;
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

static int generator_verify_key(const char *stored, const char *key) {
    pid_t pid;
    int status;
    const char *gbin = "./shared/generator";
    if (!path_exists(gbin)) gbin = "../shared/generator";
    if (!path_exists(gbin)) gbin = "./generator";
    char *args[] = { (char *)gbin, "verify", (char *)stored, (char *)key, NULL };
    if ((pid = fork()) == -1) return 1;
    if (pid == 0) {
        execv(args[0], args);
        exit(1);
    }
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static int is_uuid_format(const char *s) {
    size_t len;
    if (!s) return 0;
    len = strlen(s);

    if (len == 19) {
        if (s[4] != '-' || s[9] != '-' || s[14] != '-') return 0;
        for (int i = 0; i < 19; i++) {
            if (i == 4 || i == 9 || i == 14) continue;
            if (!isxdigit((unsigned char)s[i])) return 0;
        }
        return 1;
    }

    if (len == 36) {
        if (s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-') return 0;
        for (int i = 0; i < 36; i++) {
            if (i == 8 || i == 13 || i == 18 || i == 23) continue;
            if (!isxdigit((unsigned char)s[i])) return 0;
        }
        return 1;
    }

    return 0;
}

static void copy_str(char *dest, const char *src, size_t n) {
    size_t i = 0;
    if (!dest || !src || n == 0) return;
    while (src[i] && i < n - 1) {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';
}

static int verify_and_resolve_account(const char *identifier, const char *password, char *name_out, char *uuid_out) {
    DIR *d;
    struct dirent *ent;
    char check_dir[PATH_MAX];
    char name_file[PATH_MAX];
    char key_file[PATH_MAX];
    char stored_name[MAX_LINE];
    char stored_key[MAX_LINE];
    int found = 0;

    if (!identifier || !password) return 1;
    if (!is_valid_name(identifier)) return 1;

    /* Case 1: identifier is in UUID format */
    if (is_uuid_format(identifier)) {
        if (!join2(check_dir, sizeof(check_dir), ACCOUNT_DIR, identifier)) return 1;
        if (!is_directory(check_dir)) return 1;

        /* If folder is a UUID, it MUST have a name file */
        if (!join2(name_file, sizeof(name_file), check_dir, "name")) return 1;
        if (!read_file_line(name_file, stored_name, sizeof(stored_name))) return 1;

        if (name_out) copy_str(name_out, stored_name, 65);
        if (uuid_out) copy_str(uuid_out, identifier, 65);

        /* Verify key */
        if (generator_available() &&
            join2(key_file, sizeof(key_file), check_dir, "key") &&
            read_file_line(key_file, stored_key, sizeof(stored_key))) {
            return generator_verify_key(stored_key, password) == 0 ? 0 : 1;
        }

        /* If key file does not exist, but password is not empty, it is valid */
        return 0;
    }

    /* Case 2: identifier is NOT a UUID. Let's look for a UUID folder whose name matches identifier */
    d = opendir(ACCOUNT_DIR);
    if (d) {
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            if (!is_uuid_format(ent->d_name)) continue;
            if (!join2(check_dir, sizeof(check_dir), ACCOUNT_DIR, ent->d_name)) continue;
            if (!is_directory(check_dir)) continue;

            if (!join2(name_file, sizeof(name_file), check_dir, "name")) continue;
            if (!read_file_line(name_file, stored_name, sizeof(stored_name))) continue;

            if (strcmp(stored_name, identifier) == 0) {
                if (name_out) copy_str(name_out, stored_name, 65);
                if (uuid_out) copy_str(uuid_out, ent->d_name, 65);
                found = 1;
                break;
            }
        }
        closedir(d);

        if (found) {
            /* Verify key in the found directory */
            if (generator_available() &&
                join2(key_file, sizeof(key_file), check_dir, "key") &&
                read_file_line(key_file, stored_key, sizeof(stored_key))) {
                return generator_verify_key(stored_key, password) == 0 ? 0 : 1;
            }
            /* If key file does not exist, but password is not empty, it is valid */
            return 0;
        }
    }

    /* Case 3: identifier is a non-UUID directory (unsecure or admin) */
    if (!join2(check_dir, sizeof(check_dir), ACCOUNT_DIR, identifier)) return 1;
    if (!is_directory(check_dir)) return 1;

    if (name_out) copy_str(name_out, identifier, 65);
    if (uuid_out) uuid_out[0] = '\0';

    /* Combinatorial approach: only for admin (folder = admin, file = key with encrypted password) */
    if (strcmp(identifier, "admin") == 0) {
        if (generator_available() &&
            join2(key_file, sizeof(key_file), check_dir, "key") &&
            read_file_line(key_file, stored_key, sizeof(stored_key))) {
            return generator_verify_key(stored_key, password) == 0 ? 0 : 1;
        }
    }

    /* Check plain mode: check if file account/<identifier>/<password> exists */
    if (is_valid_key(password) && join2(key_file, sizeof(key_file), check_dir, password) && path_exists(key_file)) {
        return 0;
    }

    return 1;
}

/* ---------- Config & Ban Engine ---------- */

typedef struct {
    char type[32];
    char val[128];
    int fullcase;
} blacklist_item_t;

static blacklist_item_t server_blacklist[128];
static int server_blacklist_count = 0;

typedef struct {
    char ip[INET_ADDRSTRLEN];
    time_t expires;
} banned_ip_t;

typedef struct {
    char identifier[65];
    time_t expires;
} banned_user_t;

static banned_ip_t active_banned_ips[128];
static int banned_ips_count = 0;

static banned_user_t active_banned_users[128];
static int banned_users_count = 0;

typedef struct {
    char ip[INET_ADDRSTRLEN];
    int failed_attempts;
    time_t last_attempt;
} ip_auth_tracker_t;

static ip_auth_tracker_t auth_trackers[128];
static int auth_trackers_count = 0;

static int server_max_failed_attempts = 5;
static int server_autoban_duration_seconds = 600;

static void load_server_config(void) {
    int in_sub = path_exists("../shared") || path_exists("../server") || path_exists("../client") || path_exists("../Makefile");
    char cfg_path[PATH_MAX];
    if (in_sub) strcpy(cfg_path, "../config/server.cfg");
    else strcpy(cfg_path, "config/server.cfg");

    FILE *f = fopen(cfg_path, "r");
    if (!f && !in_sub) f = fopen("server.cfg", "r");
    if (!f && in_sub) f = fopen("../server.cfg", "r");
    if (!f) {
        if (in_sub) mkdir("../config", 0700);
        else mkdir("config", 0700);
        f = fopen(cfg_path, "w");
        if (f) {
            fprintf(f, "max_failed_attempts=5\nautoban_duration_seconds=600\n");
            fclose(f);
        }
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int val;
        if (sscanf(line, "max_failed_attempts=%d", &val) == 1) server_max_failed_attempts = val;
        else if (sscanf(line, "autoban_duration_seconds=%d", &val) == 1) server_autoban_duration_seconds = val;
    }
    fclose(f);
}

static void load_server_blacklist(void) {
    int in_sub = path_exists("../shared") || path_exists("../server") || path_exists("../client") || path_exists("../Makefile");
    char bl_path[PATH_MAX];
    if (in_sub) strcpy(bl_path, "../config/blacklist.txt");
    else strcpy(bl_path, "config/blacklist.txt");

    server_blacklist_count = 0;
    FILE *f = fopen(bl_path, "r");
    if (!f && !in_sub) f = fopen("blacklist.txt", "r");
    if (!f && in_sub) f = fopen("../blacklist.txt", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char type[32] = {0}, val[128] = {0}, param[32] = {0};
        if (sscanf(line, "%31s %127s %31s", type, val, param) >= 2) {
            if (strcmp(type, "uuid") == 0 || strcmp(type, "name") == 0) {
                copy_str(server_blacklist[server_blacklist_count].type, type, 32);
                copy_str(server_blacklist[server_blacklist_count].val, val, 128);
                server_blacklist[server_blacklist_count].fullcase = (strcmp(param, "fullcase") == 0);
                server_blacklist_count++;
                if (server_blacklist_count >= 128) break;
            }
        }
    }
    fclose(f);
}

static int is_account_blacklisted(const char *identifier, const char *resolved_name, const char *uuid) {
    for (int i = 0; i < server_blacklist_count; i++) {
        blacklist_item_t *item = &server_blacklist[i];
        if (strcmp(item->type, "uuid") == 0) {
            if (uuid && uuid[0] && strcmp(item->val, uuid) == 0) return 1;
            if (identifier && strcmp(item->val, identifier) == 0) return 1;
        } else if (strcmp(item->type, "name") == 0) {
            if (resolved_name && resolved_name[0]) {
                if (item->fullcase) {
                    if (strcasestr(resolved_name, item->val) != NULL) return 1;
                } else {
                    if (strstr(resolved_name, item->val) != NULL) return 1;
                }
            }
            if (identifier && identifier[0]) {
                if (item->fullcase) {
                    if (strcasestr(identifier, item->val) != NULL) return 1;
                } else {
                    if (strstr(identifier, item->val) != NULL) return 1;
                }
            }
        }
    }
    return 0;
}

static time_t parse_time_duration(const char *s) {
    if (!s || !*s) return 0;
    char *endptr;
    long val = strtol(s, &endptr, 10);
    if (val <= 0) return 0;
    while (*endptr == ' ') endptr++;
    if (*endptr == 's' || *endptr == 'S') return (time_t)val;
    if (*endptr == 'm') return (time_t)(val * 60);
    if (*endptr == 'h' || *endptr == 'H') return (time_t)(val * 3600);
    if (*endptr == 'd' || *endptr == 'D') return (time_t)(val * 86400);
    if (*endptr == 'M') return (time_t)(val * 2592000); /* Month */
    if (*endptr == 'y' || *endptr == 'Y') return (time_t)(val * 31536000); /* Year */
    return (time_t)val;
}

static void execute_ban_user(const char *identifier, time_t duration) {
    time_t expires = duration > 0 ? time(NULL) + duration : 0;
    
    if (banned_users_count < 128) {
        copy_str(active_banned_users[banned_users_count].identifier, identifier, 65);
        active_banned_users[banned_users_count].expires = expires;
        banned_users_count++;
    }

    DIR *d = opendir(ACCOUNT_DIR);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char check_dir[PATH_MAX], name_file[PATH_MAX], stored_name[MAX_LINE];
            if (!join2(check_dir, sizeof(check_dir), ACCOUNT_DIR, ent->d_name)) continue;
            if (!is_directory(check_dir)) continue;

            int is_target = 0;
            if (strcmp(ent->d_name, identifier) == 0) is_target = 1;
            else if (join2(name_file, sizeof(name_file), check_dir, "name") && read_file_line(name_file, stored_name, sizeof(stored_name))) {
                if (strcmp(stored_name, identifier) == 0) is_target = 1;
            }

            if (is_target) {
                char ban_file[PATH_MAX];
                if (join2(ban_file, sizeof(ban_file), check_dir, "ban")) {
                    FILE *bf = fopen(ban_file, "w");
                    if (bf) {
                        fprintf(bf, "%lld\n", (long long)expires);
                        fclose(bf);
                    }
                }
                break;
            }
        }
        closedir(d);
    }
}

static void execute_ban_ip(const char *ip, time_t duration) {
    time_t expires = duration > 0 ? time(NULL) + duration : 0;
    
    if (banned_ips_count < 128) {
        copy_str(active_banned_ips[banned_ips_count].ip, ip, INET_ADDRSTRLEN);
        active_banned_ips[banned_ips_count].expires = expires;
        banned_ips_count++;
    }

    char bans_dir[PATH_MAX];
    snprintf(bans_dir, sizeof(bans_dir), "%s/bans_ip", ACCOUNT_DIR);
    mkdir(bans_dir, 0700);

    char ip_ban_file[PATH_MAX + 256];
    snprintf(ip_ban_file, sizeof(ip_ban_file), "%s/%s", bans_dir, ip);
    FILE *bf = fopen(ip_ban_file, "w");
    if (bf) {
        fprintf(bf, "%lld\n", (long long)expires);
        fclose(bf);
    }
}

static void load_persistent_bans(void) {
    time_t now = time(NULL);
    banned_users_count = 0;
    banned_ips_count = 0;

    DIR *d = opendir(ACCOUNT_DIR);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char check_dir[PATH_MAX], ban_file[PATH_MAX];
            if (!join2(check_dir, sizeof(check_dir), ACCOUNT_DIR, ent->d_name)) continue;
            if (!is_directory(check_dir)) continue;

            if (join2(ban_file, sizeof(ban_file), check_dir, "ban") && path_exists(ban_file)) {
                char exp_str[64];
                if (read_file_line(ban_file, exp_str, sizeof(exp_str))) {
                    long long exp_val = atoll(exp_str);
                    if (exp_val == 0 || now < (time_t)exp_val) {
                        if (banned_users_count < 128) {
                            copy_str(active_banned_users[banned_users_count].identifier, ent->d_name, 65);
                            active_banned_users[banned_users_count].expires = (time_t)exp_val;
                            banned_users_count++;

                            char name_file[PATH_MAX], stored_name[MAX_LINE];
                            if (join2(name_file, sizeof(name_file), check_dir, "name") && read_file_line(name_file, stored_name, sizeof(stored_name))) {
                                if (banned_users_count < 128) {
                                    copy_str(active_banned_users[banned_users_count].identifier, stored_name, 65);
                                    active_banned_users[banned_users_count].expires = (time_t)exp_val;
                                    banned_users_count++;
                                }
                            }
                        }
                    } else {
                        unlink(ban_file);
                    }
                }
            }
        }
        closedir(d);
    }

    char bans_dir[PATH_MAX];
    snprintf(bans_dir, sizeof(bans_dir), "%s/bans_ip", ACCOUNT_DIR);
    d = opendir(bans_dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char ip_ban_file[PATH_MAX + 256], exp_str[64];
            snprintf(ip_ban_file, sizeof(ip_ban_file), "%s/%s", bans_dir, ent->d_name);
            if (read_file_line(ip_ban_file, exp_str, sizeof(exp_str))) {
                long long exp_val = atoll(exp_str);
                if (exp_val == 0 || now < (time_t)exp_val) {
                    if (banned_ips_count < 128) {
                        copy_str(active_banned_ips[banned_ips_count].ip, ent->d_name, INET_ADDRSTRLEN);
                        active_banned_ips[banned_ips_count].expires = (time_t)exp_val;
                        banned_ips_count++;
                    }
                } else {
                    unlink(ip_ban_file);
                }
            }
        }
        closedir(d);
    }
}

static int is_ip_banned(const char *ip) {
    time_t now = time(NULL);
    for (int i = 0; i < banned_ips_count; i++) {
        if (strcmp(active_banned_ips[i].ip, ip) == 0) {
            if (active_banned_ips[i].expires == 0 || now < active_banned_ips[i].expires) {
                return 1;
            }
        }
    }
    return 0;
}

static int is_user_banned(const char *identifier, const char *uuid) {
    time_t now = time(NULL);
    for (int i = 0; i < banned_users_count; i++) {
        if (identifier && strcmp(active_banned_users[i].identifier, identifier) == 0) {
            if (active_banned_users[i].expires == 0 || now < active_banned_users[i].expires) return 1;
        }
        if (uuid && uuid[0] && strcmp(active_banned_users[i].identifier, uuid) == 0) {
            if (active_banned_users[i].expires == 0 || now < active_banned_users[i].expires) return 1;
        }
    }
    return 0;
}

static void track_failed_auth(const char *ip) {
    for (int i = 0; i < auth_trackers_count; i++) {
        if (strcmp(auth_trackers[i].ip, ip) == 0) {
            auth_trackers[i].failed_attempts++;
            auth_trackers[i].last_attempt = time(NULL);
            if (auth_trackers[i].failed_attempts >= server_max_failed_attempts) {
                strncpy(active_banned_ips[banned_ips_count].ip, ip, INET_ADDRSTRLEN - 1);
                active_banned_ips[banned_ips_count].expires = time(NULL) + server_autoban_duration_seconds;
                banned_ips_count++;
                printf("[autoban]: ip '%s' banned for %ds after %d failed attempts\n", ip, server_autoban_duration_seconds, auth_trackers[i].failed_attempts);
                fflush(stdout);
            }
            return;
        }
    }
    if (auth_trackers_count < 128) {
        strncpy(auth_trackers[auth_trackers_count].ip, ip, INET_ADDRSTRLEN - 1);
        auth_trackers[auth_trackers_count].failed_attempts = 1;
        auth_trackers[auth_trackers_count].last_attempt = time(NULL);
        auth_trackers_count++;
    }
}

static void track_successful_auth(const char *ip) {
    for (int i = 0; i < auth_trackers_count; i++) {
        if (strcmp(auth_trackers[i].ip, ip) == 0) {
            auth_trackers[i].failed_attempts = 0;
            return;
        }
    }
}

static int encrypt_key_helper(const char *key, char *out, size_t outsz) {
    char cmd[512];
    const char *gbin = "./shared/generator";
    if (!path_exists(gbin)) gbin = "../shared/generator";
    if (!path_exists(gbin)) gbin = "./generator";
    snprintf(cmd, sizeof(cmd), "%s encrypt %s", gbin, key);
    FILE *p = popen(cmd, "r");
    if (!p) return 0;
    if (!fgets(out, (int)outsz, p)) { pclose(p); return 0; }
    pclose(p);
    trim_nl(out);
    return 1;
}

static int write_file_line(const char *path, const char *line) {
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    if (fprintf(f, "%s\n", line) < 0) { fclose(f); return 0; }
    fclose(f);
    return 1;
}

static int execute_server_registration(const char *name, const char *key) {
    if (!name || !key) return 1;
    if (strcmp(name, "admin") == 0) return 1;
    if (!is_valid_name(name)) return 1;
    if (is_account_blacklisted(name, name, NULL)) { printf("reg fail blacklist\n"); return 1; }

    mkdir(ACCOUNT_DIR, 0700);
    char new_dir[PATH_MAX];
    if (generator_available()) {
        char uuid[32];
        int attempts = 0;
        do {
            snprintf(uuid, sizeof(uuid), "%04x-%04x-%04x-%04x",
                     rand() & 0xffff, rand() & 0xffff,
                     rand() & 0xffff, rand() & 0xffff);
            join2(new_dir, sizeof(new_dir), ACCOUNT_DIR, uuid);
            attempts++;
        } while (path_exists(new_dir) && attempts < 100);

        if (mkdir(new_dir, 0700) != 0) { perror("mkdir reg"); return 1; }

        char name_file[PATH_MAX], key_file[PATH_MAX];
        if (!join2(name_file, sizeof(name_file), new_dir, "name")) { printf("reg fail name join\n"); return 1; }
        if (!write_file_line(name_file, name)) { printf("reg fail name write\n"); return 1; }

        char encrypted[256];
        if (!encrypt_key_helper(key, encrypted, sizeof(encrypted))) { printf("reg fail encrypt\n"); return 1; }
        if (!join2(key_file, sizeof(key_file), new_dir, "key")) { printf("reg fail key join\n"); return 1; }
        if (!write_file_line(key_file, encrypted)) { printf("reg fail key write\n"); return 1; }
        return 0;
    }

    if (!join2(new_dir, sizeof(new_dir), ACCOUNT_DIR, name)) return 1;
    if (mkdir(new_dir, 0700) != 0) return 1;

    char pass_file[PATH_MAX];
    if (!join2(pass_file, sizeof(pass_file), new_dir, key)) return 1;
    FILE *f = fopen(pass_file, "w");
    if (!f) return 1;
    fclose(f);
    return 0;
}

/* ---------- connection cleanup ---------- */

static void remove_client(int idx) {
    int fd;
    if (idx < 0 || idx >= MAX_CLIENTS) return;
    if (!clients[idx].active) return;

    fd = clients[idx].fd;
    close(fd);

    if (clients[idx].name[0] != '\0') {
        printf("disconnect [%s / %lds]\n",
               clients[idx].name,
               (long)(time(NULL) - clients[idx].connected));
        fflush(stdout);
    }

    clients[idx].active = 0;
    clients[idx].fd = -1;
    memset(clients[idx].name, 0, sizeof(clients[idx].name));
    memset(clients[idx].uuid, 0, sizeof(clients[idx].uuid));
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
            if (clients[i].active &&
                (strcmp(clients[i].name, username) == 0 ||
                 (clients[i].uuid[0] != '\0' && strcmp(clients[i].uuid, username) == 0))) {
                printf("[%d / %s / %s]\n", clients[i].id, clients[i].ip,
                       clients[i].uuid[0] != '\0' ? clients[i].uuid : "-");
                found = 1;
                break;
            }
        }
        if (!found) {
            printf("[not found]\n");
        }
        fflush(stdout);
    } else if (strncmp(cmd, "check ", 6) == 0) {
        char *username = cmd + 6;
        while (*username == ' ') username++;
        int i, found = 0;
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active &&
                (strcmp(clients[i].name, username) == 0 ||
                 (clients[i].uuid[0] != '\0' && strcmp(clients[i].uuid, username) == 0))) {
                long uptime = (long)(time(NULL) - clients[i].connected);
                char ping_str[32];
                if (clients[i].ping_ms >= 0) {
                    snprintf(ping_str, sizeof(ping_str), "%dms", clients[i].ping_ms);
                } else {
                    strcpy(ping_str, "unknown");
                }
                printf("[%s / %lds]\n", ping_str, uptime);
                found = 1;
                break;
            }
        }
        if (!found) {
            printf("[not found]\n");
        }
        fflush(stdout);
    } else if (strcmp(cmd, "list") == 0) {
        print_client_list();
    } else if (strncmp(cmd, "ban-ip ", 7) == 0) {
        char bip[64] = {0}, time_str[64] = {0};
        int num = sscanf(cmd + 7, "%63s %63s", bip, time_str);
        if (num >= 1) {
            time_t dur = num >= 2 ? parse_time_duration(time_str) : 0;
            execute_ban_ip(bip, dur);
            for (int k = 0; k < MAX_CLIENTS; k++) {
                if (clients[k].active && strcmp(clients[k].ip, bip) == 0) remove_client(k);
            }
            printf("banned ip '%s' for %lds\n", bip, (long)dur);
            fflush(stdout);
        }
    } else if (strncmp(cmd, "ban ", 4) == 0) {
        char ident[64] = {0}, time_str[64] = {0};
        int num = sscanf(cmd + 4, "%63s %63s", ident, time_str);
        if (num >= 1) {
            time_t dur = num >= 2 ? parse_time_duration(time_str) : 0;
            execute_ban_user(ident, dur);
            for (int k = 0; k < MAX_CLIENTS; k++) {
                if (clients[k].active && (strcmp(clients[k].name, ident) == 0 || strcmp(clients[k].uuid, ident) == 0)) {
                    remove_client(k);
                }
            }
            printf("banned user/uuid '%s' for %lds\n", ident, (long)dur);
            fflush(stdout);
        }
    } else if (strcmp(cmd, "config") == 0) {
        load_server_config();
        printf("server configuration:\n  max_failed_attempts = %d\n  autoban_duration_seconds = %d\n", server_max_failed_attempts, server_autoban_duration_seconds);
        fflush(stdout);
    } else if (strcmp(cmd, "blacklist") == 0) {
        load_server_blacklist();
        printf("blacklist reloaded (%d items)\n", server_blacklist_count);
        fflush(stdout);
    } else if (strcmp(cmd, "stop") == 0) {
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

static int server_broadcast_helper(const char *msg, int exclude_fd) {
    if (!msg) return 0;
    for (int c = 0; c < MAX_CLIENTS; c++) {
        if (clients[c].active && clients[c].fd != exclude_fd && clients[c].name[0] != '\0') {
            write(clients[c].fd, msg, strlen(msg));
        }
    }
    return 0;
}

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

        if (strncmp(line, "remote_register ", 16) == 0) {
            char rname[65] = {0};
            char rkey[256] = {0};
            if (sscanf(line + 16, "%64s %255s", rname, rkey) == 2) {
                if (execute_server_registration(rname, rkey) == 0) {
                    write(clients[idx].fd, "OK: registered\n", 15);
                } else {
                    write(clients[idx].fd, "ERR: registration failed\n", 25);
                }
            } else {
                write(clients[idx].fd, "ERR: invalid register args\n", 27);
            }
            remove_client(idx);
            return;
        }

        if (strncmp(line, "login ", 6) == 0) {
            char *args_str = line + 6;
            while (*args_str == ' ') args_str++;

            char name[65] = {0};
            char password[256] = {0};

            if (sscanf(args_str, "%64s %255s", name, password) == 2) {
                if (is_account_blacklisted(name, name, NULL)) {
                    write(clients[idx].fd, "ERR: blacklisted account\n", 25);
                    track_failed_auth(clients[idx].ip);
                    remove_client(idx);
                    return;
                }

                if (verify_and_resolve_account(name, password, clients[idx].name, clients[idx].uuid) == 0) {
                    if (is_user_banned(clients[idx].name, clients[idx].uuid)) {
                        write(clients[idx].fd, "ERR: you are banned\n", 20);
                        remove_client(idx);
                        return;
                    }
                    if (is_account_blacklisted(name, clients[idx].name, clients[idx].uuid)) {
                        write(clients[idx].fd, "ERR: blacklisted account\n", 25);
                        track_failed_auth(clients[idx].ip);
                        remove_client(idx);
                        return;
                    }

                    track_successful_auth(clients[idx].ip);

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
                    track_failed_auth(clients[idx].ip);
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
                fflush(stdout);
            }
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

        } else {
            if (clients[idx].name[0] != '\0') {
                plugin_engine_server_handle_packet(&plugin_sctx, clients[idx].fd, clients[idx].name, line);
            }
        }

        line = strtok_r(NULL, "\n", &saveptr);
    }
}

static void resolve_server_paths(void) {
    int in_sub = path_exists("../shared") || path_exists("../server") || path_exists("../client") || path_exists("../Makefile");
    if (in_sub) {
        strcpy(ACCOUNT_DIR, "../account");
    } else {
        strcpy(ACCOUNT_DIR, "account");
    }
}

/* ---------- main ---------- */

int main(int argc, char **argv) {
    resolve_server_paths();
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
    srand(time(NULL) ^ getpid());
    load_server_config();
    load_server_blacklist();
    load_persistent_bans();

    plugin_sctx.broadcast_msg = server_broadcast_helper;
    plugin_engine_server_init(&plugin_sctx);
    for (i = 0; i < MAX_CLIENTS; i++) {
        clients[i].active = 0;
        clients[i].fd = -1;
        memset(clients[i].name, 0, sizeof(clients[i].name));
        memset(clients[i].uuid, 0, sizeof(clients[i].uuid));
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
            char cli_ip_buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &cli_addr.sin_addr, cli_ip_buf, sizeof(cli_ip_buf));
            if (is_ip_banned(cli_ip_buf)) {
                close(new_fd);
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
                memset(clients[client_idx].uuid, 0, sizeof(clients[client_idx].uuid));
                clients[client_idx].id = -1;
                clients[client_idx].last_ping_sent = 0;
                clients[client_idx].ping_ms = -1;
                memcpy(&clients[client_idx].addr, &cli_addr, sizeof(cli_addr));
                inet_ntop(AF_INET, &cli_addr.sin_addr,
                          clients[client_idx].ip,
                          sizeof(clients[client_idx].ip));
            }
        }

        /* Handle client data */
        for (i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].active && FD_ISSET(clients[i].fd, &rfds)) {
                handle_client_data(i);
            }
        }
    }

    for (i = 0; i < MAX_CLIENTS; i++) remove_client(i);
    close(srv_fd);
    return 0;
}
