#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/file.h>  /* for flock */
#include <dirent.h>
#include <limits.h>
#include <time.h>
#include <fcntl.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define ACCOUNT_DIR    "account"
#define SESSION_DIR    "sessions"
#define GENERATOR_BIN  "./generator"
#define MAX_LINE       4096
#define MAX_NAME       32
#define SESSION_ID_LEN 32
#define SESSION_TIMEOUT 1800  /* 30 minutes */

/* ---------- utils ---------- */

static void trim_nl(char *s) {
    size_t n = strlen(s);
    while (n && (*(s + n - 1) == '\n' || *(s + n - 1) == '\r')) {
        --n;
        *(s + n) = 0;
    }
}

static int is_valid_name(const char *s) {
    size_t n = strlen(s);
    size_t i;
    if (n == 0 || n > MAX_NAME) return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)*(s + i);
        if (!(isalnum(c) || c == '_' || c == '-' || c == '.')) return 0;
    }
    return 1;
}

static int is_valid_key(const char *s) {
    size_t n;
    size_t i;
    if (!s) return 0;
    n = strlen(s);
    if (n == 0) return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)*(s + i);
        if (c >= 'a' && c <= 'z') continue;
        if (c >= 'A' && c <= 'Z') continue;
        if (c >= '0' && c <= '9') continue;
        if (c == '+' || c == '=' || c == '_' || c == '-' || c == '*') continue;
        return 0;
    }
    return 1;
}

static int is_valid_session_id(const char *s) {
    size_t n;
    size_t i;
    if (!s) return 0;
    n = strlen(s);
    if (n != SESSION_ID_LEN) return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)*(s + i);
        if (c >= 'a' && c <= 'f') continue;
        if (c >= '0' && c <= '9') continue;
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

static int mkdir_p(const char *path) {
    char *tmp;
    size_t n;
    char *p;
    int res = 1;

    if (!path || !*path) return 0;
    n = strlen(path);
    if (n >= PATH_MAX) return 0;

    tmp = malloc(n + 1);
    if (!tmp) return 0;
    memcpy(tmp, path, n + 1);

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0700) != 0 && errno != EEXIST) {
                res = 0;
                break;
            }
            *p = '/';
        }
    }
    if (res && mkdir(tmp, 0700) != 0 && errno != EEXIST) res = 0;

    free(tmp);
    return res;
}

static int rmdir_r(const char *path) {
    DIR *d;
    struct dirent *ent;
    char *child;
    struct stat st;

    d = opendir(path);
    if (!d) return 0;

    child = malloc(PATH_MAX);
    if (!child) {
        closedir(d);
        return 0;
    }

    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        snprintf(child, PATH_MAX, "%s/%s", path, ent->d_name);

        if (stat(child, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                rmdir_r(child);
            } else {
                unlink(child);
            }
        }
    }

    closedir(d);
    free(child);
    rmdir(path);
    return 1;
}

static int join2(char *out, size_t outsz, const char *a, const char *b) {
    size_t na, nb;
    if (!a || !b || !out) return 0;
    na = strlen(a);
    nb = strlen(b);
    if (na + 1 + nb + 1 > outsz) return 0;
    memcpy(out, a, na);
    *(out + na) = '/';
    memcpy(out + na + 1, b, nb + 1);
    return 1;
}

/* ---------- file locking wrappers ---------- */

static int read_file_long_locked(const char *path, long *out) {
    int fd;
    char buf[64];
    ssize_t r;
    int ret = 0;

    fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    /* Shared lock (LOCK_SH) - allows multiple readers */
    if (flock(fd, LOCK_SH) == 0) {
        r = read(fd, buf, sizeof(buf) - 1);
        if (r > 0) {
            buf[r] = '\0';
            trim_nl(buf);
            *out = atol(buf);
            ret = 1;
        }
        flock(fd, LOCK_UN);
    }

    close(fd);
    return ret;
}

static int write_file_long_locked(const char *path, long value) {
    int fd;
    char buf[64];
    int len;
    int ret = 0;

    len = snprintf(buf, sizeof(buf), "%ld\n", value);

    /* O_WRONLY|O_CREAT|O_TRUNC creates file if needed, clears it if exists */
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;

    /* Exclusive lock (LOCK_EX) - only one writer at a time */
    if (flock(fd, LOCK_EX) == 0) {
        if (write(fd, buf, len) == len) {
            ret = 1;
        }
        flock(fd, LOCK_UN);
    }

    close(fd);
    return ret;
}

static int read_file_line_locked(const char *path, char *buf, size_t bufsz) {
    int fd;
    ssize_t r;
    int ret = 0;

    fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    if (flock(fd, LOCK_SH) == 0) {
        r = read(fd, buf, bufsz - 1);
        if (r > 0) {
            buf[r] = '\0';
            trim_nl(buf);
            ret = 1;
        }
        flock(fd, LOCK_UN);
    }

    close(fd);
    return ret;
}

static int write_file_line_locked(const char *path, const char *content) {
    int fd;
    int len;
    int ret = 0;

    len = strlen(content);

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;

    if (flock(fd, LOCK_EX) == 0) {
        if (write(fd, content, len) == len) {
            /* add newline if not present */
            if (*(content + len - 1) != '\n') {
                write(fd, "\n", 1);
            }
            ret = 1;
        }
        flock(fd, LOCK_UN);
    }

    close(fd);
    return ret;
}

/* ---------- time ---------- */

static long current_time(void) {
    return (long)time(NULL);
}

/* ---------- random session id ---------- */

static int generate_session_id(char *out) {
    int fd;
    unsigned char bytes[SESSION_ID_LEN / 2];
    ssize_t r;
    size_t i;
    const char *hex = "0123456789abcdef";

    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return 0;

    r = read(fd, bytes, sizeof(bytes));
    close(fd);

    if (r != (ssize_t)sizeof(bytes)) return 0;

    for (i = 0; i < sizeof(bytes); i++) {
        *(out + i * 2) = *(hex + (*(bytes + i) >> 4));
        *(out + i * 2 + 1) = *(hex + (*(bytes + i) & 0x0F));
    }
    *(out + SESSION_ID_LEN) = '\0';

    return 1;
}

/* ---------- generator ---------- */

static char *run_generator_cmd(const char *cmd) {
    FILE *fp;
    char *result;

    if (!cmd) return NULL;
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

static int run_generator_exit(const char *cmd) {
    int ret = system(cmd);
    if (ret == -1) return 1;
    return WEXITSTATUS(ret);
}

static int generator_verify_key(const char *stored, const char *key) {
    char cmd[MAX_LINE];
    snprintf(cmd, MAX_LINE, "%s verify '%s' '%s'", GENERATOR_BIN, stored, key);
    return run_generator_exit(cmd);
}

/* ---------- account resolve ---------- */

static int resolve_uuid_dir(const char *identifier, char *uuid_dir_out, char *name_out) {
    DIR *d;
    struct dirent *ent;
    char *check_dir;
    char *name_file;
    char *stored_name;
    int found = 0;

    if (!identifier) return 0;

    check_dir = malloc(PATH_MAX);
    name_file = malloc(PATH_MAX);
    stored_name = malloc(MAX_LINE);

    if (!check_dir || !name_file || !stored_name) {
        free(check_dir);
        free(name_file);
        free(stored_name);
        return 0;
    }

    d = opendir(ACCOUNT_DIR);
    if (!d) {
        free(check_dir);
        free(name_file);
        free(stored_name);
        return 0;
    }

    while ((ent = readdir(d)) != NULL) {
        if (*(ent->d_name) == '.') continue;
        if (!join2(check_dir, PATH_MAX, ACCOUNT_DIR, ent->d_name)) continue;
        if (!is_directory(check_dir)) continue;
        if (!join2(name_file, PATH_MAX, check_dir, "name")) continue;
        if (!read_file_line_locked(name_file, stored_name, MAX_LINE)) continue;

        if (strcmp(stored_name, identifier) == 0 ||
            strcmp(ent->d_name, identifier) == 0) {
            if (uuid_dir_out) {
                size_t len = strlen(check_dir);
                memcpy(uuid_dir_out, check_dir, len + 1);
            }
            if (name_out) {
                size_t len = strlen(stored_name);
                memcpy(name_out, stored_name, len + 1);
            }
            found = 1;
        break;
            }
    }
    closedir(d);

    free(check_dir);
    free(name_file);
    free(stored_name);
    return found;
}

static int resolve_plain_dir(const char *name, char *dir_out) {
    if (!name || !dir_out) return 0;
    if (!join2(dir_out, PATH_MAX, ACCOUNT_DIR, name)) return 0;
    return is_directory(dir_out);
}

/* ---------- auth ---------- */

static int auth_plain(const char *identifier, const char *key) {
    char *user_dir;
    char *key_file;
    int res = 1;

    user_dir = malloc(PATH_MAX);
    key_file = malloc(PATH_MAX);
    if (!user_dir || !key_file) {
        free(user_dir);
        free(key_file);
        return 1;
    }

    if (!join2(user_dir, PATH_MAX, ACCOUNT_DIR, identifier)) goto done;
    if (!is_directory(user_dir)) goto done;
    if (!join2(key_file, PATH_MAX, user_dir, key)) goto done;
    if (path_exists(key_file)) res = 0;

    done:
    free(user_dir);
    free(key_file);
    return res;
}

static int auth_secure(const char *identifier, const char *key) {
    char *uuid_dir;
    char *key_file;
    char *stored_key;
    int res = 1;

    uuid_dir = malloc(PATH_MAX);
    key_file = malloc(PATH_MAX);
    stored_key = malloc(MAX_LINE);

    if (!uuid_dir || !key_file || !stored_key) {
        free(uuid_dir);
        free(key_file);
        free(stored_key);
        return 1;
    }

    if (!resolve_uuid_dir(identifier, uuid_dir, NULL)) goto done;
    if (!join2(key_file, PATH_MAX, uuid_dir, "key")) goto done;
    if (!read_file_line_locked(key_file, stored_key, MAX_LINE)) goto done;

    if (generator_verify_key(stored_key, key) == 0) res = 0;

    done:
    free(uuid_dir);
    free(key_file);
    free(stored_key);
    return res;
}

static int auth(const char *identifier, const char *key, char *resolved_name) {
    char *name_buf;

    if (!identifier || !key) return 1;
    if (!is_valid_name(identifier)) return 1;

    if (auth_plain(identifier, key) == 0) {
        if (resolved_name) {
            size_t len = strlen(identifier);
            memcpy(resolved_name, identifier, len + 1);
        }
        return 0;
    }

    if (generator_available()) {
        name_buf = malloc(MAX_LINE);
        if (name_buf) {
            if (resolve_uuid_dir(identifier, NULL, name_buf)) {
                if (auth_secure(identifier, key) == 0) {
                    if (resolved_name) {
                        size_t len = strlen(name_buf);
                        memcpy(resolved_name, name_buf, len + 1);
                    }
                    free(name_buf);
                    return 0;
                }
            }
            free(name_buf);
        }
    }

    return 1;
}

/* ---------- session management ---------- */

static int session_path(const char *session_id, const char *file, char *out) {
    char sess_dir[PATH_MAX];
    if (!join2(sess_dir, PATH_MAX, SESSION_DIR, session_id)) return 0;
    if (file) {
        return join2(out, PATH_MAX, sess_dir, file);
    } else {
        size_t len = strlen(sess_dir);
        memcpy(out, sess_dir, len + 1);
        return 1;
    }
}

static int session_exists(const char *session_id) {
    char path[PATH_MAX];
    if (!session_path(session_id, NULL, path)) return 0;
    return is_directory(path);
}

static int session_is_expired(const char *session_id) {
    char path[PATH_MAX];
    long active;

    if (!session_path(session_id, "active", path)) return 1;

    /* No need to lock for simple check, read is atomic enough on local FS for basic check */
    if (!read_file_long_locked(path, &active)) return 1;
    if (active < 0) return 1;

    return ((long)time(NULL) - active) > SESSION_TIMEOUT;
}

static int session_create(const char *username, char *session_id_out) {
    char session_id[SESSION_ID_LEN + 1];
    char path[PATH_MAX];
    long now;

    if (!generate_session_id(session_id)) return 1;
    if (!mkdir_p(SESSION_DIR)) return 1;

    if (!session_path(session_id, NULL, path)) return 1;

    /* Atomic directory creation */
    if (mkdir(path, 0700) != 0) return 1;

    now = current_time();

    if (!session_path(session_id, "user", path)) return 1;
    if (!write_file_line_locked(path, username)) return 1;

    if (!session_path(session_id, "created", path)) return 1;
    if (!write_file_long_locked(path, now)) return 1;

    if (!session_path(session_id, "active", path)) return 1;
    if (!write_file_long_locked(path, now)) return 1;

    if (!session_path(session_id, "pid", path)) return 1;
    if (!write_file_long_locked(path, (long)getpid())) return 1;

    memcpy(session_id_out, session_id, SESSION_ID_LEN + 1);
    return 0;
}

static int session_destroy(const char *session_id) {
    char path[PATH_MAX];
    if (!is_valid_session_id(session_id)) return 1;
    if (!session_path(session_id, NULL, path)) return 1;
    if (!is_directory(path)) return 1;
    return rmdir_r(path) ? 0 : 1;
}

static int session_touch(const char *session_id) {
    char path[PATH_MAX];
    if (!is_valid_session_id(session_id)) return 1;
    if (!session_exists(session_id)) return 1;
    if (session_is_expired(session_id)) return 1;

    if (!session_path(session_id, "active", path)) return 1;
    return write_file_long_locked(path, current_time()) ? 0 : 1;
}

static int session_get_user(const char *session_id, char *username_out) {
    char path[PATH_MAX];
    if (!is_valid_session_id(session_id)) return 1;
    if (!session_exists(session_id)) return 1;

    if (!session_path(session_id, "user", path)) return 1;
    return read_file_line_locked(path, username_out, MAX_NAME + 1) ? 0 : 1;
}

/* ---------- commands ---------- */

static int cmd_login(const char *name, const char *key) {
    char resolved_name[MAX_NAME + 1];
    char session_id[SESSION_ID_LEN + 1];

    if (!name || !key) return 1;
    if (!is_valid_key(key)) return 1;

    if (auth(name, key, resolved_name) != 0) {
        return 1;
    }

    if (session_create(resolved_name, session_id) != 0) {
        return 1;
    }

    printf("%s\n", session_id);
    return 0;
}

static int cmd_logout(const char *session_id) {
    if (!session_id) return 1;
    return session_destroy(session_id);
}

static int cmd_check(const char *session_id) {
    if (!session_id) return 1;
    if (!is_valid_session_id(session_id)) return 1;
    if (!session_exists(session_id)) return 1;
    if (session_is_expired(session_id)) return 1;
    return 0;
}

static int cmd_touch(const char *session_id) {
    if (!session_id) return 1;
    return session_touch(session_id);
}

static int cmd_info(const char *session_id) {
    char username[MAX_NAME + 1];
    char path[PATH_MAX];
    long created, active, now, uptime, idle;

    if (!session_id) return 1;
    if (!is_valid_session_id(session_id)) return 1;
    if (!session_exists(session_id)) return 1;

    if (session_get_user(session_id, username) != 0) return 1;

    if (!session_path(session_id, "created", path)) return 1;
    if (!read_file_long_locked(path, &created)) return 1;

    if (!session_path(session_id, "active", path)) return 1;
    if (!read_file_long_locked(path, &active)) return 1;

    now = current_time();

    if (created < 0 || active < 0) return 1;

    uptime = now - created;
    idle = now - active;

    printf("user:%s\n", username);
    printf("created:%ld\n", created);
    printf("uptime:%ld\n", uptime);
    printf("idle:%ld\n", idle);
    printf("expired:%d\n", idle > SESSION_TIMEOUT ? 1 : 0);

    return 0;
}

static int cmd_whoami(const char *session_id) {
    char username[MAX_NAME + 1];

    if (!session_id) return 1;
    if (cmd_check(session_id) != 0) return 1;
    if (session_get_user(session_id, username) != 0) return 1;

    printf("%s\n", username);
    return 0;
}

static int cmd_list(void) {
    DIR *d;
    struct dirent *ent;
    char username[MAX_NAME + 1];
    char path[PATH_MAX];
    long created, active;
    long now;
    int count = 0;

    d = opendir(SESSION_DIR);
    if (!d) {
        printf("0 sessions\n");
        return 0;
    }

    now = current_time();

    while ((ent = readdir(d)) != NULL) {
        if (*(ent->d_name) == '.') continue;
        if (!is_valid_session_id(ent->d_name)) continue;

        if (session_get_user(ent->d_name, username) != 0) continue;

        if (!session_path(ent->d_name, "created", path)) continue;
        if (!read_file_long_locked(path, &created)) continue;

        if (!session_path(ent->d_name, "active", path)) continue;
        if (!read_file_long_locked(path, &active)) continue;

        if (created < 0 || active < 0) continue;

        printf("%s %s uptime:%ld idle:%ld%s\n",
               ent->d_name, username,
               now - created, now - active,
               (now - active > SESSION_TIMEOUT) ? " [expired]" : "");
        count++;
    }

    closedir(d);
    printf("%d session(s)\n", count);
    return 0;
}

static int cmd_cleanup(void) {
    DIR *d;
    struct dirent *ent;
    char path[PATH_MAX];
    int removed = 0;

    d = opendir(SESSION_DIR);
    if (!d) return 0;

    while ((ent = readdir(d)) != NULL) {
        if (*(ent->d_name) == '.') continue;
        if (!is_valid_session_id(ent->d_name)) continue;

        if (session_is_expired(ent->d_name)) {
            if (session_path(ent->d_name, NULL, path)) {
                rmdir_r(path);
                removed++;
            }
        }
    }

    closedir(d);
    printf("%d session(s) removed\n", removed);
    return 0;
}

/* ---------- main ---------- */

static void print_usage(void) {
    fprintf(stderr, "usage:\n");
    fprintf(stderr, "  session login <name> <key>\n");
    fprintf(stderr, "  session logout <session_id>\n");
    fprintf(stderr, "  session check <session_id>\n");
    fprintf(stderr, "  session touch <session_id>\n");
    fprintf(stderr, "  session info <session_id>\n");
    fprintf(stderr, "  session whoami <session_id>\n");
    fprintf(stderr, "  session list\n");
    fprintf(stderr, "  session cleanup\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (strcmp(*(argv + 1), "login") == 0) {
        if (argc != 4) { print_usage(); return 1; }
        return cmd_login(*(argv + 2), *(argv + 3));
    }

    if (strcmp(*(argv + 1), "logout") == 0) {
        if (argc != 3) { print_usage(); return 1; }
        return cmd_logout(*(argv + 2));
    }

    if (strcmp(*(argv + 1), "check") == 0) {
        if (argc != 3) { print_usage(); return 1; }
        return cmd_check(*(argv + 2));
    }

    if (strcmp(*(argv + 1), "touch") == 0) {
        if (argc != 3) { print_usage(); return 1; }
        return cmd_touch(*(argv + 2));
    }

    if (strcmp(*(argv + 1), "info") == 0) {
        if (argc != 3) { print_usage(); return 1; }
        return cmd_info(*(argv + 2));
    }

    if (strcmp(*(argv + 1), "whoami") == 0) {
        if (argc != 3) { print_usage(); return 1; }
        return cmd_whoami(*(argv + 2));
    }

    if (strcmp(*(argv + 1), "list") == 0) {
        return cmd_list();
    }

    if (strcmp(*(argv + 1), "cleanup") == 0) {
        return cmd_cleanup();
    }

    print_usage();
    return 1;
}
