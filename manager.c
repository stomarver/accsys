#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define BASE_DIR       "account"
#define GENERATOR_BIN  "./generator"
#define MAX_LINE       4096
#define MAX_NAME       32

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

static int read_file_line(const char *path, char *buf, size_t bufsz) {
    FILE *f;
    if (!path || !buf || bufsz == 0) return 0;
    f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(buf, (int)bufsz, f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    trim_nl(buf);
    return 1;
}

static int write_file_line(const char *path, const char *content) {
    FILE *f;
    if (!path || !content) return 0;
    f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "%s\n", content);
    fclose(f);
    return 1;
}

/* ---------- generator ---------- */


static char *run_generator_cmd(char *const args[]) {
    int link[2];
    pid_t pid;
    char *result = NULL;

    if (pipe(link) == -1) return NULL;

    if ((pid = fork()) == -1) {
        close(link[0]); close(link[1]);
        return NULL;
    }

    if (pid == 0) {
        dup2(link[1], STDOUT_FILENO);
        close(link[0]); close(link[1]);
        execv(args[0], args);
        exit(1);
    } else {
        close(link[1]);
        result = malloc(MAX_LINE);
        if (result) {
            FILE *fp = fdopen(link[0], "r");
            if (fp) {
                if (!fgets(result, MAX_LINE, fp)) {
                    free(result); result = NULL;
                }
                fclose(fp);
            } else { free(result); result = NULL; close(link[0]); }
        } else { close(link[0]); }
        waitpid(pid, NULL, 0);
        if (result) trim_nl(result);
        return result;
    }
}

static int run_generator_exit(char *const args[]) {
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

static char *generator_uuid(void) {
    char *args[] = { GENERATOR_BIN, "uuid", NULL };
    return run_generator_cmd(args);
}

static char *generator_encrypt_key(const char *key) {
    if (!key) return NULL;
    char *args[] = { GENERATOR_BIN, "encrypt", (char *)key, NULL };
    return run_generator_cmd(args);
}

static int generator_verify_key(const char *stored, const char *key) {
    char *args[] = { GENERATOR_BIN, "verify", (char *)stored, (char *)key, NULL };
    return run_generator_exit(args);
}

/* ---------- resolve ---------- */

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

    d = opendir(BASE_DIR);
    if (!d) {
        free(check_dir);
        free(name_file);
        free(stored_name);
        return 0;
    }

    while ((ent = readdir(d)) != NULL) {
        if (*(ent->d_name) == '.') continue;
        if (!join2(check_dir, PATH_MAX, BASE_DIR, ent->d_name)) continue;
        if (!is_directory(check_dir)) continue;
        if (!join2(name_file, PATH_MAX, check_dir, "name")) continue;
        if (!read_file_line(name_file, stored_name, MAX_LINE)) continue;

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
    if (!join2(dir_out, PATH_MAX, BASE_DIR, name)) return 0;
    return is_directory(dir_out);
}

/* ---------- internal auth ---------- */

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

    if (!join2(user_dir, PATH_MAX, BASE_DIR, identifier)) goto done;
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
    if (!read_file_line(key_file, stored_key, MAX_LINE)) goto done;

    /* generator verify returns 0 on match */
    if (generator_verify_key(stored_key, key) == 0) res = 0;

    done:
    free(uuid_dir);
    free(key_file);
    free(stored_key);
    return res;
}

static int auth(const char *identifier, const char *key) {
    if (!identifier || !key) return 1;
    if (!is_valid_name(identifier)) return 1;

    if (auth_plain(identifier, key) == 0) return 0;

    if (generator_available()) {
        if (auth_secure(identifier, key) == 0) return 0;
    }

    return 1;
}

/* ---------- register ---------- */

static int cmd_register_plain(const char *name, const char *key) {
    char *user_dir;
    char *key_file;
    FILE *f;
    int res = 1;

    user_dir = malloc(PATH_MAX);
    key_file = malloc(PATH_MAX);
    if (!user_dir || !key_file) {
        free(user_dir);
        free(key_file);
        return 1;
    }

    if (!join2(user_dir, PATH_MAX, BASE_DIR, name)) goto done;
    if (path_exists(user_dir)) goto done;
    if (!mkdir_p(user_dir)) goto done;
    if (!join2(key_file, PATH_MAX, user_dir, key)) goto done;

    f = fopen(key_file, "w");
    if (!f) goto done;
    fclose(f);
    res = 0;

    done:
    free(user_dir);
    free(key_file);
    return res;
}

static int cmd_register_generator(const char *name, const char *key) {
    char *uuid = NULL;
    char *enc_key = NULL;
    char *uuid_dir;
    char *name_file;
    char *key_file;
    char *existing_dir;
    int res = 1;

    uuid_dir = malloc(PATH_MAX);
    name_file = malloc(PATH_MAX);
    key_file = malloc(PATH_MAX);
    existing_dir = malloc(PATH_MAX);

    if (!uuid_dir || !name_file || !key_file || !existing_dir) {
        free(uuid_dir);
        free(name_file);
        free(key_file);
        free(existing_dir);
        return 1;
    }

    uuid = generator_uuid();
    if (!uuid) goto done;

    enc_key = generator_encrypt_key(key);
    if (!enc_key) goto done;

    if (resolve_uuid_dir(name, existing_dir, NULL)) goto done;
    if (resolve_plain_dir(name, existing_dir)) goto done;

    if (!join2(uuid_dir, PATH_MAX, BASE_DIR, uuid)) goto done;
    if (!mkdir_p(uuid_dir)) goto done;

    if (!join2(name_file, PATH_MAX, uuid_dir, "name")) goto done;
    if (!write_file_line(name_file, name)) goto done;

    if (!join2(key_file, PATH_MAX, uuid_dir, "key")) goto done;
    if (!write_file_line(key_file, enc_key)) goto done;

    res = 0;

    done:
    free(uuid);
    free(enc_key);
    free(uuid_dir);
    free(name_file);
    free(key_file);
    free(existing_dir);
    return res;
}

static int cmd_register(const char *name, const char *key) {
    if (!name || !key) return 1;
    if (!is_valid_name(name)) return 1;
    if (!is_valid_key(key)) return 1;
    if (!mkdir_p(BASE_DIR)) return 1;

    if (generator_available())
        return cmd_register_generator(name, key);
    return cmd_register_plain(name, key);
}

/* ---------- rename ---------- */

static int cmd_rename_plain(const char *identifier, const char *new_name) {
    char *old_dir;
    char *new_dir;
    int res = 1;

    old_dir = malloc(PATH_MAX);
    new_dir = malloc(PATH_MAX);
    if (!old_dir || !new_dir) {
        free(old_dir);
        free(new_dir);
        return 1;
    }

    if (!join2(old_dir, PATH_MAX, BASE_DIR, identifier)) goto done;
    if (!is_directory(old_dir)) goto done;
    if (!join2(new_dir, PATH_MAX, BASE_DIR, new_name)) goto done;
    if (path_exists(new_dir)) goto done;
    if (rename(old_dir, new_dir) == 0) res = 0;

    done:
    free(old_dir);
    free(new_dir);
    return res;
}

static int cmd_rename_secure(const char *identifier, const char *new_name) {
    char *uuid_dir;
    char *name_file;
    char *check_dir;
    int res = 1;

    uuid_dir = malloc(PATH_MAX);
    name_file = malloc(PATH_MAX);
    check_dir = malloc(PATH_MAX);

    if (!uuid_dir || !name_file || !check_dir) {
        free(uuid_dir);
        free(name_file);
        free(check_dir);
        return 1;
    }

    if (!resolve_uuid_dir(identifier, uuid_dir, NULL)) goto done;
    if (resolve_uuid_dir(new_name, check_dir, NULL)) goto done;
    if (resolve_plain_dir(new_name, check_dir)) goto done;
    if (!join2(name_file, PATH_MAX, uuid_dir, "name")) goto done;
    if (!write_file_line(name_file, new_name)) goto done;
    res = 0;

    done:
    free(uuid_dir);
    free(name_file);
    free(check_dir);
    return res;
}

static int cmd_rename(const char *identifier, const char *key, const char *new_name) {
    if (!identifier || !key || !new_name) return 1;
    if (!is_valid_name(new_name)) return 1;
    if (!is_valid_key(key)) return 1;
    if (auth(identifier, key) != 0) return 1;

    if (generator_available()) {
        if (cmd_rename_secure(identifier, new_name) == 0) return 0;
    }

    return cmd_rename_plain(identifier, new_name);
}

/* ---------- rekey ---------- */

static int cmd_rekey_plain(const char *identifier, const char *old_key, const char *new_key) {
    char *user_dir;
    char *old_file;
    char *new_file;
    FILE *f;
    int res = 1;

    user_dir = malloc(PATH_MAX);
    old_file = malloc(PATH_MAX);
    new_file = malloc(PATH_MAX);

    if (!user_dir || !old_file || !new_file) {
        free(user_dir);
        free(old_file);
        free(new_file);
        return 1;
    }

    if (!join2(user_dir, PATH_MAX, BASE_DIR, identifier)) goto done;
    if (!is_directory(user_dir)) goto done;
    if (!join2(old_file, PATH_MAX, user_dir, old_key)) goto done;
    if (!path_exists(old_file)) goto done;
    if (!join2(new_file, PATH_MAX, user_dir, new_key)) goto done;

    f = fopen(new_file, "w");
    if (!f) goto done;
    fclose(f);

    unlink(old_file);
    res = 0;

    done:
    free(user_dir);
    free(old_file);
    free(new_file);
    return res;
}

static int cmd_rekey_secure(const char *identifier, const char *new_key) {
    char *uuid_dir;
    char *key_file;
    char *enc_key = NULL;
    int res = 1;

    uuid_dir = malloc(PATH_MAX);
    key_file = malloc(PATH_MAX);

    if (!uuid_dir || !key_file) {
        free(uuid_dir);
        free(key_file);
        return 1;
    }

    if (!resolve_uuid_dir(identifier, uuid_dir, NULL)) goto done;

    enc_key = generator_encrypt_key(new_key);
    if (!enc_key) goto done;

    if (!join2(key_file, PATH_MAX, uuid_dir, "key")) goto done;
    if (!write_file_line(key_file, enc_key)) goto done;
    res = 0;

    done:
    free(enc_key);
    free(uuid_dir);
    free(key_file);
    return res;
}

static int cmd_rekey(const char *identifier, const char *old_key, const char *new_key) {
    if (!identifier || !old_key || !new_key) return 1;
    if (!is_valid_key(old_key)) return 1;
    if (!is_valid_key(new_key)) return 1;
    if (auth(identifier, old_key) != 0) return 1;

    if (generator_available()) {
        if (cmd_rekey_secure(identifier, new_key) == 0) return 0;
    }

    return cmd_rekey_plain(identifier, old_key, new_key);
}

/* ---------- main ---------- */

static void print_usage(void) {
    fprintf(stderr, "usage:\n");
    fprintf(stderr, "  manager register <name> <key>\n");
    fprintf(stderr, "  manager rename <name> <key> <new_name>\n");
    fprintf(stderr, "  manager rekey <name> <old_key> <new_key>\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (strcmp(*(argv + 1), "register") == 0) {
        if (argc != 4) {
            print_usage();
            return 1;
        }
        return cmd_register(*(argv + 2), *(argv + 3));
    }

    if (strcmp(*(argv + 1), "rename") == 0) {
        if (argc != 5) {
            print_usage();
            return 1;
        }
        return cmd_rename(*(argv + 2), *(argv + 3), *(argv + 4));
    }

    if (strcmp(*(argv + 1), "rekey") == 0) {
        if (argc != 5) {
            print_usage();
            return 1;
        }
        return cmd_rekey(*(argv + 2), *(argv + 3), *(argv + 4));
    }

    print_usage();
    return 1;
}
